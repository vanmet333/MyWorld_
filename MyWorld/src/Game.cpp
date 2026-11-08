#include "Game.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// Render вызывает этот callback при нажатии R.
void set_map_regeneration_callback(bool (*callback)(void*), void* user_data);

using namespace std;
namespace {
    filesystem::path find_world_consts_file() {
        error_code error;
        filesystem::path directory = filesystem::current_path(error);
        while (!directory.empty()) {
            const filesystem::path direct_path = directory / L"src" / L"world_consts.txt";
            if (filesystem::exists(direct_path, error)) return direct_path;

            const filesystem::path project_path = directory / L"MyWorld" / L"src" / L"world_consts.txt";
            if (filesystem::exists(project_path, error)) return project_path;

            const filesystem::path parent = directory.parent_path();
            if (parent == directory) break;
            directory = parent;
        }
        return {};
    }

    WorldGenerationConfig load_world_consts() {
        const filesystem::path file_path = find_world_consts_file();
        if (file_path.empty())
            throw runtime_error("world_consts.txt not found");

        ifstream file(file_path);
        if (!file)
            throw runtime_error("Failed to open world_consts.txt: " + file_path.string());

        WorldGenerationConfig config{};

        constexpr array<const char*, WorldGenerationConfig::tree_region_count> tree_region_names = {
            "snow_plains", "snow_taiga", "taiga", "plains", "forest",
            "dark_forest", "desert", "savanna", "jungle", "beach"
        };

        string line;
        while (getline(file, line)) {
            const size_t separator = line.find('=');
            if (separator == string::npos)
                continue;

            string key = line.substr(0, separator);
            string value = line.substr(separator + 1);

            const auto trim = [](string& text) {
                while (!text.empty() && isspace(static_cast<unsigned char>(text.front())))
                    text.erase(text.begin());
                while (!text.empty() && isspace(static_cast<unsigned char>(text.back())))
                    text.pop_back();
            };
            trim(key);
            trim(value);

            if (key.empty() || key.front() == '#')
                continue;

            try {
                bool is_tree_setting = false;

                for (size_t region = 0; region < tree_region_names.size(); ++region) {
                    const string prefix = string(tree_region_names[region]) + "_tree_";

                    if (key == prefix + "density") {
                        config.tree_density[region] = stod(value);
                        is_tree_setting = true;
                        break;
                    }

                    if (key == prefix + "clumpiness") {
                        config.tree_clumpiness[region] = stod(value);
                        is_tree_setting = true;
                        break;
                    }
                }

                if (is_tree_setting)
                    continue;

                if (key == "persistence" ||
                    key == "deep_water_boundary" || key == "shallow_water_boundary" ||
                    key == "beach_boundary" || key == "plains_boundary" ||
                    key == "mountains_boundary" ||
                    key == "rock_chance" || key == "copper_chance" ||
                    key == "iron_chance" || key == "gold_chance" ||
                    key == "platinum_chance" || key == "cold_biome_boundary" ||
                    key == "hot_biome_boundary" || key == "wet_biome_boundary" ||
                    key == "dry_biome_boundary" || key == "min_biome_size") {
                    const double parsed_value = stod(value);
                    if (!isfinite(parsed_value))
                        throw invalid_argument("value must be finite");

                    if (key == "persistence") config.persistence = parsed_value;
                    else if (key == "deep_water_boundary") config.deep_water_boundary = parsed_value;
                    else if (key == "shallow_water_boundary") config.shallow_water_boundary = parsed_value;
                    else if (key == "beach_boundary") config.beach_boundary = parsed_value;
                    else if (key == "plains_boundary") config.plains_boundary = parsed_value;
                    else if (key == "mountains_boundary") config.mountains_boundary = parsed_value;
                    else if (key == "rock_chance") config.rock_chance = parsed_value;
                    else if (key == "copper_chance") config.copper_chance = parsed_value;
                    else if (key == "iron_chance") config.iron_chance = parsed_value;
                    else if (key == "gold_chance") config.gold_chance = parsed_value;
                    else if (key == "platinum_chance") config.platinum_chance = parsed_value;
                    else if (key == "cold_biome_boundary") config.cold_biome_boundary = parsed_value;
                    else if (key == "hot_biome_boundary") config.hot_biome_boundary = parsed_value;
                    else if (key == "wet_biome_boundary") config.wet_biome_boundary = parsed_value;
                    else if (key == "dry_biome_boundary") config.dry_biome_boundary = parsed_value;
                    else if (key == "min_biome_size") config.min_biome_size = parsed_value;
                }
                else {
                    const int parsed_value = stoi(value);

                    if (key == "map_height" && parsed_value > 0) config.map_height = parsed_value;
                    else if (key == "map_width" && parsed_value > 0) config.map_width = parsed_value;
                    else if (key == "octaves" && parsed_value > 0) config.octaves = parsed_value;
                    else if (key == "scale" && parsed_value > 0) config.scale = parsed_value;
                    else if (key == "heat_map_scale" && parsed_value > 0) config.heat_map_scale = parsed_value;
                    else if (key == "mineral_count" && parsed_value >= 0) config.mineral_count = parsed_value;
                }
            }
            catch (const exception& error) {
                throw runtime_error("Invalid world_consts value for '" + key + "': " + error.what());
            }
        }

        if (config.map_width <= 0 || config.map_height <= 0 ||
            config.octaves <= 0 || config.scale <= 0 ||
            config.heat_map_scale <= 0 ||
            config.deep_water_boundary <= 0.0 ||
            config.shallow_water_boundary <= config.deep_water_boundary ||
            config.beach_boundary <= config.shallow_water_boundary ||
            config.plains_boundary <= config.beach_boundary ||
            config.mountains_boundary <= config.plains_boundary ||
            config.mineral_count < 0) {
            throw runtime_error("world_consts.txt has missing or invalid required values");
        }

        for (size_t region = 0; region < WorldGenerationConfig::tree_region_count; ++region) {
            const double density = config.tree_density[region];
            const double clumpiness = config.tree_clumpiness[region];

            if (!isfinite(density) || density < 0.0 || density > 1.0) {
                throw runtime_error(
                    string(tree_region_names[region]) +
                    "_tree_density must be between 0.0 and 1.0"
                );
            }

            if (!isfinite(clumpiness) || clumpiness < 0.0 || clumpiness > 1.0) {
                throw runtime_error(
                    string(tree_region_names[region]) +
                    "_tree_clumpiness must be between 0.0 and 1.0"
                );
            }
        }

        return config;
    }


}

// Полный сброс мира: вместе с картами уничтожаются все объекты и чанки.
void Game::start()
{
    world_ = World{};

    const WorldGenerationConfig config = load_world_consts();
    const int map_height = config.map_height;
    const int map_width = config.map_width;
    const int octaves = config.octaves;
    world_.set_generation_config(config);
    world_.generate_height_map(
        map_height,
        map_width,
        octaves,
        config.scale
    );
    world_.generate_continent_map();
    world_.generate_heat_map(
        map_height,
        map_width,
        octaves,
        config.heat_map_scale
    );
    world_.generate_tile_map(map_height, map_width);
    world_.generate_humidity_map(map_height, map_width, octaves, config.scale);
    world_.generate_biome_map(map_height, map_width);
    world_.create_initial_items(static_cast<size_t>(config.mineral_count));
}

void Game::run()
{
    Game::start();

    // Не даём исключениям пересекать границу оконной процедуры WinAPI.
    set_map_regeneration_callback(
        [](void* user_data) -> bool {
            try {
                static_cast<Game*>(user_data)->start();
                return true;
            }
            catch (const exception& error) {
                cerr << "Map regeneration failed: " << error.what() << '\n';
                return false;
            }
        },
        this
    );

    render_.draw(world_);
    set_map_regeneration_callback(nullptr, nullptr);
}