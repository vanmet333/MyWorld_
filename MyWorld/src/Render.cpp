#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>

#include "Render.h"
#include "World.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
    };

    struct TileTexture {
        UINT width = 0;
        UINT height = 0;
        vector<uint32_t> pixels;
        vector<unsigned char> alpha;
    };

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

            filesystem::path texture_directory = filesystem::current_path() / L"assets" / L"textures" / L"terrain";
            error_code error;
            if (!filesystem::exists(texture_directory, error)) {
                vector<wchar_t> executable_path(32768);
                const DWORD length = GetModuleFileNameW(
                    nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size())
                );
                if (length > 0 && length < executable_path.size()) {
                    filesystem::path project_directory = filesystem::path(
                        wstring(executable_path.data(), length)
                    ).parent_path().parent_path().parent_path();
                    texture_directory = project_directory / L"assets" / L"textures" / L"terrain";
                }
            }

            vector<TileTexture> loaded;
            loaded.reserve(file_names.size());
            for (const wchar_t* file_name : file_names) {
                auto bitmap = make_unique<Bitmap>((texture_directory / file_name).c_str());
                TileTexture texture;
                if (bitmap->GetLastStatus() == Ok) {
                    texture.width = bitmap->GetWidth();
                    texture.height = bitmap->GetHeight();
                    texture.pixels.resize(static_cast<size_t>(texture.width) * texture.height);
                    texture.alpha.resize(static_cast<size_t>(texture.width) * texture.height);
                    for (UINT y = 0; y < texture.height; ++y) {
                        for (UINT x = 0; x < texture.width; ++x) {
                            Color color;
                            if (bitmap->GetPixel(x, y, &color) == Ok) {
                                const uint32_t alpha = color.GetA();
                                const uint32_t red = color.GetR() * alpha / 255;
                                const uint32_t green = color.GetG() * alpha / 255;
                                const uint32_t blue = color.GetB() * alpha / 255;
                                texture.pixels[static_cast<size_t>(y) * texture.width + x] =
                                    (red << 16) | (green << 8) | blue;
                                texture.alpha[static_cast<size_t>(y) * texture.width + x] =
                                    static_cast<unsigned char>(alpha);
                            }
                        }
                    }
                }
                loaded.push_back(move(texture));
            }
            return loaded;
            }();
        return textures;
    }

    vector<TileTexture>& tree_textures() {
        static vector<TileTexture> textures = [] {
            vector<TileTexture> loaded;
            filesystem::path directory = filesystem::current_path() / L"assets" / L"textures" / L"tree" / L"plains";
            error_code error;
            if (!filesystem::exists(directory, error)) {
                vector<wchar_t> executable_path(32768);
                const DWORD length = GetModuleFileNameW(nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
                if (length > 0 && length < executable_path.size()) {
                    filesystem::path project_directory = filesystem::path(wstring(executable_path.data(), length)).parent_path().parent_path().parent_path();
                    directory = project_directory / L"assets" / L"textures" / L"tree" / L"plains";
                }
            }
            for (int variant = 1; variant <= 3; ++variant) {
                const filesystem::path file = directory / (L"plains_tree_ver" + to_wstring(variant) + L".png");
                auto bitmap = make_unique<Bitmap>(file.c_str());
                TileTexture texture;
                if (bitmap->GetLastStatus() == Ok) {
                    texture.width = bitmap->GetWidth();
                    texture.height = bitmap->GetHeight();
                    texture.pixels.resize(static_cast<size_t>(texture.width) * texture.height);
                    texture.alpha.resize(static_cast<size_t>(texture.width) * texture.height);
                    for (UINT y = 0; y < texture.height; ++y) for (UINT x = 0; x < texture.width; ++x) {
                        Color color;
                        if (bitmap->GetPixel(x, y, &color) == Ok) {
                            const uint32_t alpha = color.GetA();
                            texture.pixels[static_cast<size_t>(y) * texture.width + x] =
                                ((color.GetR() * alpha / 255) << 16) | ((color.GetG() * alpha / 255) << 8) | (color.GetB() * alpha / 255);
                            texture.alpha[static_cast<size_t>(y) * texture.width + x] = static_cast<unsigned char>(alpha);
                        }
                    }
                }
                loaded.push_back(move(texture));
            }
            return loaded;
            }();
        return textures;
    }

    // Функция вынесена из tree_textures()
    vector<TileTexture>& mineral_textures() {
        static vector<TileTexture> textures = [] {
            vector<TileTexture> loaded;
            filesystem::path directory = filesystem::current_path() / L"assets" / L"textures" / L"minerals";
            error_code error;
            if (!filesystem::exists(directory, error)) {
                vector<wchar_t> executable_path(32768);
                const DWORD length = GetModuleFileNameW(nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
                if (length > 0 && length < executable_path.size()) {
                    filesystem::path project_directory = filesystem::path(wstring(executable_path.data(), length)).parent_path().parent_path().parent_path();
                    directory = project_directory / L"assets" / L"textures" / L"minerals";
                }
            }

            // Порядок должен соответствовать MineralVariant (Rock, Copper, Iron, Gold, Platinum)
            const array<const wchar_t*, 5> file_names = {
                L"rock.png", L"copper.png", L"iron.png", L"gold.png", L"platinum.png"
            };

            for (const auto* file_name : file_names) {
                const filesystem::path file = directory / file_name;
                auto bitmap = make_unique<Bitmap>(file.c_str());
                TileTexture texture;
                if (bitmap->GetLastStatus() == Ok) {
                    texture.width = bitmap->GetWidth();
                    texture.height = bitmap->GetHeight();
                    texture.pixels.resize(static_cast<size_t>(texture.width) * texture.height);
                    texture.alpha.resize(static_cast<size_t>(texture.width) * texture.height);
                    for (UINT y = 0; y < texture.height; ++y) for (UINT x = 0; x < texture.width; ++x) {
                        Color color;
                        if (bitmap->GetPixel(x, y, &color) == Ok) {
                            const uint32_t alpha = color.GetA();
                            texture.pixels[static_cast<size_t>(y) * texture.width + x] =
                                ((color.GetR() * alpha / 255) << 16) | ((color.GetG() * alpha / 255) << 8) | (color.GetB() * alpha / 255);
                            texture.alpha[static_cast<size_t>(y) * texture.width + x] = static_cast<unsigned char>(alpha);
                        }
                    }
                }
                loaded.push_back(move(texture));
            }
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
                    context->show_continents
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
            const float wheel_steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(w_param)) /
                static_cast<float>(WHEEL_DELTA);
            zoom_at_cursor(window, *context, pow(1.2f, wheel_steps), l_param);
            return 0;
        }
        case WM_KEYDOWN: {
            if (!context)
                return DefWindowProcW(window, message, w_param, l_param);
            constexpr float pan_step = 48.0f;
            switch (w_param) {
            case 'C':
                // Игнорируем автоповтор клавиши, чтобы удержание C не переключало режим многократно.
                if ((l_param & (1LL << 30)) == 0) {
                    context->show_continents = !context->show_continents;
                    InvalidateRect(window, nullptr, FALSE);
                }
                return 0;
            case 'W': context->camera_y -= pan_step; break;
            case 'S': context->camera_y += pan_step; break;
            case 'A': context->camera_x -= pan_step; break;
            case 'D': context->camera_x += pan_step; break;
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
    bool show_continents
) const {
    const auto& tile_map = world.get_tiled_map();
    const auto& continent_map = world.get_continent_map();
    const auto& continent_colors = world.get_continent_colors();
    if (tile_map.empty() || tile_map.front().empty() || tile_size <= 0.0f)
        return;

    const size_t map_width = tile_map.front().size();
    for (size_t y = 0; y < tile_map.size(); ++y) {
        if (tile_map[y].size() != map_width)
            return;
    }

    const array<uint32_t, 6> fallback_colors = {
        (16u << 16) | (55u << 8) | 140u,
        (15u << 16) | (92u << 8) | 215u,
        (247u << 16) | (222u << 8) | 25u,
        (23u << 16) | (165u << 8) | 39u,
        (103u << 16) | (103u << 8) | 103u,
        (205u << 16) | (223u << 8) | 249u
    };
    const bool use_flat_colors = tile_size < 7.0f;
    vector<TileTexture>* textures = use_flat_colors ? nullptr : &tile_textures();
    if (frame_pixels.size() != static_cast<size_t>(target_map_size) * target_map_size)
        frame_pixels.resize(static_cast<size_t>(target_map_size) * target_map_size);
    fill(frame_pixels.begin(), frame_pixels.end(), 0u);

    const unsigned int reported_threads = thread::hardware_concurrency();
    const size_t thread_count = min(
        static_cast<size_t>(reported_threads == 0 ? 1 : reported_threads),
        static_cast<size_t>(target_map_size)
    );

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

                const bool has_texture = !use_flat_colors && textures &&
                    !(*textures)[tile_type].pixels.empty() &&
                    (*textures)[tile_type].width > 0 &&
                    (*textures)[tile_type].height > 0;

                if (!has_texture) {
                    output_row[screen_x] = fallback_colors[tile_type];
                }
                else {
                    const size_t transform_index = static_cast<size_t>(tile_y) * map_width +
                        static_cast<size_t>(tile_x);
                    const int transform = transform_index < tile_transforms.size()
                        ? tile_transforms[transform_index]
                        : 0;
                    double texture_x = map_x - tile_x;
                    double texture_y = local_y;

                    // Undo the tile's clockwise rotation, then its horizontal mirror.
                    for (int rotation = 0; rotation < transform % 4; ++rotation) {
                        const double previous_x = texture_x;
                        texture_x = texture_y;
                        texture_y = 1.0 - previous_x;
                    }
                    if (transform >= 4)
                        texture_x = 1.0 - texture_x;

                    const TileTexture& texture = (*textures)[tile_type];
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

                // Континенты накладываются поверх поверхности с непрозрачностью 70%
                // (то есть 30% прозрачности). Вода с ID континента -1 не окрашивается.
                if (show_continents &&
                    static_cast<size_t>(tile_y) < continent_map.size() &&
                    static_cast<size_t>(tile_x) < continent_map[static_cast<size_t>(tile_y)].size()) {
                    const int continent_id = continent_map[static_cast<size_t>(tile_y)][static_cast<size_t>(tile_x)];
                    if (continent_id >= 0 && static_cast<size_t>(continent_id) < continent_colors.size()) {
                        constexpr uint32_t overlay_alpha = 179; // 70% непрозрачности
                        constexpr uint32_t inverse_alpha = 255 - overlay_alpha;
                        const uint32_t continent_color = continent_colors[static_cast<size_t>(continent_id)];
                        const uint32_t base_color = output_row[screen_x];

                        const uint32_t red = ((((continent_color >> 16) & 0xffu) * overlay_alpha) +
                            (((base_color >> 16) & 0xffu) * inverse_alpha) + 127u) / 255u;
                        const uint32_t green = ((((continent_color >> 8) & 0xffu) * overlay_alpha) +
                            (((base_color >> 8) & 0xffu) * inverse_alpha) + 127u) / 255u;
                        const uint32_t blue = (((continent_color & 0xffu) * overlay_alpha) +
                            ((base_color & 0xffu) * inverse_alpha) + 127u) / 255u;

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

    const vector<TileTexture>& available_tree_textures = tree_textures();
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

    // Сначала рисуем дальние объекты; основание ниже на карте означает ближний слой.
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

        // Правильный выбор текстуры в зависимости от типа объекта
        const TileTexture* object_texture = nullptr;
        if (object.type == WorldObjectType::Tree) {
            if (object.variant >= 0 && static_cast<size_t>(object.variant) < available_tree_textures.size() &&
                !available_tree_textures[object.variant].pixels.empty()) {
                object_texture = &available_tree_textures[object.variant];
            }
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
        const int right = min(target_map_size, left + draw_width);
        const int bottom = min(target_map_size, top + draw_height);
        if (right <= 0 || bottom <= 0 || left >= target_map_size || top >= target_map_size)
            continue;

        const bool apply_small_object_effect = use_flat_colors && object.width < 5 && object.height < 5;

        // Цвет заглушка для минералов (серый) и деревьев (зеленый)
        const uint32_t fallback_color = (object.type == WorldObjectType::Mineral)
            ? ((128u << 16) | (128u << 8) | 128u)
            : ((20u << 16) | (105u << 8) | 35u);

        const auto sample_object_pixel = [&](int local_x, int local_y) {
            array<uint32_t, 4> sample{}; // premultiplied RGBA
            if (local_x < 0 || local_y < 0 || local_x >= draw_width || local_y >= draw_height)
                return sample;
            if (object_texture) {
                const UINT sx = min(static_cast<UINT>((local_x + 0.5) * object_texture->width / draw_width), object_texture->width - 1);
                const UINT sy = min(static_cast<UINT>((local_y + 0.5) * object_texture->height / draw_height), object_texture->height - 1);
                const size_t source_index = static_cast<size_t>(sy) * object_texture->width + sx;
                const uint32_t color = object_texture->pixels[source_index];
                sample = { (color >> 16) & 0xffu, (color >> 8) & 0xffu, color & 0xffu,
                    object_texture->alpha[source_index] };
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