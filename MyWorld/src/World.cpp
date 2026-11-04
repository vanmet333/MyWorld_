#include "World.h"
#include <iostream>
#include <random>
#include <vector>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>
#include <thread>
#include <iterator>
#include <mutex>
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
        original_continent_map_.clear();
        continent_bridge_map_.clear();
        continent_colors_.clear();
        original_continent_colors_.clear();
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
    original_continent_map_.clear();
    continent_bridge_map_.clear();
    continent_colors_.clear();
    original_continent_colors_.clear();
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

void World::generate_continent_map() {
    continent_map_.clear();
    original_continent_map_.clear();
    continent_bridge_map_.clear();
    continent_colors_.clear();
    original_continent_colors_.clear();
    if (tiled_map_.empty() || tiled_map_.front().empty()) return;

    const int height = static_cast<int>(tiled_map_.size());
    const int width = static_cast<int>(tiled_map_.front().size());
    continent_map_.assign(height, vector<int>(width, -1));
    continent_bridge_map_.assign(height, vector<unsigned char>(width, 0));
    uniform_int_distribution<unsigned int> color_channel(75, 235);
    constexpr int neighbor_x[4] = { 1, -1, 0, 0 };
    constexpr int neighbor_y[4] = { 0, 0, 1, -1 };

    vector<int> continent_sizes;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (tiled_map_[y][x] < 2 || continent_map_[y][x] >= 0) continue;

            const int continent_id = static_cast<int>(continent_colors_.size());
            const uint32_t red = color_channel(random_engine);
            const uint32_t green = color_channel(random_engine);
            const uint32_t blue = color_channel(random_engine);
            continent_colors_.push_back((red << 16) | (green << 8) | blue);
            continent_sizes.push_back(0);

            vector<pair<int, int>> frontier{ { x, y } };
            continent_map_[y][x] = continent_id;
            while (!frontier.empty()) {
                const auto [tile_x, tile_y] = frontier.back();
                frontier.pop_back();
                ++continent_sizes[continent_id];
                for (int direction = 0; direction < 4; ++direction) {
                    const int next_x = tile_x + neighbor_x[direction];
                    const int next_y = tile_y + neighbor_y[direction];
                    if (next_x < 0 || next_x >= width || next_y < 0 || next_y >= height ||
                        tiled_map_[next_y][next_x] < 2 || continent_map_[next_y][next_x] >= 0)
                        continue;
                    continent_map_[next_y][next_x] = continent_id;
                    frontier.push_back({ next_x, next_y });
                }
            }
        }
    }

    const int minimum_continent_area = max(1, static_cast<int>(ceil(
        static_cast<double>(height) * width * generation_config_.continent_min_area_fraction
    )));
    const size_t maximum_enclosed_water_area = static_cast<size_t>(minimum_continent_area);
    vector<unsigned char> visited_water(static_cast<size_t>(height) * width, 0);
    vector<int> water_frontier;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int start = y * width + x;
            if (tiled_map_[y][x] >= 2 || visited_water[start]) continue;

            water_frontier.assign(1, start);
            visited_water[start] = 1;
            bool touches_map_edge = false;
            bool touches_multiple_continents = false;
            int surrounding_continent = -1;
            for (size_t head = 0; head < water_frontier.size(); ++head) {
                const int index = water_frontier[head];
                const int water_x = index % width;
                const int water_y = index / width;
                if (water_x == 0 || water_x == width - 1 || water_y == 0 || water_y == height - 1)
                    touches_map_edge = true;

                for (int direction = 0; direction < 4; ++direction) {
                    const int next_x = water_x + neighbor_x[direction];
                    const int next_y = water_y + neighbor_y[direction];
                    if (next_x < 0 || next_x >= width || next_y < 0 || next_y >= height) continue;
                    if (tiled_map_[next_y][next_x] < 2) {
                        const int next = next_y * width + next_x;
                        if (!visited_water[next]) {
                            visited_water[next] = 1;
                            water_frontier.push_back(next);
                        }
                        continue;
                    }

                    const int adjacent_continent = continent_map_[next_y][next_x];
                    if (surrounding_continent < 0)
                        surrounding_continent = adjacent_continent;
                    else if (surrounding_continent != adjacent_continent)
                        touches_multiple_continents = true;
                }
            }

            if (water_frontier.size() > maximum_enclosed_water_area || touches_map_edge ||
                touches_multiple_continents || surrounding_continent < 0) continue;

            for (const int index : water_frontier)
                continent_map_[index / width][index % width] = surrounding_continent;
            continent_sizes[surrounding_continent] += static_cast<int>(water_frontier.size());
        }
    }

    vector<int> continent_remap(continent_sizes.size(), -1);
    vector<int> large_continent_sizes;
    vector<uint32_t> large_continent_colors;
    for (size_t old_id = 0; old_id < continent_sizes.size(); ++old_id) {
        if (continent_sizes[old_id] < minimum_continent_area) continue;
        continent_remap[old_id] = static_cast<int>(large_continent_sizes.size());
        large_continent_sizes.push_back(continent_sizes[old_id]);
        large_continent_colors.push_back(continent_colors_[old_id]);
    }
    for (auto& row : continent_map_) {
        for (int& continent_id : row)
            continent_id = continent_id < 0 ? -1 : continent_remap[continent_id];
    }
    continent_sizes = move(large_continent_sizes);
    continent_colors_ = move(large_continent_colors);
    original_continent_map_ = continent_map_;
    original_continent_colors_ = continent_colors_;

    const int tile_count = height * width;
    vector<unsigned char> sea_tiles(tile_count, 0);
    vector<int> sea_frontier;
    const auto add_sea_tile = [&](int x, int y) {
        if (tiled_map_[y][x] >= 2) return;
        const int index = y * width + x;
        if (sea_tiles[index]) return;
        sea_tiles[index] = 1;
        sea_frontier.push_back(index);
    };
    for (int x = 0; x < width; ++x) {
        add_sea_tile(x, 0);
        if (height > 1) add_sea_tile(x, height - 1);
    }
    for (int y = 1; y + 1 < height; ++y) {
        add_sea_tile(0, y);
        if (width > 1) add_sea_tile(width - 1, y);
    }
    for (size_t head = 0; head < sea_frontier.size(); ++head) {
        const int index = sea_frontier[head];
        const int x = index % width;
        const int y = index / width;
        for (int direction = 0; direction < 4; ++direction) {
            const int next_x = x + neighbor_x[direction];
            const int next_y = y + neighbor_y[direction];
            if (next_x < 0 || next_x >= width || next_y < 0 || next_y >= height ||
                tiled_map_[next_y][next_x] >= 2) continue;
            const int next = next_y * width + next_x;
            if (sea_tiles[next]) continue;
            sea_tiles[next] = 1;
            sea_frontier.push_back(next);
        }
    }

    struct BridgeCut {
        double cost;
        size_t estimated_tiles;
        double normal_x;
        double normal_y;
        double offset;
        double half_width;
        double min_along;
        double max_along;
    };
    struct WaterToWaterCut {
        BridgeCut line;
        vector<int> path;
    };
    vector<vector<int>> pending_regions(continent_sizes.size());
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int continent_id = continent_map_[y][x];
            if (continent_id >= 0)
                pending_regions[continent_id].push_back(y * width + x);
        }
    }

    vector<int> member_stamp(tile_count, 0);
    int current_member_stamp = 0;

    vector<unsigned char> planned_cut_tiles(static_cast<size_t>(height) * width, 0);
    vector<unsigned char> cut_band_mask(static_cast<size_t>(height) * width, 0);
    const size_t minimum_area = static_cast<size_t>(minimum_continent_area);

    // Draw a fixed number of the lowest-cost water-to-water cuts per continent.
    for (size_t initial_id = 0; initial_id < continent_sizes.size(); ++initial_id) {
        const size_t cut_area_divisor = minimum_area * static_cast<size_t>(
            generation_config_.continent_cut_count_area_multiple
        );
        const size_t target_cut_count = cut_area_divisor == 0 ? 0 :
            static_cast<size_t>(continent_sizes[initial_id]) / cut_area_divisor;
        if (target_cut_count == 0 || pending_regions[initial_id].size() < 2 * minimum_area)
            continue;

        vector<vector<int>> regions;
        regions.push_back(move(pending_regions[initial_id]));
        size_t next_region = 0;
        size_t cuts_drawn = 0;

        while (next_region < regions.size() && cuts_drawn < target_cut_count) {
            vector<int> region = move(regions[next_region++]);
            if (region.size() < 2 * minimum_area) continue;

            ++current_member_stamp;
            int min_x = width;
            int max_x = -1;
            int min_y = height;
            int max_y = -1;
            for (const int index : region) {
                member_stamp[index] = current_member_stamp;
                const int x = index % width;
                const int y = index / width;
                min_x = min(min_x, x);
                max_x = max(max_x, x);
                min_y = min(min_y, y);
                max_y = max(max_y, y);
            }
            vector<BridgeCut> candidates;
            const auto tile_cut_cost = [&](int tile_type) {
                switch (tile_type) {
                case 2: return generation_config_.continent_cut_sand_coefficient;
                case 3: return generation_config_.continent_cut_plains_coefficient;
                case 4: return generation_config_.continent_cut_mountain_coefficient;
                case 5: return generation_config_.continent_cut_snow_coefficient;
                default: return 0.0; // water
                }
            };
            constexpr double pi = 3.14159265358979323846;
            const double offset_step = generation_config_.continent_cut_offset_step;
            const int angle_step_degrees = generation_config_.continent_cut_angle_step_degrees;
            const size_t angle_count = angle_step_degrees > 0 ?
                static_cast<size_t>((180 + angle_step_degrees - 1) / angle_step_degrees) : 0;
            vector<vector<BridgeCut>> angle_candidates(angle_count);
            const auto evaluate_angle = [&](size_t angle_index) {
                const int angle_degrees = static_cast<int>(angle_index) * angle_step_degrees;
                const double angle = angle_degrees * pi / 180.0;
                const double normal_x = -sin(angle);
                const double normal_y = cos(angle);
                const double tangent_x = normal_y;
                const double tangent_y = -normal_x;
                const double half_width = 0.5 * (fabs(normal_x) + fabs(normal_y));
                vector<double> projections;
                projections.reserve(region.size());
                double min_along = numeric_limits<double>::max();
                double max_along = numeric_limits<double>::lowest();
                for (const int index : region) {
                    const double x = index % width + 0.5;
                    const double y = index / width + 0.5;
                    projections.push_back(x * normal_x + y * normal_y);
                    const double along = x * tangent_x + y * tangent_y;
                    min_along = min(min_along, along);
                    max_along = max(max_along, along);
                }
                sort(projections.begin(), projections.end());

                const double min_projection = projections.front();
                const double max_projection = projections.back();
                size_t before_cut = 0;
                size_t after_cut = 0;
                for (double offset = min_projection; offset <= max_projection + 1e-9; offset += offset_step) {
                    const double lower_edge = offset - half_width;
                    const double upper_edge = offset + half_width;
                    while (before_cut < projections.size() && projections[before_cut] < lower_edge)
                        ++before_cut;
                    after_cut = max(after_cut, before_cut);
                    while (after_cut < projections.size() && projections[after_cut] <= upper_edge)
                        ++after_cut;

                    const size_t land_after_cut = projections.size() - after_cut;
                    if (before_cut < minimum_area || land_after_cut < minimum_area ||
                        after_cut == before_cut) continue;

                    angle_candidates[angle_index].push_back({
                        0.0, after_cut - before_cut,
                        normal_x, normal_y, offset, half_width,
                        min_along - generation_config_.continent_cut_trace_margin,
                        max_along + generation_config_.continent_cut_trace_margin
                    });
                }
            };
            const unsigned int hardware_threads = max(1u, thread::hardware_concurrency());
            const size_t angle_worker_count = min(angle_count, static_cast<size_t>(hardware_threads));
            atomic<size_t> next_angle{ 0 };
            vector<thread> angle_workers;
            angle_workers.reserve(angle_worker_count > 0 ? angle_worker_count - 1 : 0);
            const auto angle_worker = [&]() {
                size_t angle_index;
                while ((angle_index = next_angle.fetch_add(1, memory_order_relaxed)) < angle_count)
                    evaluate_angle(angle_index);
            };
            for (size_t worker = 1; worker < angle_worker_count; ++worker)
                angle_workers.emplace_back(angle_worker);
            angle_worker();
            for (thread& worker : angle_workers) worker.join();
            const size_t candidates_per_angle = static_cast<size_t>(
                max(1, generation_config_.continent_cut_candidates_per_angle));
            for (vector<BridgeCut>& per_angle : angle_candidates) {
                sort(per_angle.begin(), per_angle.end(), [](const BridgeCut& left, const BridgeCut& right) {
                    return left.estimated_tiles < right.estimated_tiles;
                });
                if (per_angle.size() > candidates_per_angle)
                    per_angle.resize(candidates_per_angle);
                candidates.insert(candidates.end(), per_angle.begin(), per_angle.end());
            }

            const auto is_cut_tile = [](int x, int y, const BridgeCut& cut) {
                const double projection = (x + 0.5) * cut.normal_x + (y + 0.5) * cut.normal_y;
                const double along = (x + 0.5) * cut.normal_y - (y + 0.5) * cut.normal_x;
                return fabs(projection - cut.offset) <= cut.half_width + 1e-9 &&
                    along >= cut.min_along && along <= cut.max_along;
            };
            const auto trace_water_to_water_paths = [&](const BridgeCut& cut) {
                vector<vector<int>> paths;
                const double tangent_x = cut.normal_y;
                const double tangent_y = -cut.normal_x;
                bool inside_region = false;
                bool previous_sample_was_water = false;
                vector<int> path;
                const double trace_step = generation_config_.continent_cut_trace_step;
                for (double along_cut = cut.min_along;
                    along_cut <= cut.max_along; along_cut += trace_step) {
                    const double point_x = cut.normal_x * cut.offset + tangent_x * along_cut;
                    const double point_y = cut.normal_y * cut.offset + tangent_y * along_cut;
                    const int tile_x = static_cast<int>(floor(point_x));
                    const int tile_y = static_cast<int>(floor(point_y));
                    const bool inside_map = tile_x >= 0 && tile_x < width && tile_y >= 0 && tile_y < height;
                    const int index = inside_map ? tile_y * width + tile_x : -1;
                    const bool on_current_region = inside_map && member_stamp[index] == current_member_stamp;
                    const bool on_water = inside_map && sea_tiles[index];

                    if (on_current_region && !inside_region) {
                        if (!previous_sample_was_water) return vector<vector<int>>{};
                        inside_region = true;
                        path.clear();
                    }
                    else if (!on_current_region && inside_region) {
                        if (!on_water) return vector<vector<int>>{};
                        inside_region = false;
                        if (!path.empty()) paths.push_back(move(path));
                        path.clear();
                    }
                    if (on_current_region && (path.empty() || path.back() != index))
                        path.push_back(index);
                    previous_sample_was_water = on_water;
                }
                if (inside_region) return vector<vector<int>>{};
                return paths;
            };
            bool found_cut = false;
            BridgeCut selected_cut{};
            vector<vector<int>> split_regions;
            vector<vector<int>> selected_paths;
            vector<vector<WaterToWaterCut>> traced_candidates(candidates.size());
            atomic<size_t> next_candidate{ 0 };
            const size_t trace_worker_count = min(candidates.size(), static_cast<size_t>(hardware_threads));
            vector<thread> trace_workers;
            trace_workers.reserve(trace_worker_count > 0 ? trace_worker_count - 1 : 0);
            const auto trace_worker = [&]() {
                size_t candidate_index;
                while ((candidate_index = next_candidate.fetch_add(1, memory_order_relaxed)) < candidates.size()) {
                    const BridgeCut& candidate = candidates[candidate_index];
                    vector<WaterToWaterCut>& local_segments = traced_candidates[candidate_index];
                    vector<vector<int>> candidate_paths = trace_water_to_water_paths(candidate);
                    for (vector<int>& path : candidate_paths) {
                        if (path.empty()) continue;
                        BridgeCut segment = candidate;
                        double min_along = numeric_limits<double>::max();
                        double max_along = numeric_limits<double>::lowest();
                        double path_cost = 0.0;
                        for (const int index : path) {
                            const double x = index % width + 0.5;
                            const double y = index / width + 0.5;
                            const double along = x * segment.normal_y - y * segment.normal_x;
                            min_along = min(min_along, along);
                            max_along = max(max_along, along);
                            path_cost += tile_cut_cost(tiled_map_[index / width][index % width]);
                        }
                        segment.cost = path_cost;
                        segment.min_along = min_along - segment.half_width;
                        segment.max_along = max_along + segment.half_width;
                        local_segments.push_back({ segment, move(path) });
                    }
                }
            };
            for (size_t worker = 1; worker < trace_worker_count; ++worker)
                trace_workers.emplace_back(trace_worker);
            trace_worker();
            for (thread& worker : trace_workers) worker.join();
            vector<WaterToWaterCut> segment_candidates;
            for (vector<WaterToWaterCut>& per_candidate : traced_candidates)
                segment_candidates.insert(segment_candidates.end(),
                    make_move_iterator(per_candidate.begin()), make_move_iterator(per_candidate.end()));
            sort(segment_candidates.begin(), segment_candidates.end(), [](const WaterToWaterCut& left,
                const WaterToWaterCut& right) {
                if (left.line.cost != right.line.cost)
                    return left.line.cost < right.line.cost;
                if (left.path.size() != right.path.size())
                    return left.path.size() < right.path.size();
                if (left.line.normal_x != right.line.normal_x)
                    return left.line.normal_x < right.line.normal_x;
                if (left.line.normal_y != right.line.normal_y)
                    return left.line.normal_y < right.line.normal_y;
                if (left.line.offset != right.line.offset)
                    return left.line.offset < right.line.offset;
                if (left.line.min_along != right.line.min_along)
                    return left.line.min_along < right.line.min_along;
                return left.line.max_along < right.line.max_along;
            });

            const size_t validation_batch_size = static_cast<size_t>(
                max(1, generation_config_.continent_cut_validation_batch_size));
            const size_t minimum_split_area = max(minimum_area, static_cast<size_t>(ceil(
                region.size() * generation_config_.continent_cut_min_split_fraction
            )));
            for (size_t batch_start = 0; batch_start < segment_candidates.size() && !found_cut;) {
                const size_t batch_end = min(segment_candidates.size(), batch_start + validation_batch_size);
                atomic<size_t> best_cut_index{ batch_end };
                mutex best_cut_mutex;
                const size_t validation_worker_count = min(batch_end - batch_start,
                    static_cast<size_t>(hardware_threads));
                vector<thread> validation_workers;
                validation_workers.reserve(validation_worker_count > 0 ? validation_worker_count - 1 : 0);
                atomic<size_t> next_segment_candidate{ batch_start };
                const auto validate_worker = [&]() {
                    vector<int> local_visited(tile_count, 0);
                    vector<int> local_frontier;
                    int local_visit_stamp = 0;
                    size_t candidate_index;
                    while ((candidate_index = next_segment_candidate.fetch_add(1, memory_order_relaxed)) < batch_end) {
                        if (candidate_index >= best_cut_index.load(memory_order_relaxed)) break;
                        const BridgeCut& candidate = segment_candidates[candidate_index].line;
                        if (local_visit_stamp == numeric_limits<int>::max()) {
                            fill(local_visited.begin(), local_visited.end(), 0);
                            local_visit_stamp = 0;
                        }
                        ++local_visit_stamp;

                        vector<vector<int>> candidate_regions;
                        for (const int start : region) {
                            const int start_x = start % width;
                            const int start_y = start / width;
                            if (is_cut_tile(start_x, start_y, candidate) ||
                                local_visited[start] == local_visit_stamp) continue;

                            vector<int> part;
                            local_frontier.assign(1, start);
                            local_visited[start] = local_visit_stamp;
                            for (size_t head = 0; head < local_frontier.size(); ++head) {
                                const int index = local_frontier[head];
                                const int x = index % width;
                                const int y = index / width;
                                part.push_back(index);
                                for (int direction = 0; direction < 4; ++direction) {
                                    const int next_x = x + neighbor_x[direction];
                                    const int next_y = y + neighbor_y[direction];
                                    if (next_x < min_x || next_x > max_x || next_y < min_y || next_y > max_y ||
                                        is_cut_tile(next_x, next_y, candidate)) continue;
                                    const int next = next_y * width + next_x;
                                    if (member_stamp[next] != current_member_stamp ||
                                        local_visited[next] == local_visit_stamp) continue;
                                    local_visited[next] = local_visit_stamp;
                                    local_frontier.push_back(next);
                                }
                            }
                            candidate_regions.push_back(move(part));
                            if (candidate_regions.size() > 2) break;
                        }

                        if (candidate_regions.size() == 2 &&
                            candidate_regions[0].size() >= minimum_split_area &&
                            candidate_regions[1].size() >= minimum_split_area) {
                            lock_guard<mutex> lock(best_cut_mutex);
                            if (candidate_index < best_cut_index.load(memory_order_relaxed)) {
                                selected_cut = candidate;
                                split_regions = move(candidate_regions);
                                best_cut_index.store(candidate_index, memory_order_relaxed);
                            }
                        }
                    }
                };
                for (size_t worker = 1; worker < validation_worker_count; ++worker)
                    validation_workers.emplace_back(validate_worker);
                validate_worker();
                for (thread& worker : validation_workers) worker.join();
                const size_t winning_index = best_cut_index.load(memory_order_relaxed);
                if (winning_index < batch_end) {
                    selected_paths.push_back(move(segment_candidates[winning_index].path));
                    found_cut = true;
                }
                batch_start = batch_end;
            }
            if (!found_cut) {
                continue;
            }

            const int continent_id = continent_map_[region.front() / width][region.front() % width];
            vector<int> cut_band;
            const int cut_band_radius = generation_config_.continent_cut_band_radius;
            for (const int index : region) {
                if (!is_cut_tile(index % width, index / width, selected_cut)) continue;
                const int x = index % width;
                const int y = index / width;
                for (int dy = -cut_band_radius; dy <= cut_band_radius; ++dy) {
                    for (int dx = -cut_band_radius; dx <= cut_band_radius; ++dx) {
                        const int expanded_x = x + dx;
                        const int expanded_y = y + dy;
                        if (expanded_x < 0 || expanded_x >= width || expanded_y < 0 || expanded_y >= height)
                            continue;
                        if (continent_map_[expanded_y][expanded_x] == continent_id ||
                            tiled_map_[expanded_y][expanded_x] == 1)
                            cut_band.push_back(expanded_y * width + expanded_x);
                    }
                }
            }

            const double tangent_x = selected_cut.normal_y;
            const double tangent_y = -selected_cut.normal_x;
            for (const vector<int>& path : selected_paths) {
                if (path.empty()) continue;
                for (int direction : { -1, 1 }) {
                    const int endpoint = direction < 0 ? path.front() : path.back();
                    const double endpoint_x = endpoint % width + 0.5;
                    const double endpoint_y = endpoint / width + 0.5;
                    const double endpoint_along = endpoint_x * tangent_x + endpoint_y * tangent_y;
                    const double endpoint_max_distance = generation_config_.continent_cut_endpoint_max_distance;
                    const double endpoint_step = generation_config_.continent_cut_trace_step;
                    for (double distance = endpoint_step; distance <= endpoint_max_distance;
                        distance += endpoint_step) {
                        const double along = endpoint_along + direction * distance;
                        const double point_x = selected_cut.normal_x * selected_cut.offset + tangent_x * along;
                        const double point_y = selected_cut.normal_y * selected_cut.offset + tangent_y * along;
                        const int tile_x = static_cast<int>(floor(point_x));
                        const int tile_y = static_cast<int>(floor(point_y));
                        if (tile_x < 0 || tile_x >= width || tile_y < 0 || tile_y >= height) break;
                        if (tiled_map_[tile_y][tile_x] == 1) {
                            cut_band.push_back(tile_y * width + tile_x);
                            break;
                        }
                        if (continent_map_[tile_y][tile_x] != continent_id) break;
                    }
                }
            }
            for (const int index : cut_band)
                cut_band_mask[index] = 1;
            bool leaves_small_piece = false;
            for (const vector<int>& part : split_regions) {
                const size_t remaining_area = static_cast<size_t>(count_if(part.begin(), part.end(),
                    [&](int index) { return cut_band_mask[index] == 0; }));
                if (remaining_area < minimum_split_area) {
                    leaves_small_piece = true;
                    break;
                }
            }
            for (const int index : cut_band)
                cut_band_mask[index] = 0;
            if (leaves_small_piece)
                continue;

            for (const int index : cut_band) {
                planned_cut_tiles[index] = 1;
                continent_bridge_map_[index / width][index % width] = 1;
            }
            ++cuts_drawn;

            for (vector<int>& part : split_regions) {
                part.erase(remove_if(part.begin(), part.end(), [&](int index) {
                    return planned_cut_tiles[index] != 0;
                }), part.end());
                if (part.size() >= 2 * minimum_area)
                    regions.push_back(move(part));
            }
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
const vector<vector<int>>& World::get_continent_map() const {
    return continent_map_;
}
const vector<vector<int>>& World::get_original_continent_map() const {
    return original_continent_map_;
}
const vector<vector<unsigned char>>& World::get_continent_bridge_map() const {
    return continent_bridge_map_;
}
const vector<uint32_t>& World::get_continent_colors() const {
    return continent_colors_;
}
const vector<uint32_t>& World::get_original_continent_colors() const {
    return original_continent_colors_;
}
