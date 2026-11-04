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
        const filesystem::path file_path = find_world_consts_file();
        if (file_path.empty())
            throw runtime_error("world_consts.txt not found");

        ifstream file(file_path);
        if (!file)
            throw runtime_error("Failed to open world_consts.txt: " + file_path.string());

        WorldGenerationConfig config{};
        string line;
        while (getline(file, line)) {
            const size_t separator = line.find('=');
            if (separator == string::npos) continue;

            string key = line.substr(0, separator);
            string value = line.substr(separator + 1);

            // Удаляем пробелы и невидимые символы с концов строк
            auto trim = [](string& s) {
                while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
                while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
                };
            trim(key);
            trim(value);

            if (key.empty()) continue;

            try {
                if (key == "persistence" ||
                    key == "deep_water_boundary" || key == "shallow_water_boundary" ||
                    key == "beach_boundary" || key == "plains_boundary" || key == "mountains_boundary" ||
                    key == "continent_min_area_fraction" ||
                    key == "rock_chance" || key == "copper_chance" || key == "iron_chance" ||
                    key == "gold_chance" || key == "platinum_chance")
                {
                    const double parsed_value = stod(value);
                    if (!isfinite(parsed_value)) continue;
                    if (key == "persistence" && parsed_value >= 0.0) config.persistence = parsed_value;
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
                }
                else {
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
                    else if (key == "mineral_count" && parsed_value >= 0) config.mineral_count = parsed_value; // Читаем минералы
                }
            }
            catch (const exception&) {
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
            config.tree_variant_count <= 0 ||
            config.tree_width_tiles <= 0 ||
            config.tree_narrow_width_tiles <= 0 ||
            config.tree_height_tiles <= 0) {
            throw runtime_error("world_consts.txt has missing or invalid required values");
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
    world_.create_initial_items({
        { WorldObjectType::Tree, static_cast<size_t>(config.tree_count) },
        { WorldObjectType::Mineral, static_cast<size_t>(config.mineral_count) }
        });
}

void Game::run()
{
    Game::start();
    render_.draw(world_);
}