#pragma once

#include "WorldObjects.h"
#include <vector>

class Chunk {
public:
    static constexpr int size_in_tiles = 32;

    void add_object(ObjectId id, bool is_dynamic);
    void remove_object(ObjectId id, bool is_dynamic);
    void update(double delta_time);

    const std::vector<ObjectId>& static_object_ids() const;
    const std::vector<ObjectId>& dynamic_object_ids() const;

private:
    std::vector<ObjectId> static_object_ids_;
    std::vector<ObjectId> dynamic_object_ids_;
};
