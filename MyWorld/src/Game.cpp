#include "Game.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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
        WorldGenerationConfig config;
        const filesystem::path file_path = find_world_consts_file();
        if (file_path.empty()) return config;

        ifstream file(file_path);
        string line;
        while (getline(file, line)) {
            const size_t separator = line.find('=');
            if (separator == string::npos) continue;

            const string key = line.substr(0, separator);
            const string value = line.substr(separator + 1);
            try {
                if (key == "persistence" ||
                    key == "deep_water_boundary" || key == "shallow_water_boundary" ||
                    key == "beach_boundary" || key == "plains_boundary" || key == "mountains_boundary" ||
                    key == "continent_min_area_fraction") {
                    const double parsed_value = stod(value);
                    if (!isfinite(parsed_value)) continue;
                    if (key == "persistence" && parsed_value >= 0.0) config.persistence = parsed_value;
                    else if (key == "deep_water_boundary") config.deep_water_boundary = parsed_value;
                    else if (key == "shallow_water_boundary") config.shallow_water_boundary = parsed_value;
                    else if (key == "beach_boundary") config.beach_boundary = parsed_value;
                    else if (key == "plains_boundary") config.plains_boundary = parsed_value;
                    else if (key == "mountains_boundary") config.mountains_boundary = parsed_value;
                    else if (key == "continent_min_area_fraction" && parsed_value >= 0.0 && parsed_value < 1.0)
                        config.continent_min_area_fraction = parsed_value;
                } else {
                    const int parsed_value = stoi(value);
                    if (key == "map_height" && parsed_value > 0) config.map_height = parsed_value;
                    else if (key == "map_width" && parsed_value > 0) config.map_width = parsed_value;
                    else if (key == "octaves" && parsed_value > 0) config.octaves = parsed_value;
                    else if (key == "scale" && parsed_value > 0) config.scale = parsed_value;
                    else if (key == "heat_map_scale" && parsed_value > 0) config.heat_map_scale = parsed_value;
                    else if (key == "tree_count" && parsed_value >= 0) config.tree_count = parsed_value;
                    else if (key == "tree_variant_count" && parsed_value > 0) config.tree_variant_count = parsed_value;
                    else if (key == "tree_width_tiles" && parsed_value > 0) config.tree_width_tiles = parsed_value;
                    else if (key == "tree_narrow_variant" && parsed_value >= 0) config.tree_narrow_variant = parsed_value;
                    else if (key == "tree_narrow_width_tiles" && parsed_value > 0) config.tree_narrow_width_tiles = parsed_value;
                    else if (key == "tree_height_tiles" && parsed_value > 0) config.tree_height_tiles = parsed_value;
                    else if (key == "placement_attempt_multiplier" && parsed_value >= 0) config.placement_attempt_multiplier = parsed_value;
                    else if (key == "placement_attempt_padding" && parsed_value >= 0) config.placement_attempt_padding = parsed_value;
                }
            } catch (const exception&) {
                // Invalid entries leave the corresponding default value in place.
            }
        }
        return config;
    }
}

void Game::start() 
{
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
    world_.generate_heat_map(
        map_height,
        map_width,
        octaves,
        config.heat_map_scale
    );
    world_.generate_tile_map(map_height, map_width);
    world_.generate_continent_map();
    world_.create_initial_items({
        { WorldObjectType::Tree, static_cast<size_t>(config.tree_count) }
    });
}

void Game::run()
{
    Game::start();
    render_.draw(world_);
}
