#include "World.h"
#include <iostream>
#include <random>
#include <vector>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
#include <thread>
#include <stdexcept>
#include <ppl.h>

using namespace std;

namespace {
    mt19937 random_engine{ random_device{}() };

    template <typename Function>
    void parallel_for_range(size_t count, Function&& function) {
        if (count == 0) return;

        const unsigned int hardware_threads = max(1u, thread::hardware_concurrency());
        const size_t worker_count = min(count, static_cast<size_t>(hardware_threads));
        if (worker_count == 1) {
            function(0, count);
            return;
        }

        const auto run_worker = [&](size_t worker_index) {
            const size_t begin = count * worker_index / worker_count;
            const size_t end = count * (worker_index + 1) / worker_count;
            function(begin, end);
            };

        vector<thread> workers;
        workers.reserve(worker_count - 1);
        for (size_t worker = 1; worker < worker_count; ++worker)
            workers.emplace_back(run_worker, worker);
        run_worker(0);
        for (thread& worker : workers)
            worker.join();
    }

    double smooth_interpolate(double start, double end, double amount) {
        const double smooth_amount = amount * amount * (3.0 - 2.0 * amount);
        return start + (end - start) * smooth_amount;
    }

    void rank_normalize(vector<vector<double>>& noise) {
        if (noise.empty() || noise.front().empty()) return;

        const size_t height = noise.size();
        const size_t width = noise.front().size();
        const size_t value_count = height * width;
        vector<size_t> sorted_indices(value_count);
        iota(sorted_indices.begin(), sorted_indices.end(), 0);
        concurrency::parallel_sort(sorted_indices.begin(), sorted_indices.end(), [&](size_t left, size_t right) {
            const double left_value = noise[left / width][left % width];
            const double right_value = noise[right / width][right % width];
            if (left_value == right_value) return left < right;
            return left_value < right_value;
            });
        parallel_for_range(value_count, [&](size_t first_rank, size_t end_rank) {
            for (size_t rank = first_rank; rank < end_rank; ++rank) {
                const size_t index = sorted_indices[rank];
                noise[index / width][index % width] =
                    (static_cast<double>(rank) + 0.5) / value_count;
            }
            });
    }
}

vector<vector<double>> generate_white_noise(int height, int width) {
    if (height <= 0 || width <= 0)
        return {};

    uniform_real_distribution<double> distribution(0.0, 1.0);
    vector<vector<double>> noise(height, vector<double>(width, 0.0));
    for (auto& row : noise)
        for (double& value : row)
            value = distribution(random_engine);
    return noise;
}

vector<vector<double>> generate_value_noise(
    int height, int width, int octaves, int scale, double persistence
) {
    if (height <= 0 || width <= 0 || octaves <= 0)
        return {};

    const double finest_scale = pow(2.0, octaves - 1);
    const int first_octave_scale = max(1, static_cast<int>(finest_scale) / max(1, scale));
    if (first_octave_scale > min(width, height))
        return {};

    vector<vector<vector<double>>> octave_maps(octaves);
    vector<int> octave_scales(octaves);
    int a = first_octave_scale;
    for (int i = 0; i < octaves; i++) {
        octave_scales[i] = a;
        const int octave_height = (height - 1 + a - 1) / a + 1;
        const int octave_width = (width - 1 + a - 1) / a + 1;
        octave_maps[i] = generate_white_noise(octave_height, octave_width);
        a = max(1, a / 2);
    }

    vector<vector<double>> noise(height, vector<double>(width, 0.0));
    vector<double> octave_amplitudes(octave_maps.size());
    double amplitude = 1.0;
    double total_amplitude = 0.0;
    for (size_t octave = 0; octave < octave_maps.size(); ++octave) {
        octave_amplitudes[octave] = amplitude;
        total_amplitude += amplitude;
        amplitude *= persistence;
    }

    parallel_for_range(static_cast<size_t>(height), [&](size_t first_row, size_t end_row) {
        for (size_t row = first_row; row < end_row; ++row) {
            const int y = static_cast<int>(row);
            for (int x = 0; x < width; ++x) {
                double value = 0.0;
                for (size_t octave = 0; octave < octave_maps.size(); ++octave) {
                    const auto& octave_map = octave_maps[octave];
                    const int octave_height = static_cast<int>(octave_map.size());
                    const int octave_width = static_cast<int>(octave_map.front().size());
                    const int octave_scale = octave_scales[octave];
                    const int y0 = min(y / octave_scale, octave_height - 1);
                    const int x0 = min(x / octave_scale, octave_width - 1);
                    const int y1 = min(y0 + 1, octave_height - 1);
                    const int x1 = min(x0 + 1, octave_width - 1);

                    const int y_start = y0 * octave_scale;
                    const int x_start = x0 * octave_scale;
                    const int y_end = y1 * octave_scale;
                    const int x_end = x1 * octave_scale;
                    const double y_amount = y_end == y_start
                        ? 0.0 : static_cast<double>(y - y_start) / (y_end - y_start);
                    const double x_amount = x_end == x_start
                        ? 0.0 : static_cast<double>(x - x_start) / (x_end - x_start);

                    const double top = smooth_interpolate(
                        octave_map[y0][x0], octave_map[y0][x1], x_amount
                    );
                    const double bottom = smooth_interpolate(
                        octave_map[y1][x0], octave_map[y1][x1], x_amount
                    );
                    value += smooth_interpolate(top, bottom, y_amount) * octave_amplitudes[octave];
                }
                noise[y][x] = value / total_amplitude;
            }
        }
        });

    // Ранговая перенормировка держит доли биомов предсказуемыми.
    rank_normalize(noise);

    return noise;
}

void World::set_generation_config(const WorldGenerationConfig& config) {
    generation_config_ = config;
}

void World::generate_height_map(int height, int width, int octaves, int scale) {
    if (height <= 0 || width <= 0) {
        height_map_.clear();
        return;
    }

    if (generation_config_.deep_water_boundary <= 0.0)
        throw runtime_error("deep_water_boundary must be greater than zero to find an ocean border.");
    const int noise_scale = scale > 0 ? scale : generation_config_.scale;
    height_map_ = generate_value_noise(
        height,
        width,
        octaves,
        noise_scale,
        generation_config_.persistence
    );
    if (height_map_.empty() || height_map_.front().empty()) return;

    // Keep the generated land inside an irregular, oval-shaped envelope. The
    // terrain noise bends the coast so it does not read as a rectangular frame.
    const double fade_width = 0.10;
    parallel_for_range(static_cast<size_t>(height), [&](size_t first_row, size_t end_row) {
        for (size_t row = first_row; row < end_row; ++row) {
            const double normalized_y =
                (static_cast<double>(row) - (height - 1) * 0.5) / (height * 0.5);
            for (int x = 0; x < width; ++x) {
                const double normalized_x =
                    (static_cast<double>(x) - (width - 1) * 0.5) / (width * 0.5);
                const double radius = sqrt(normalized_x * normalized_x + normalized_y * normalized_y);
                const double terrain = height_map_[row][x];
                const double coast_start = 0.72 + terrain * 0.18;
                const double fade_amount = clamp((radius - coast_start) / fade_width, 0.0, 1.0);
                const double coast_fade = fade_amount * fade_amount * (3.0 - 2.0 * fade_amount);
                height_map_[row][x] = terrain * (1.0 - coast_fade);
            }
        }
        });
    rank_normalize(height_map_);
}

void World::generate_heat_map(int height, int width, int octaves, int scale) {
    heat_map_ = generate_value_noise(
        height, width, octaves,
        scale > 0 ? scale : generation_config_.heat_map_scale,
        generation_config_.persistence
    );
}

void World::generate_tile_map(int height, int width) {
    if (height <= 0 || width <= 0 ||
        height_map_.size() != static_cast<size_t>(height) ||
        height_map_.empty() ||
        height_map_.front().size() != static_cast<size_t>(width)) {
        tiled_map_.clear();
        continent_map_.clear();
        continent_colors_.clear();
        chunks_.clear();
        map_width_ = map_height_ = chunks_x_ = chunks_y_ = 0;
        return;
    }

    map_width_ = width;
    map_height_ = height;
    chunks_x_ = (width + Chunk::size_in_tiles - 1) / Chunk::size_in_tiles;
    chunks_y_ = (height + Chunk::size_in_tiles - 1) / Chunk::size_in_tiles;
    chunks_.clear();
    chunks_.resize(static_cast<size_t>(chunks_x_) * chunks_y_);
    objects_.clear();
    continent_map_.clear();
    continent_colors_.clear();
    next_object_id_ = 1;
    tiled_map_.assign(height, vector<int>(width, 0));

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (height_map_[y][x] <= generation_config_.deep_water_boundary)
                tiled_map_[y][x] = 0;
            else if (height_map_[y][x] <= generation_config_.shallow_water_boundary)
                tiled_map_[y][x] = 1;
            else if (height_map_[y][x] <= generation_config_.beach_boundary)
                tiled_map_[y][x] = 2;
            else if (height_map_[y][x] <= generation_config_.plains_boundary)
                tiled_map_[y][x] = 3;
            else if (height_map_[y][x] <= generation_config_.mountains_boundary)
                tiled_map_[y][x] = 4;
            else
                tiled_map_[y][x] = 5;
        }
    }
}

void World::create_initial_items(const vector<InitialItem>& items) {
    for (const auto& [type, count] : items) {
        switch (type) {
        case WorldObjectType::Tree:
            generate_trees(count);
            break;
        case WorldObjectType::Rock:
            generate_rocks(count);
            break;
        }
    }
}

void World::generate_trees(size_t count) {
    if (map_width_ <= 0 || map_height_ <= 0 || tiled_map_.empty()) return;
    uniform_int_distribution<int> x_dist(0, map_width_ - 1);
    uniform_int_distribution<int> y_dist(0, map_height_ - 1);
    if (generation_config_.tree_variant_count <= 0) return;
    uniform_int_distribution<int> variant_dist(0, generation_config_.tree_variant_count - 1);
    const size_t max_attempts = count * static_cast<size_t>(max(0, generation_config_.placement_attempt_multiplier)) +
        static_cast<size_t>(max(0, generation_config_.placement_attempt_padding));
    size_t attempts = 0;
    while (count > 0 && attempts++ < max_attempts) {
        const int x = x_dist(random_engine);
        const int y = y_dist(random_engine);
        const int variant = variant_dist(random_engine);
        const int object_width = variant == generation_config_.tree_narrow_variant
            ? generation_config_.tree_narrow_width_tiles : generation_config_.tree_width_tiles;
        const int object_height = generation_config_.tree_height_tiles;
        bool overlaps_tree = false;
        for (const WorldObject& object : objects_) {
            if (object.type != WorldObjectType::Tree) continue;
            const bool overlaps_x = x < object.x + object.width && object.x < x + object_width;
            const bool overlaps_y = y < object.y + object.height && object.y < y + object_height;
            if (overlaps_x && overlaps_y) {
                overlaps_tree = true;
                break;
            }
        }
        if (!overlaps_tree &&
            can_place_object(x, y, object_width, object_height, WorldObjectType::Tree)) {
            add_object({ 0, WorldObjectType::Tree, x, y, object_width, object_height, false, variant });
            --count;
        }
    }
}

void World::generate_rocks(size_t count) {
    if (map_width_ <= 0 || map_height_ <= 0 || tiled_map_.empty()) return;
    uniform_int_distribution<int> x_dist(0, map_width_ - 1);
    uniform_int_distribution<int> y_dist(0, map_height_ - 1);
    uniform_int_distribution<int> variant_dist(0, 1);
    const size_t max_attempts = count * 30 + 100;
    size_t attempts = 0;
    while (count > 0 && attempts++ < max_attempts) {
        const int x = x_dist(random_engine);
        const int y = y_dist(random_engine);
        if (can_place_object(x, y, 1, 1, WorldObjectType::Rock)) {
            add_object({ 0, WorldObjectType::Rock, x, y, 1, 1, false, variant_dist(random_engine) });
            --count;
        }
    }
}

bool World::can_place_object(int x, int y, int width, int height, WorldObjectType type) const {
    if (width <= 0 || height <= 0 || x < 0 || y < 0 ||
        x + width > map_width_ || y + height > map_height_) return false;

    for (int tile_y = y; tile_y < y + height; ++tile_y) {
        for (int tile_x = x; tile_x < x + width; ++tile_x) {
            const int tile = tiled_map_[tile_y][tile_x];
            if (type == WorldObjectType::Tree && tile != 3) return false;
            if (type == WorldObjectType::Rock && tile != 4 && tile != 5) return false;
        }
    }
    return true;
}

size_t World::chunk_index(int chunk_x, int chunk_y) const {
    return static_cast<size_t>(chunk_y) * chunks_x_ + chunk_x;
}

void World::add_object(WorldObject object) {
    object.id = next_object_id_++;
    objects_.push_back(object);

    const int first_chunk_x = object.x / Chunk::size_in_tiles;
    const int first_chunk_y = object.y / Chunk::size_in_tiles;
    const int last_chunk_x = (object.x + object.width - 1) / Chunk::size_in_tiles;
    const int last_chunk_y = (object.y + object.height - 1) / Chunk::size_in_tiles;
    for (int cy = first_chunk_y; cy <= last_chunk_y; ++cy)
        for (int cx = first_chunk_x; cx <= last_chunk_x; ++cx)
            chunks_[chunk_index(cx, cy)].add_object(object.id, object.is_dynamic);
}

void World::update_chunks(double delta_time) {
    for (Chunk& chunk : chunks_) chunk.update(delta_time);
}

const vector<WorldObject>& World::get_objects() const { return objects_; }
const vector<Chunk>& World::get_chunks() const { return chunks_; }

const vector<vector<double>>& World::get_height_map() const {
    return height_map_;
}
const vector<vector<double>>& World::get_heat_map() const {
    return heat_map_;
}
const vector<vector<int>>& World::get_tiled_map() const {
    return tiled_map_;
}