#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>

#include "Render.h"
#include "World.h"
#include <iostream>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <iostream>

using namespace std;
using namespace Gdiplus;

namespace {
    constexpr int target_map_size = 1024;
    constexpr int minimum_tile_size = 4;
    constexpr DWORD window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const wchar_t* window_class_name = L"MyWorldMapWindow";

    class GdiplusRuntime {
    public:
        GdiplusRuntime() {
            GdiplusStartupInput startup_input;
            status_ = GdiplusStartup(&token_, &startup_input, nullptr);
        }

        ~GdiplusRuntime() {
            if (status_ == Ok)
                GdiplusShutdown(token_);
        }

        bool is_ready() const { return status_ == Ok; }

    private:
        ULONG_PTR token_{};
        Status status_{ GenericError };
    };

    GdiplusRuntime& gdiplus_runtime() {
        static GdiplusRuntime runtime;
        return runtime;
    }

    struct WindowContext {
        const Render* render;
        const World* world;
        vector<unsigned char> tile_transforms;
        int base_tile_size;
        float zoom_scale;
        float minimum_zoom;
        float maximum_zoom;
        float camera_x = 0.0f;
        float camera_y = 0.0f;
        vector<uint32_t> frame_pixels;
        bool show_continents = false;
        bool show_humidity = false;
        bool show_heat = false;
    };

    using MapRegenerationCallback = bool (*)(void*);
    MapRegenerationCallback map_regeneration_callback = nullptr;
    void* map_regeneration_user_data = nullptr;

    struct TextureMipLevel {
        UINT width = 0;
        UINT height = 0;
        vector<uint32_t> pixels;   // premultiplied RGB
        vector<unsigned char> alpha;
    };

    struct TileTexture {
        UINT width = 0;
        UINT height = 0;
        vector<uint32_t> pixels;   // premultiplied RGB
        vector<unsigned char> alpha;
        vector<TextureMipLevel> mipmaps; // уровни 1/2, 1/4, 1/8... исходной текстуры
    };

    // Создаёт mip-цепочку из уже загруженной premultiplied RGBA текстуры.
    // Уменьшение выполняется усреднением 2x2 с корректной обработкой краёв.
    void build_mipmaps(TileTexture& texture) {
        if (texture.width == 0 || texture.height == 0 ||
            texture.pixels.empty() || texture.alpha.size() != texture.pixels.size())
            return;

        TextureMipLevel previous{
            texture.width, texture.height, texture.pixels, texture.alpha
        };

        while (previous.width > 1 || previous.height > 1) {
            TextureMipLevel next;
            next.width = max(1u, previous.width / 2u);
            next.height = max(1u, previous.height / 2u);
            const size_t count = static_cast<size_t>(next.width) * next.height;
            next.pixels.resize(count);
            next.alpha.resize(count);

            for (UINT y = 0; y < next.height; ++y) {
                for (UINT x = 0; x < next.width; ++x) {
                    uint32_t red = 0, green = 0, blue = 0, alpha = 0, samples = 0;
                    for (UINT dy = 0; dy < 2; ++dy) {
                        for (UINT dx = 0; dx < 2; ++dx) {
                            const UINT sx = min(previous.width - 1u, x * 2u + dx);
                            const UINT sy = min(previous.height - 1u, y * 2u + dy);
                            const size_t index = static_cast<size_t>(sy) * previous.width + sx;
                            const uint32_t color = previous.pixels[index];
                            red += (color >> 16) & 0xffu;
                            green += (color >> 8) & 0xffu;
                            blue += color & 0xffu;
                            alpha += previous.alpha[index];
                            ++samples;
                        }
                    }

                    const size_t destination = static_cast<size_t>(y) * next.width + x;
                    next.pixels[destination] =
                        (((red + samples / 2u) / samples) << 16) |
                        (((green + samples / 2u) / samples) << 8) |
                        ((blue + samples / 2u) / samples);
                    next.alpha[destination] =
                        static_cast<unsigned char>((alpha + samples / 2u) / samples);
                }
            }

            texture.mipmaps.push_back(next);
            previous = move(next);
        }
    }

    // Стандартное source-over смешивание для одного канала:
    //   out = src * a + dst * (1 - a)
    // в целочисленной форме с округлением.
    constexpr uint32_t blend_channel(uint32_t src, uint32_t dst, uint32_t alpha) {
        return (src * alpha + dst * (255u - alpha) + 127u) / 255u;
    }

    // === Быстрая загрузка текстуры через LockBits ===
    // Раньше на каждый пиксель вызывался Bitmap::GetPixel (~80 нс / вызов).
    // С LockBits читаем сырой буфер напрямую (единицы нс / пиксель) — на порядки быстрее.
    TileTexture load_texture(const filesystem::path& path) {
        TileTexture texture;

        auto bitmap = make_unique<Bitmap>(path.c_str());
        if (bitmap->GetLastStatus() != Ok)
            return texture;

        texture.width = bitmap->GetWidth();
        texture.height = bitmap->GetHeight();
        if (texture.width == 0 || texture.height == 0)
            return texture;

        const size_t pixel_count = static_cast<size_t>(texture.width) * texture.height;
        texture.pixels.resize(pixel_count);
        texture.alpha.resize(pixel_count);

        BitmapData data{};
        const Rect rect(0, 0, static_cast<INT>(texture.width), static_cast<INT>(texture.height));
        if (bitmap->LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok) {
            texture = {};
            return texture;
        }

        const LONG stride = data.Stride;
        const bool bottom_up = stride < 0;
        const BYTE* const base = static_cast<const BYTE*>(data.Scan0);

        for (UINT y = 0; y < texture.height; ++y) {
            const auto* row = reinterpret_cast<const DWORD*>(
                base + static_cast<ptrdiff_t>(y) * stride
                );
            const UINT store_y = bottom_up ? (texture.height - 1 - y) : y;
            const size_t row_offset = static_cast<size_t>(store_y) * texture.width;
            for (UINT x = 0; x < texture.width; ++x) {
                const DWORD argb = row[x];
                const uint32_t a = (argb >> 24) & 0xFFu;
                const uint32_t r = (argb >> 16) & 0xFFu;
                const uint32_t g = (argb >> 8) & 0xFFu;
                const uint32_t b = argb & 0xFFu;
                texture.pixels[row_offset + x] =
                    ((r * a / 255u) << 16) | ((g * a / 255u) << 8) | (b * a / 255u);
                texture.alpha[row_offset + x] = static_cast<unsigned char>(a);
            }
        }

        bitmap->UnlockBits(&data);
        return texture;
    }

    // Ищет папку ассетов сначала относительно рабочего каталога,
    // затем относительно директории исполняемого файла (три уровня вверх).
    filesystem::path resolve_asset_directory(const wchar_t* relative_path) {
        filesystem::path directory = filesystem::current_path() / relative_path;
        error_code error;
        if (!filesystem::exists(directory, error)) {
            vector<wchar_t> executable_path(32768);
            const DWORD length = GetModuleFileNameW(
                nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size())
            );
            if (length > 0 && length < executable_path.size()) {
                const filesystem::path project_directory = filesystem::path(
                    wstring(executable_path.data(), length)
                ).parent_path().parent_path().parent_path();
                directory = project_directory / relative_path;
            }
        }
        return directory;
    }

    vector<TileTexture>& tile_textures() {
        static vector<TileTexture> textures = [] {
            const array<const wchar_t*, 6> file_names = {
                L"deep_water.png",
                L"shallow_water.png",
                L"beach.png",
                L"plains.png",
                L"mountains.png",
                L"snow.png"
            };
            const filesystem::path directory = resolve_asset_directory(L"assets/textures/terrain");
            vector<TileTexture> loaded;
            loaded.reserve(file_names.size());
            for (const wchar_t* file_name : file_names)
                loaded.push_back(load_texture(directory / file_name));
            return loaded;
            }();
        return textures;
    }


    // Биомные текстуры обычной суши. Порядок соответствует ID в biome_map_:
    // 0 snow_plains, 1 snow_taiga, 2 taiga, 3 plains, 4 forest,
    // 5 dark_forest, 6 desert, 7 savanna, 8 jungle.
    vector<TileTexture>& biome_textures() {
        static vector<TileTexture> textures = [] {
            const array<const wchar_t*, 9> file_names = {
                L"snow_plains.png",
                L"snow_taiga.png",
                L"taiga.png",
                L"plains.png",
                L"forest.png",
                L"dark_forest.png",
                L"desert.png",
                L"savanna.png",
                L"jungle.png"
            };
            const filesystem::path directory = resolve_asset_directory(L"assets/textures/terrain");
            vector<TileTexture> loaded;
            loaded.reserve(file_names.size());
            for (const wchar_t* file_name : file_names)
                loaded.push_back(load_texture(directory / file_name));
            return loaded;
            }();
        return textures;
    }
    // Индексы совпадают с WorldGenerationConfig::tree_density/clumpiness:
    // 0..8 — биомы, 9 — пляж.
    using TreeTextureBank = array<vector<TileTexture>, 10>;

    TreeTextureBank& tree_textures() {
        static TreeTextureBank textures = [] {
            constexpr array<const wchar_t*, 10> folder_names = {
                L"snow_plains",
                L"snow_taiga",
                L"taiga",
                L"plains",
                L"forest",
                L"dark_forest",
                L"desert",
                L"savanna",
                L"jungle",
                L"beach"
            };

            TreeTextureBank loaded{};
            const filesystem::path root =
                resolve_asset_directory(L"assets/textures/tree");

            for (size_t biome_id = 0; biome_id < folder_names.size(); ++biome_id) {
                const filesystem::path directory = root / folder_names[biome_id];
                error_code error;

                if (!filesystem::is_directory(directory, error)) {
                    wcerr << L"[TREE TEXTURES] Missing folder: "
                        << folder_names[biome_id] << L'\n';
                    continue;
                }

                vector<filesystem::path> image_files;
                for (filesystem::directory_iterator it(directory, error), end;
                    !error && it != end;
                    it.increment(error)) {
                    error_code file_error;
                    if (!it->is_regular_file(file_error) || file_error)
                        continue;

                    wstring extension = it->path().extension().wstring();
                    transform(extension.begin(), extension.end(), extension.begin(),
                        [](wchar_t character) {
                            return static_cast<wchar_t>(towlower(character));
                        });

                    if (extension == L".png")
                        image_files.push_back(it->path());
                }

                sort(image_files.begin(), image_files.end(),
                    [](const filesystem::path& left, const filesystem::path& right) {
                        return left.filename().wstring() < right.filename().wstring();
                    });

                for (const filesystem::path& image_path : image_files) {
                    TileTexture texture = load_texture(image_path);
                    if (texture.width == 0 || texture.height == 0 ||
                        texture.pixels.empty() ||
                        texture.alpha.size() != texture.pixels.size()) {
                        cerr << "[TREE TEXTURES] Could not load: "
                            << image_path.filename().string() << '\n';
                        continue;
                    }

                    build_mipmaps(texture);
                    loaded[biome_id].push_back(move(texture));
                }

                wcout << "[TREE TEXTURES] " << folder_names[biome_id]
                    << ": " << loaded[biome_id].size()
                    << " variant(s) loaded" << '\n';
            }

            return loaded;
        }();

        return textures;
    }



    vector<TileTexture>& mineral_textures() {
        static vector<TileTexture> textures = [] {
            vector<TileTexture> loaded;
            const filesystem::path directory =
                resolve_asset_directory(L"assets/textures/minerals");
            // Порядок должен соответствовать MineralVariant (Rock, Copper, Iron, Gold, Platinum)
            const array<const wchar_t*, 5> file_names = {
                L"rock.png", L"copper.png", L"iron.png", L"gold.png", L"platinum.png"
            };
            for (const auto* file_name : file_names)
                loaded.push_back(load_texture(directory / file_name));
            return loaded;
            }();
        return textures;
    }

    int base_tile_size_for(const vector<vector<int>>& tile_map) {
        if (tile_map.empty() || tile_map.front().empty())
            return 0;

        const size_t longest_side = max(tile_map.size(), tile_map.front().size());
        int base_tile_size = minimum_tile_size;
        if (longest_side >= static_cast<size_t>(target_map_size))
            base_tile_size = minimum_tile_size;
        else
            base_tile_size = max(minimum_tile_size, target_map_size / static_cast<int>(longest_side));

        return base_tile_size;
    }

    void clamp_camera(WindowContext& context) {
        const auto& tile_map = context.world->get_tiled_map();
        const double map_pixel_width = tile_map.front().size() * context.base_tile_size * context.zoom_scale;
        const double map_pixel_height = tile_map.size() * context.base_tile_size * context.zoom_scale;

        if (map_pixel_width <= target_map_size)
            context.camera_x = static_cast<float>((map_pixel_width - target_map_size) / 2.0);
        else
            context.camera_x = clamp(
                context.camera_x,
                0.0f,
                static_cast<float>(map_pixel_width - target_map_size)
            );

        if (map_pixel_height <= target_map_size)
            context.camera_y = static_cast<float>((map_pixel_height - target_map_size) / 2.0);
        else
            context.camera_y = clamp(
                context.camera_y,
                0.0f,
                static_cast<float>(map_pixel_height - target_map_size)
            );
    }

    // После генерации карты обновляем случайные преобразования текстур,
    // масштаб и положение камеры (старые координаты больше не актуальны).
    void reset_map_view(WindowContext& context) {
        const auto& tile_map = context.world->get_tiled_map();
        context.base_tile_size = base_tile_size_for(tile_map);
        if (context.base_tile_size <= 0)
            return;

        const size_t map_width = tile_map.front().size();
        const size_t map_height = tile_map.size();
        const double base_map_width = static_cast<double>(map_width) * context.base_tile_size;
        const double base_map_height = static_cast<double>(map_height) * context.base_tile_size;

        context.minimum_zoom = static_cast<float>(min({
            1.0,
            target_map_size / base_map_width,
            target_map_size / base_map_height
            }));
        context.zoom_scale = context.minimum_zoom;
        context.camera_x = 0.0f;
        context.camera_y = 0.0f;

        random_device random_device_source;
        mt19937 random_engine(random_device_source());
        uniform_int_distribution<int> transform_distribution(0, 7);
        context.tile_transforms.clear();
        context.tile_transforms.reserve(map_width * map_height);
        for (size_t i = 0; i < map_width * map_height; ++i) {
            context.tile_transforms.push_back(
                static_cast<unsigned char>(transform_distribution(random_engine))
            );
        }

        clamp_camera(context);
    }

    void zoom_at_cursor(HWND window, WindowContext& context, float zoom_factor, LPARAM l_param) {
        POINT cursor{ GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param) };
        ScreenToClient(window, &cursor);

        const float old_tile_size = context.base_tile_size * context.zoom_scale;
        const float map_x = (cursor.x + context.camera_x) / old_tile_size;
        const float map_y = (cursor.y + context.camera_y) / old_tile_size;
        context.zoom_scale = clamp(
            context.zoom_scale * zoom_factor,
            context.minimum_zoom,
            context.maximum_zoom
        );

        const float new_tile_size = context.base_tile_size * context.zoom_scale;
        context.camera_x = map_x * new_tile_size - cursor.x;
        context.camera_y = map_y * new_tile_size - cursor.y;
        clamp_camera(context);
        InvalidateRect(window, nullptr, FALSE);
    }

    // Быстрое вычисление коэффициента зума для колеса мыши.
    // Для типичных целочисленных шагов (delta кратно WHEEL_DELTA) обходимся
    // умножением, без вызова pow() в горячем обработчике сообщений.
    float wheel_zoom_factor(WPARAM w_param) {
        constexpr float zoom_step = 1.2f;
        const int delta = GET_WHEEL_DELTA_WPARAM(w_param);
        const int whole_steps = delta / WHEEL_DELTA;

        if (whole_steps == 0) {
            // Дробный/субшаговый delta (редкость) — только здесь используем pow().
            const float wheel_steps = static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA);
            return pow(zoom_step, wheel_steps);
        }

        float factor = 1.0f;
        const int abs_steps = whole_steps < 0 ? -whole_steps : whole_steps;
        for (int i = 0; i < abs_steps; ++i)
            factor *= zoom_step;
        return whole_steps > 0 ? factor : 1.0f / factor;
    }

    LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
        auto* context = reinterpret_cast<WindowContext*>(
            GetWindowLongPtrW(window, GWLP_USERDATA)
            );

        if (message == WM_NCCREATE) {
            const auto* create_info = reinterpret_cast<CREATESTRUCTW*>(l_param);
            context = static_cast<WindowContext*>(create_info->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
        }

        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC device_context = BeginPaint(window, &paint);
            if (context && context->render && context->world) {
                context->render->draw_map_tiles(
                    *context->world,
                    context->tile_transforms,
                    context->base_tile_size * context->zoom_scale,
                    context->camera_x,
                    context->camera_y,
                    context->frame_pixels,
                    context->show_continents,
                    context->show_humidity,
                    context->show_heat
                );

                BITMAPINFO bitmap_info{};
                bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bitmap_info.bmiHeader.biWidth = target_map_size;
                bitmap_info.bmiHeader.biHeight = -target_map_size;
                bitmap_info.bmiHeader.biPlanes = 1;
                bitmap_info.bmiHeader.biBitCount = 32;
                bitmap_info.bmiHeader.biCompression = BI_RGB;
                SetDIBitsToDevice(
                    device_context,
                    0, 0,
                    target_map_size, target_map_size,
                    0, 0,
                    0, target_map_size,
                    context->frame_pixels.data(),
                    &bitmap_info,
                    DIB_RGB_COLORS
                );
            }
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEWHEEL: {
            if (!context)
                return DefWindowProcW(window, message, w_param, l_param);
            zoom_at_cursor(window, *context, wheel_zoom_factor(w_param), l_param);
            return 0;
        }
        case WM_KEYDOWN: {
            if (!context)
                return DefWindowProcW(window, message, w_param, l_param);
            constexpr float pan_step = 48.0f;
            switch (w_param) {
            case 'C':
                if ((l_param & (1LL << 30)) == 0) {
                    context->show_continents = !context->show_continents;
                    InvalidateRect(window, nullptr, FALSE);
                }
                return 0;
            case 'W': context->camera_y -= pan_step; break;
            case 'S': context->camera_y += pan_step; break;
            case 'A': context->camera_x -= pan_step; break;
            case 'D': context->camera_x += pan_step; break;
            case 'H':
                if ((l_param & (1LL << 30)) == 0) {
                    context->show_humidity = !context->show_humidity;
                    InvalidateRect(window, nullptr, FALSE);
                }
                return 0;
            case 'T':
                if ((l_param & (1LL << 30)) == 0) {
                    context->show_heat = !context->show_heat;
                    InvalidateRect(window, nullptr, FALSE);
                }
                return 0;
            case 'R':
                // Игнорируем автоповтор удерживаемой клавиши.
                if ((l_param & (1LL << 30)) == 0 &&
                    map_regeneration_callback &&
                    map_regeneration_callback(map_regeneration_user_data)) {
                    reset_map_view(*context);
                    InvalidateRect(window, nullptr, FALSE);
                }
                return 0;

            default: return DefWindowProcW(window, message, w_param, l_param);
            }
            clamp_camera(*context);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, w_param, l_param);
        }
    }
}

// Устанавливается Game перед запуском окна и очищается после его закрытия.
void set_map_regeneration_callback(bool (*callback)(void*), void* user_data) {
    map_regeneration_callback = callback;
    map_regeneration_user_data = user_data;
}

void Render::set_zoom_scale(float zoom_scale) {
    if (isfinite(zoom_scale) && zoom_scale > 0.0f)
        zoom_scale_ = zoom_scale;
}

void Render::draw(const World& world) const {
    if (!gdiplus_runtime().is_ready())
        return;

    const auto& tile_map = world.get_tiled_map();
    const int base_tile_size = base_tile_size_for(tile_map);
    if (base_tile_size == 0)
        return;

    const size_t map_width = tile_map.front().size();
    const size_t map_height = tile_map.size();

    const double base_map_width = static_cast<double>(map_width) * base_tile_size;
    const double base_map_height = static_cast<double>(map_height) * base_tile_size;
    const float minimum_zoom = static_cast<float>(min({
        1.0,
        target_map_size / base_map_width,
        target_map_size / base_map_height
        }));
    const float maximum_zoom = 30.0f;
    const float initial_zoom = minimum_zoom;

    vector<unsigned char> tile_transforms;
    random_device random_device_source;
    mt19937 random_engine(random_device_source());
    uniform_int_distribution<int> transform_distribution(0, 7);
    tile_transforms.reserve(map_width * map_height);
    for (size_t i = 0; i < map_width * map_height; ++i) {
        tile_transforms.push_back(
            static_cast<unsigned char>(transform_distribution(random_engine))
        );
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    static bool class_registered = false;
    if (!class_registered) {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        window_class.lpszClassName = window_class_name;
        if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return;
        class_registered = true;
    }

    RECT window_rect{ 0, 0, target_map_size, target_map_size };
    if (!AdjustWindowRect(&window_rect, window_style, FALSE))
        return;

    WindowContext context{
        this,
        &world,
        move(tile_transforms),
        base_tile_size,
        initial_zoom,
        minimum_zoom,
        maximum_zoom
    };
    context.frame_pixels.resize(static_cast<size_t>(target_map_size) * target_map_size);
    const double initial_map_pixel_width = map_width * base_tile_size * initial_zoom;
    const double initial_map_pixel_height = map_height * base_tile_size * initial_zoom;
    context.camera_x = static_cast<float>(max(0.0, (initial_map_pixel_width - target_map_size) / 2.0));
    context.camera_y = static_cast<float>(max(0.0, (initial_map_pixel_height - target_map_size) / 2.0));
    clamp_camera(context);
    HWND window = CreateWindowExW(
        0,
        window_class_name,
        L"MyWorld",
        window_style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top,
        nullptr,
        nullptr,
        instance,
        &context
    );
    if (!window)
        return;

    ShowWindow(window, SW_SHOW);
    SetFocus(window);
    UpdateWindow(window);

    MSG message{};
    int result = 0;
    while ((result = static_cast<int>(GetMessageW(&message, nullptr, 0, 0))) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void Render::draw_map_tiles(
    const World& world,
    const vector<unsigned char>& tile_transforms,
    float tile_size,
    float camera_x,
    float camera_y,
    vector<uint32_t>& frame_pixels,
    bool show_continents,
    bool show_humidity,
    bool show_heat
)
const {
    const auto& tile_map = world.get_tiled_map();
    const auto& biome_map = world.get_biome_map();
    const auto& continent_map = world.get_continent_map();
    const auto& continent_colors = world.get_continent_colors();
    if (tile_map.empty() || tile_map.front().empty() || tile_size <= 0.0f)
        return;

    const size_t map_width = tile_map.front().size();
    for (size_t y = 0; y < tile_map.size(); ++y) {
        if (tile_map[y].size() != map_width)
            return;
    }

    // Fallback-цвета ландшафта: вода, пляж, равнина, горы, снег.
    const array<uint32_t, 6> fallback_colors = {
        (16u << 16) | (55u << 8) | 140u,
        (15u << 16) | (92u << 8) | 215u,
        (247u << 16) | (222u << 8) | 25u,
        (23u << 16) | (165u << 8) | 39u,
        (103u << 16) | (103u << 8) | 103u,
        (205u << 16) | (223u << 8) | 249u
    };

    // Fallback-цвета биомов (порядок соответствует ID в biome_map)
    const array<uint32_t, 9> biome_fallback_colors = {
        (255u << 16) | (250u << 8) | 250u, // 0 snow_plains
        (240u << 16) | (248u << 8) | 255u, // 1 snow_taiga
        (5u << 16) | (54u << 8) | 5u,      // 2 taiga
        (23u << 16) | (165u << 8) | 39u,   // 3 plains
        (15u << 16) | (145u << 8) | 29u,   // 4 forest
        (85u << 16) | (55u << 8) | 35u,    // 5 dark_forest
        (255u << 16) | (211u << 8) | 78u,  // 6 desert
        (237u << 16) | (164u << 8) | 35u,  // 7 savanna
        (15u << 16) | (70u << 8) | 30u    // 8 jungle
    };
    const bool use_flat_colors = tile_size < 7.0f;
    vector<TileTexture>* textures = use_flat_colors ? nullptr : &tile_textures();
    vector<TileTexture>* biomes = use_flat_colors ? nullptr : &biome_textures();
    if (frame_pixels.size() != static_cast<size_t>(target_map_size) * target_map_size)
        frame_pixels.resize(static_cast<size_t>(target_map_size) * target_map_size);
    fill(frame_pixels.begin(), frame_pixels.end(), 0u);

    // === Подготовка карты влажности ===
    // ВАЖНО: все ссылки на карты получены ДО запуска потоков ниже.
    // Потоки только читают эти const-объекты, поэтому гонок данных нет.
    const auto& humidity_map = world.get_humidity_map();
    const bool has_humidity =
        show_humidity &&
        !humidity_map.empty() &&
        humidity_map.size() == tile_map.size() &&
        !humidity_map.front().empty() &&
        humidity_map.front().size() == map_width &&
        !continent_map.empty() &&
        continent_map.size() == humidity_map.size();

    // === Подготовка тепловой карты ===
    const auto& heat_map = world.get_heat_map();
    const bool has_heat =
        show_heat &&
        !heat_map.empty() &&
        heat_map.size() == tile_map.size() &&
        !heat_map.front().empty() &&
        heat_map.front().size() == map_width &&
        !continent_map.empty() &&
        continent_map.size() == heat_map.size();

    // Для влажности используем абсолютную шкалу 0..1, а не перцентили карты.
    // Благодаря этому один и тот же цвет всегда означает одно и то же значение
    // влажности независимо от конкретного seed.

    // heat_map уже хранит нормализованные значения в диапазоне 0..1.

    const unsigned int reported_threads = thread::hardware_concurrency();
    const size_t thread_count = min(
        static_cast<size_t>(reported_threads == 0 ? 1 : reported_threads),
        static_cast<size_t>(target_map_size)
    );

    // Лямбда захватывает всё по ссылке, но все внешние объекты — константные,
    // и мы к ним обращаемся только на чтение. Каждый поток пишет только
    // в свой диапазон строк frame_pixels, поэтому записи не пересекаются.
    const auto render_rows = [&](int first_row, int end_row) {
        for (int screen_y = first_row; screen_y < end_row; ++screen_y) {
            const double map_y = (screen_y + 0.5 + camera_y) / tile_size;
            const long long tile_y = static_cast<long long>(floor(map_y));
            if (tile_y < 0 || static_cast<size_t>(tile_y) >= tile_map.size())
                continue;

            const double local_y = map_y - tile_y;
            uint32_t* output_row = frame_pixels.data() +
                static_cast<size_t>(screen_y) * target_map_size;

            for (int screen_x = 0; screen_x < target_map_size; ++screen_x) {
                const double map_x = (screen_x + 0.5 + camera_x) / tile_size;
                const long long tile_x = static_cast<long long>(floor(map_x));
                if (tile_x < 0 || static_cast<size_t>(tile_x) >= map_width)
                    continue;

                const int tile_type = tile_map[static_cast<size_t>(tile_y)][static_cast<size_t>(tile_x)];
                if (tile_type < 0 || tile_type >= static_cast<int>(fallback_colors.size()))
                    continue;

                // Обычная суша (tile_type == 3) получает текстуру из biome_map.
                // Вода, пляж, горы и высокогорье (типы 0, 1, 2, 4, 5)
                // продолжают использовать прежние текстуры из tile_map.
                const TileTexture* selected_texture = nullptr;
                if (!use_flat_colors) {
                    if (tile_type == 3 && biomes &&
                        static_cast<size_t>(tile_y) < biome_map.size() &&
                        static_cast<size_t>(tile_x) < biome_map[static_cast<size_t>(tile_y)].size()) {
                        const int biome_id = biome_map[static_cast<size_t>(tile_y)][static_cast<size_t>(tile_x)];
                        if (biome_id >= 0 && static_cast<size_t>(biome_id) < biomes->size()) {
                            const TileTexture& biome_texture = (*biomes)[static_cast<size_t>(biome_id)];
                            if (!biome_texture.pixels.empty() && biome_texture.width > 0 && biome_texture.height > 0)
                                selected_texture = &biome_texture;
                        }
                    }

                    // Если biome_map отсутствует/некорректна или файл биома
                    // не загрузился, используем старую plains.png.
                    if (!selected_texture && textures &&
                        static_cast<size_t>(tile_type) < textures->size()) {
                        const TileTexture& legacy_texture = (*textures)[static_cast<size_t>(tile_type)];
                        if (!legacy_texture.pixels.empty() && legacy_texture.width > 0 && legacy_texture.height > 0)
                            selected_texture = &legacy_texture;
                    }
                }

                const bool has_texture = selected_texture != nullptr;

                uint32_t fallback_color = fallback_colors[tile_type];
                if (tile_type == 3 &&
                    static_cast<size_t>(tile_y) < biome_map.size() &&
                    static_cast<size_t>(tile_x) < biome_map[static_cast<size_t>(tile_y)].size()) {
                    const int biome_id = biome_map[static_cast<size_t>(tile_y)][static_cast<size_t>(tile_x)];
                    if (biome_id >= 0 && static_cast<size_t>(biome_id) < biome_fallback_colors.size())
                        fallback_color = biome_fallback_colors[static_cast<size_t>(biome_id)];
                }

                if (!has_texture) {
                    output_row[screen_x] = fallback_color;
                }
                else {
                    const size_t transform_index = static_cast<size_t>(tile_y) * map_width +
                        static_cast<size_t>(tile_x);
                    const int transform = transform_index < tile_transforms.size()
                        ? tile_transforms[transform_index]
                        : 0;
                    double texture_x = map_x - tile_x;
                    double texture_y = local_y;

                    // Отменяем поворот плитки по часовой стрелке, затем — зеркально
                    for (int rotation = 0; rotation < transform % 4; ++rotation) {
                        const double previous_x = texture_x;
                        texture_x = texture_y;
                        texture_y = 1.0 - previous_x;
                    }
                    if (transform >= 4)
                        texture_x = 1.0 - texture_x;

                    const TileTexture& texture = *selected_texture;
                    const size_t source_x = min(
                        static_cast<size_t>(texture_x * texture.width),
                        static_cast<size_t>(texture.width - 1)
                    );
                    const size_t source_y = min(
                        static_cast<size_t>(texture_y * texture.height),
                        static_cast<size_t>(texture.height - 1)
                    );
                    output_row[screen_x] = texture.pixels[source_y * texture.width + source_x];
                }

                // Континенты: 70% непрозрачности, вода (id = -1) не окрашивается.
                if (show_continents &&
                    static_cast<size_t>(tile_y) < continent_map.size() &&
                    static_cast<size_t>(tile_x) < continent_map[static_cast<size_t>(tile_y)].size()) {
                    const int continent_id =
                        continent_map[static_cast<size_t>(tile_y)][static_cast<size_t>(tile_x)];
                    if (continent_id >= 0 && static_cast<size_t>(continent_id) < continent_colors.size()) {
                        constexpr uint32_t overlay_alpha = 179; // ~70% непрозрачности
                        const uint32_t cc = continent_colors[static_cast<size_t>(continent_id)];
                        const uint32_t base_color = output_row[screen_x];

                        const uint32_t red = blend_channel((cc >> 16) & 0xFFu, (base_color >> 16) & 0xFFu, overlay_alpha);
                        const uint32_t green = blend_channel((cc >> 8) & 0xFFu, (base_color >> 8) & 0xFFu, overlay_alpha);
                        const uint32_t blue = blend_channel(cc & 0xFFu, base_color & 0xFFu, overlay_alpha);

                        output_row[screen_x] = (red << 16) | (green << 8) | blue;
                    }
                }

                // Влажность: 65% непрозрачности, только на континентах.
                if (has_humidity &&
                    static_cast<size_t>(tile_y) < continent_map.size() &&
                    static_cast<size_t>(tile_x) < continent_map[static_cast<size_t>(tile_y)].size()) {
                    const int continent_id =
                        continent_map[static_cast<size_t>(tile_y)][static_cast<size_t>(tile_x)];
                    if (continent_id >= 0 &&
                        static_cast<size_t>(tile_y) < humidity_map.size() &&
                        static_cast<size_t>(tile_x) < humidity_map[static_cast<size_t>(tile_y)].size()) {

                        const double h =
                            humidity_map[static_cast<size_t>(tile_y)][static_cast<size_t>(tile_x)];

                        if (isfinite(h)) {
                            // Фиксированная шкала влажности:
                            // 0.0 — сухо (красно-оранжевый),
                            // 0.3 — граница сухого/умеренного (жёлтый),
                            // 0.7 — граница умеренного/влажного (зелёный),
                            // 1.0 — очень влажно (синий).
                            // Никакой автонормализации и гаммы: цвет соответствует
                            // реальному значению humidity_map_ и порогам биомов.
                            const double t = clamp(h, 0.0, 1.0);

                            uint32_t hum_r, hum_g, hum_b;
                            double u;

                            if (t < 0.3) {
                                // Красно-оранжевый -> жёлтый.
                                u = t / 0.3;
                                hum_r = 255;
                                hum_g = static_cast<uint32_t>(
                                    75.0 + (215.0 - 75.0) * u + 0.5
                                    );
                                hum_b = static_cast<uint32_t>(
                                    35.0 + (65.0 - 35.0) * u + 0.5
                                    );
                            }
                            else if (t < 0.7) {
                                // Жёлтый -> зелёный.
                                u = (t - 0.3) / 0.4;
                                hum_r = static_cast<uint32_t>(
                                    255.0 + (75.0 - 255.0) * u + 0.5
                                    );
                                hum_g = static_cast<uint32_t>(
                                    215.0 + (190.0 - 215.0) * u + 0.5
                                    );
                                hum_b = static_cast<uint32_t>(
                                    65.0 + (115.0 - 65.0) * u + 0.5
                                    );
                            }
                            else {
                                // Зелёный -> синий.
                                u = (t - 0.7) / 0.3;
                                hum_r = static_cast<uint32_t>(
                                    75.0 + (35.0 - 75.0) * u + 0.5
                                    );
                                hum_g = static_cast<uint32_t>(
                                    190.0 + (105.0 - 190.0) * u + 0.5
                                    );
                                hum_b = static_cast<uint32_t>(
                                    115.0 + (235.0 - 115.0) * u + 0.5
                                    );
                            }

                            constexpr uint32_t overlay_alpha = 166; // ~65% непрозрачности
                            const uint32_t base_color = output_row[screen_x];

                            const uint32_t red = blend_channel(hum_r, (base_color >> 16) & 0xFFu, overlay_alpha);
                            const uint32_t green = blend_channel(hum_g, (base_color >> 8) & 0xFFu, overlay_alpha);
                            const uint32_t blue = blend_channel(hum_b, base_color & 0xFFu, overlay_alpha);

                            output_row[screen_x] = (red << 16) | (green << 8) | blue;
                        }
                    }
                }

                // Тепловая карта: холодный синий -> белый -> горячий красный.
                // Рисуется только на суше, с той же прозрачностью, что и влажность.
                if (has_heat &&
                    static_cast<size_t>(tile_y) < continent_map.size() &&
                    static_cast<size_t>(tile_x) < continent_map[static_cast<size_t>(tile_y)].size() &&
                    continent_map[static_cast<size_t>(tile_y)][static_cast<size_t>(tile_x)] >= 0 &&
                    static_cast<size_t>(tile_y) < heat_map.size() &&
                    static_cast<size_t>(tile_x) < heat_map[static_cast<size_t>(tile_y)].size()) {

                    const double value =
                        heat_map[static_cast<size_t>(tile_y)][static_cast<size_t>(tile_x)];

                    if (isfinite(value)) {
                        // Значения heat_map уже находятся в диапазоне 0..1.
                        const double t = clamp(value, 0.0, 1.0);

                        uint32_t heat_r, heat_g, heat_b;
                        if (t <= 0.5) {
                            // Синий -> белый.
                            const uint32_t u = static_cast<uint32_t>(t * 2.0 * 255.0 + 0.5);
                            heat_r = u;
                            heat_g = u;
                            heat_b = 255;
                        }
                        else {
                            // Белый -> красный.
                            const uint32_t u = static_cast<uint32_t>((1.0 - (t - 0.5) * 2.0) * 255.0 + 0.5);
                            heat_r = 255;
                            heat_g = u;
                            heat_b = u;
                        }

                        constexpr uint32_t heat_overlay_alpha = 166; // ~65% непрозрачности
                        const uint32_t base_color = output_row[screen_x];
                        const uint32_t red = blend_channel(heat_r, (base_color >> 16) & 0xFFu, heat_overlay_alpha);
                        const uint32_t green = blend_channel(heat_g, (base_color >> 8) & 0xFFu, heat_overlay_alpha);
                        const uint32_t blue = blend_channel(heat_b, base_color & 0xFFu, heat_overlay_alpha);
                        output_row[screen_x] = (red << 16) | (green << 8) | blue;
                    }
                }
            }
        }
        };

    vector<thread> workers;
    workers.reserve(thread_count > 0 ? thread_count - 1 : 0);
    for (size_t worker = 0; worker + 1 < thread_count; ++worker) {
        const int first_row = static_cast<int>(worker * target_map_size / thread_count);
        const int end_row = static_cast<int>((worker + 1) * target_map_size / thread_count);
        workers.emplace_back(render_rows, first_row, end_row);
    }

    const int current_thread_first_row = static_cast<int>((thread_count - 1) * target_map_size / thread_count);
    render_rows(current_thread_first_row, target_map_size);
    for (thread& worker : workers)
        worker.join();


    // Объекты запрашиваются только через ID тех чанков, которые пересекают экран.
    const auto& chunks = world.get_chunks();
    const auto& objects = world.get_objects();
    const int chunks_x = (static_cast<int>(map_width) + Chunk::size_in_tiles - 1) /
        Chunk::size_in_tiles;
    if (chunks_x <= 0 || chunks.empty()) return;

    const int first_tile_x = max(0, static_cast<int>(floor(camera_x / tile_size)));
    const int first_tile_y = max(0, static_cast<int>(floor(camera_y / tile_size)));
    const int last_tile_x = min(static_cast<int>(map_width) - 1,
        static_cast<int>(floor((camera_x + target_map_size) / tile_size)));
    const int last_tile_y = min(static_cast<int>(tile_map.size()) - 1,
        static_cast<int>(floor((camera_y + target_map_size) / tile_size)));
    if (last_tile_x < first_tile_x || last_tile_y < first_tile_y) return;

    const int first_chunk_x = first_tile_x / Chunk::size_in_tiles;
    const int first_chunk_y = first_tile_y / Chunk::size_in_tiles;
    const int last_chunk_x = last_tile_x / Chunk::size_in_tiles;
    const int last_chunk_y = last_tile_y / Chunk::size_in_tiles;

    const TreeTextureBank& available_tree_textures = tree_textures();
    const vector<TileTexture>& available_mineral_textures = mineral_textures();

    vector<unsigned char> drawn(objects.size(), 0);
    vector<size_t> visible_objects;
    for (int chunk_y = first_chunk_y; chunk_y <= last_chunk_y; ++chunk_y) {
        for (int chunk_x = first_chunk_x; chunk_x <= last_chunk_x; ++chunk_x) {
            const size_t chunk_index = static_cast<size_t>(chunk_y) * chunks_x + chunk_x;
            if (chunk_index >= chunks.size()) continue;
            for (ObjectId id : chunks[chunk_index].static_object_ids()) {
                if (id == 0 || static_cast<size_t>(id) > objects.size()) continue;
                const size_t object_index = static_cast<size_t>(id - 1);
                if (drawn[object_index]) continue;
                drawn[object_index] = 1;
                visible_objects.push_back(object_index);
            }
        }
    }

    // Сначала рисуем дальние объекты; основание ниже на карте — ближний слой.
    stable_sort(visible_objects.begin(), visible_objects.end(), [&](size_t left, size_t right) {
        const WorldObject& a = objects[left];
        const WorldObject& b = objects[right];
        if (a.y + a.height != b.y + b.height)
            return a.y + a.height < b.y + b.height;
        if (a.x != b.x)
            return a.x < b.x;
        return a.id < b.id;
        });

    for (size_t object_index : visible_objects) {
        const WorldObject& object = objects[object_index];

        const TileTexture* object_texture = nullptr;
        if (object.type == WorldObjectType::Tree) {
            if (object.biome_id < 0 ||
                static_cast<size_t>(object.biome_id) >= available_tree_textures.size())
                continue;

            const vector<TileTexture>& variants =
                available_tree_textures[static_cast<size_t>(object.biome_id)];

            // Количество вариантов берётся из реально найденных PNG-файлов.
            // variant хранит seed, поэтому добавление новых PNG не требует правок C++.
            if (variants.empty())
                continue;

            const size_t variant_index =
                static_cast<uint32_t>(object.variant) % variants.size();
            object_texture = &variants[variant_index];
        }
        else if (object.type == WorldObjectType::Mineral) {
            if (object.variant >= 0 && static_cast<size_t>(object.variant) < available_mineral_textures.size() &&
                !available_mineral_textures[object.variant].pixels.empty()) {
                object_texture = &available_mineral_textures[object.variant];
            }
        }

        const double sprite_height = max(1.0, object.height * static_cast<double>(tile_size));
        const double sprite_width = object_texture && object_texture->height > 0
            ? sprite_height * static_cast<double>(object_texture->width) / object_texture->height
            : max(1.0, object.width * static_cast<double>(tile_size));
        const int footprint_left = static_cast<int>(floor(object.x * tile_size - camera_x));
        const int left = footprint_left + static_cast<int>(floor((object.width * tile_size - sprite_width) / 2.0));
        const int top = static_cast<int>(floor(object.y * tile_size - camera_y));
        const int draw_width = max(1, static_cast<int>(ceil(sprite_width)));
        const int draw_height = max(1, static_cast<int>(ceil(sprite_height)));

        // Подбираем mip-уровень по фактическому уменьшению спрайта на экране.
        const vector<uint32_t>* sampled_pixels = object_texture ? &object_texture->pixels : nullptr;
        const vector<unsigned char>* sampled_alpha = object_texture ? &object_texture->alpha : nullptr;
        UINT sampled_width = object_texture ? object_texture->width : 0;
        UINT sampled_height = object_texture ? object_texture->height : 0;
        if (object_texture && object.type == WorldObjectType::Tree &&
            !object_texture->mipmaps.empty()) {
            const double reduction = max(
                static_cast<double>(object_texture->width) / draw_width,
                static_cast<double>(object_texture->height) / draw_height
            );
            if (reduction >= 2.0) {
                size_t mip_index = static_cast<size_t>(floor(log2(reduction)));
                mip_index = min(mip_index, object_texture->mipmaps.size());
                if (mip_index > 0) {
                    const TextureMipLevel& mip = object_texture->mipmaps[mip_index - 1];
                    sampled_pixels = &mip.pixels;
                    sampled_alpha = &mip.alpha;
                    sampled_width = mip.width;
                    sampled_height = mip.height;
                }
            }
        }

        const int right = min(target_map_size, left + draw_width);
        const int bottom = min(target_map_size, top + draw_height);
        if (right <= 0 || bottom <= 0 || left >= target_map_size || top >= target_map_size)
            continue;

        const bool apply_small_object_effect = use_flat_colors && object.width < 5 && object.height < 5;

        const uint32_t fallback_color = (object.type == WorldObjectType::Mineral)
            ? ((128u << 16) | (128u << 8) | 128u)
            : ((20u << 16) | (105u << 8) | 35u);

        const auto sample_object_pixel = [&](int local_x, int local_y) {
            array<uint32_t, 4> sample{}; // premultiplied RGBA
            if (local_x < 0 || local_y < 0 || local_x >= draw_width || local_y >= draw_height)
                return sample;
            if (object_texture && sampled_pixels && sampled_alpha &&
                sampled_width > 0 && sampled_height > 0) {
                const UINT sx = min(static_cast<UINT>((local_x + 0.5) * sampled_width / draw_width), sampled_width - 1);
                const UINT sy = min(static_cast<UINT>((local_y + 0.5) * sampled_height / draw_height), sampled_height - 1);
                const size_t source_index = static_cast<size_t>(sy) * sampled_width + sx;
                const uint32_t color = (*sampled_pixels)[source_index];
                sample = { (color >> 16) & 0xffu, (color >> 8) & 0xffu, color & 0xffu,
                    (*sampled_alpha)[source_index] };
            }
            else {
                sample = { (fallback_color >> 16) & 0xffu, (fallback_color >> 8) & 0xffu,
                    fallback_color & 0xffu, 255u };
            }
            return sample;
            };

        for (int screen_y = max(0, top); screen_y < bottom; ++screen_y) {
            for (int screen_x = max(0, left); screen_x < right; ++screen_x) {
                const int local_x = screen_x - left;
                const int local_y = screen_y - top;
                array<uint32_t, 4> source{};
                if (apply_small_object_effect) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const auto sample = sample_object_pixel(local_x + dx, local_y + dy);
                            for (size_t channel = 0; channel < source.size(); ++channel)
                                source[channel] += sample[channel];
                        }
                    }
                    for (uint32_t& channel : source)
                        channel = (channel + 4u) / 9u;
                }
                else {
                    source = sample_object_pixel(local_x, local_y);
                }
                const uint32_t source_alpha = source[3];
                const uint32_t red = apply_small_object_effect ? min(source_alpha, (source[0] * 120u + 50u) / 100u) : source[0];
                const uint32_t green = apply_small_object_effect ? min(source_alpha, (source[1] * 120u + 50u) / 100u) : source[1];
                const uint32_t blue = apply_small_object_effect ? min(source_alpha, (source[2] * 120u + 50u) / 100u) : source[2];
                const uint32_t old = frame_pixels[static_cast<size_t>(screen_y) *
                    target_map_size + screen_x];
                const uint32_t inv_alpha = 255u - source_alpha;
                const uint32_t out_red = red + (((old >> 16) & 0xffu) * inv_alpha + 127u) / 255u;
                const uint32_t out_green = green + (((old >> 8) & 0xffu) * inv_alpha + 127u) / 255u;
                const uint32_t out_blue = blue + ((old & 0xffu) * inv_alpha + 127u) / 255u;
                frame_pixels[static_cast<size_t>(screen_y) * target_map_size + screen_x] =
                    (min(out_red, 255u) << 16) | (min(out_green, 255u) << 8) | min(out_blue, 255u);
            }
        }
    }
}