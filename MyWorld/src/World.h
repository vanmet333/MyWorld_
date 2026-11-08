#pragma once

#include "Chunk.h"
#include "WorldObjects.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace std;

struct WorldGenerationConfig {
    int map_width{};
    int map_height{};
    int octaves{};
    int scale{};
    int heat_map_scale{};

    double persistence{};
    double deep_water_boundary{};
    double shallow_water_boundary{};
    double beach_boundary{};
    double plains_boundary{};
    double mountains_boundary{};

    // Распределение деревьев по биомам:
    // 0 snow_plains, 1 snow_taiga, 2 taiga, 3 plains, 4 forest,
    // 5 dark_forest, 6 desert, 7 savanna, 8 jungle, 9 beach.
    // density — доля допустимых точек размещения (0.001 = примерно 1 дерево на 1000 точек).
    // clumpiness: 0 = равномерно, 1 = выраженные группы.
    static constexpr size_t tree_region_count = 10;
    array<double, tree_region_count> tree_density{
        0.0007, 0.0012, 0.0015, 0.0005, 0.0015,
        0.0020, 0.00025, 0.0008, 0.0020, 0.0003
    };
    array<double, tree_region_count> tree_clumpiness{
        0.35, 0.45, 0.45, 0.15, 0.45,
        0.65, 0.25, 0.55, 0.60, 0.35
    };

    // Ore variant chances and total amount.
    int mineral_count{};
    double rock_chance{};
    double copper_chance{};
    double iron_chance{};
    double gold_chance{};
    double platinum_chance{};

    // Climate thresholds.
    double cold_biome_boundary{};
    double hot_biome_boundary{};
    double dry_biome_boundary{};
    double wet_biome_boundary{};
    double min_biome_size{};
};

vector<vector<double>> generate_value_noise(
    int height, int width, int octaves, int scale = 1, double persistence = 0.5
);

class World {
public:
    World() = default;

    void set_generation_config(const WorldGenerationConfig& config);
    void generate_height_map(int height, int width, int octaves, int scale = 0);
    void generate_heat_map(int height, int width, int octaves, int scale = 0);
    void generate_tile_map(int height, int width);
    void create_initial_items(size_t mineral_count);
    void update_chunks(double delta_time);
    void generate_continent_map();
    void generate_humidity_map(int height, int width, int octaves, int scale);
    void generate_biome_map(int height, int width);

    const vector<vector<int>>& get_continent_map() const;
    const vector<uint32_t>& get_continent_colors() const;
    const vector<vector<double>>& get_height_map() const;
    const vector<vector<double>>& get_heat_map() const;
    const vector<vector<int>>& get_biome_map() const;
    const vector<vector<int>>& get_tiled_map() const;
    const vector<WorldObject>& get_objects() const;
    const vector<Chunk>& get_chunks() const;
    const vector<vector<double>>& get_humidity_map() const;

private:
    void generate_trees();
    void generate_minerals(size_t count);
    void add_object(WorldObject object);
    bool can_place_object(
        int x, int y, int width, int height,
        WorldObjectType type, int required_tree_region = -1
    ) const;
    size_t chunk_index(int chunk_x, int chunk_y) const;

    int map_width_{};
    int map_height_{};
    int chunks_x_{};
    int chunks_y_{};
    ObjectId next_object_id_{ 1 };
    WorldGenerationConfig generation_config_;
    vector<vector<double>> height_map_;
    vector<vector<double>> heat_map_;
    vector<vector<int>> tiled_map_;
    vector<vector<int>> continent_map_;
    vector<vector<double>> humidity_map_;
    vector<vector<int>> biome_map_;
    vector<uint32_t> continent_colors_;
    vector<Chunk> chunks_;
    vector<WorldObject> objects_;
};
