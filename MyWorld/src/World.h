#pragma once

#include "Chunk.h"
#include "WorldObjects.h"
#include <cstddef>
#include <vector>

#include <fstream>
#include <string>
#include <unordered_map>
#include <cctype>

struct WorldGenerationConfig {
    int    map_width;
    int    map_height;
    int    octaves;
    int    scale;
    int    heat_map_scale;
    double persistence;
    double deep_water_boundary;
    double shallow_water_boundary;
    double beach_boundary;
    double plains_boundary;
    double mountains_boundary;
    int    tree_count;
    int    tree_variant_count;
    int    tree_width_tiles;
    int    tree_narrow_variant;
    int    tree_narrow_width_tiles;
    int    tree_height_tiles;
    int    placement_attempt_multiplier;
    int    placement_attempt_padding;
    // Ore variant chances (should sum to ~1.0; normalized automatically).
    int mineral_count;
    double rock_chance;
    double copper_chance;
    double iron_chance;
    double gold_chance;
    double platinum_chance;

    static WorldGenerationConfig load(const std::string& path = "world_consts.txt") {
        WorldGenerationConfig c{};

        std::ifstream f(path);
        if (!f) return c;

        std::unordered_map<std::string, std::string> kv;
        std::string line;
        while (std::getline(f, line)) {
            auto hash = line.find_first_of("#;");
            if (hash != std::string::npos) line.erase(hash);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto key = line.substr(0, eq);
            auto val = line.substr(eq + 1);
            auto trim = [](std::string& s) {
                auto ws = [](char ch) { return std::isspace((unsigned char)ch); };
                while (!s.empty() && ws(s.front())) s.erase(s.begin());
                while (!s.empty() && ws(s.back()))  s.pop_back();
                };
            trim(key); trim(val);
            kv[key] = val;
        }

        auto get = [&](const char* k, auto& field) {
            auto it = kv.find(k);
            if (it == kv.end()) return;
            using T = std::decay_t<decltype(field)>;
            if      constexpr (std::is_same_v<T, int>)    field = std::stoi(it->second);
            else if constexpr (std::is_same_v<T, double>) field = std::stod(it->second);
            };

        get("map_width", c.map_width);
        get("map_height", c.map_height);
        get("octaves", c.octaves);
        get("scale", c.scale);
        get("heat_map_scale", c.heat_map_scale);
        get("persistence", c.persistence);
        get("deep_water_boundary", c.deep_water_boundary);
        get("shallow_water_boundary", c.shallow_water_boundary);
        get("beach_boundary", c.beach_boundary);
        get("plains_boundary", c.plains_boundary);
        get("mountains_boundary", c.mountains_boundary);
        get("mineral_count", c.mineral_count);
        get("rock_chance", c.rock_chance);      
        get("copper_chance", c.copper_chance);
        get("iron_chance", c.iron_chance);
        get("gold_chance", c.gold_chance);
        get("platinum_chance", c.platinum_chance);
        get("tree_count", c.tree_count);
        get("tree_variant_count", c.tree_variant_count);
        get("tree_width_tiles", c.tree_width_tiles);
        get("tree_narrow_variant", c.tree_narrow_variant);
        get("tree_narrow_width_tiles", c.tree_narrow_width_tiles);
        get("tree_height_tiles", c.tree_height_tiles);
        get("placement_attempt_multiplier", c.placement_attempt_multiplier);
        get("placement_attempt_padding", c.placement_attempt_padding);

        return c;
    }
};

std::vector<std::vector<double>> generate_value_noise(
    int height, int width, int octaves, int scale = 1, double persistence = 0.5
);
class World
{
public:
    World() = default;

    void set_generation_config(const WorldGenerationConfig& config);
    void generate_height_map(int height, int width, int octaves, int scale = 0);
    void generate_heat_map(int height, int width, int octaves, int scale = 0);
    void generate_tile_map(int height, int width);
    void create_initial_items(const std::vector<InitialItem>& items);
    void update_chunks(double delta_time);

    const std::vector<std::vector<double>>& get_height_map() const;
    const std::vector<std::vector<double>>& get_heat_map() const;
    const std::vector<std::vector<int>>& get_tiled_map() const;
    const std::vector<WorldObject>& get_objects() const;
    const std::vector<Chunk>& get_chunks() const;


private:
    void generate_trees(std::size_t count);
    void generate_minerals(std::size_t count);
    void add_object(WorldObject object);
    bool can_place_object(int x, int y, int width, int height, WorldObjectType type) const;
    std::size_t chunk_index(int chunk_x, int chunk_y) const;

    int map_width_{};
    int map_height_{};
    int chunks_x_{};
    int chunks_y_{};
    ObjectId next_object_id_{ 1 };
    WorldGenerationConfig generation_config_;
    std::vector<std::vector<double>> height_map_;
    std::vector<std::vector<double>> heat_map_;
    std::vector<std::vector<int>> tiled_map_;
    std::vector<std::vector<int>> continent_map_;
    std::vector<std::vector<int>> original_continent_map_;
    std::vector<std::vector<unsigned char>> continent_bridge_map_;
    std::vector<std::uint32_t> continent_colors_;
    std::vector<std::uint32_t> original_continent_colors_;
    std::vector<Chunk> chunks_;
    std::vector<WorldObject> objects_;
};










