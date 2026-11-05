#pragma once

#include <cstdint>
#include <vector>

using namespace std;
class World;

class Render
{
public:
    void set_zoom_scale(float zoom_scale);
    void draw(const World& world) const;
    void draw_map_tiles(
        const World& world,
        const vector<unsigned char>& tile_transforms,
        float screen_tile_size,
        float camera_x,
        float camera_y,
        vector<uint32_t>& frame_pixels,
        bool show_continents = false,
        bool show_humidity = false,
        bool show_heat = false
    ) const;

private:
    float zoom_scale_ = 1.0f;
};
