#pragma once

#include "Chunk.h"
#include "WorldObjects.h"
#include <cstddef>
#include <vector>

struct WorldGenerationConfig {
    int map_width = 512;
    int map_height = 512;
    int octaves = 9;
    int scale = 2;
    int heat_map_scale = 1;
    double persistence = 0.5;
    double deep_water_boundary = 0.5;
    double shallow_water_boundary = 0.6;
    double beach_boundary = 0.65;
    double plains_boundary = 0.85;
    double mountains_boundary = 0.95;
    int tree_count = 200;
    int tree_variant_count = 3;
    int tree_width_tiles = 3;
    int tree_narrow_variant = 2;
    int tree_narrow_width_tiles = 2;
    int tree_height_tiles = 3;
    int placement_attempt_multiplier = 30;
    int placement_attempt_padding = 100;
    double continent_min_area_fraction = 0.015;

    // Continent cutting / bridge parameters
    int continent_cut_count_area_multiple = 100;
    double continent_cut_sand_coefficient = 1.0;
    double continent_cut_plains_coefficient = 1.0;
    double continent_cut_mountain_coefficient = 2.0;
    int continent_cut_candidates_per_angle = 5;
    double continent_cut_trace_step = 0.5;
    int continent_cut_validation_batch_size = 64;
    double continent_cut_min_split_fraction = 0.25;
    int continent_cut_band_radius = 1;
    double continent_cut_endpoint_max_distance = 50.0;
    double continent_cut_snow_coefficient = 1.0;
    double continent_cut_offset_step = 0.5;
    double continent_cut_angle_step_degrees = 5.0;
    double continent_cut_trace_margin = 2.0;
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
    void generate_continent_map();
    void create_initial_items(const std::vector<InitialItem>& items);
    void update_chunks(double delta_time);

    const std::vector<std::vector<double>>& get_height_map() const;
    const std::vector<std::vector<double>>& get_heat_map() const;
    const std::vector<std::vector<int>>& get_tiled_map() const;
    const std::vector<std::vector<int>>& get_continent_map() const;
    const std::vector<std::vector<int>>& get_original_continent_map() const;
    const std::vector<std::vector<unsigned char>>& get_continent_bridge_map() const;
    const std::vector<std::uint32_t>& get_continent_colors() const;
    const std::vector<std::uint32_t>& get_original_continent_colors() const;
    const std::vector<WorldObject>& get_objects() const;
    const std::vector<Chunk>& get_chunks() const;


private:
    void generate_trees(std::size_t count);
    void generate_rocks(std::size_t count);
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










