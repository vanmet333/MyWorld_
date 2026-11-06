#pragma once

#include "WorldObjects.h"
#include <vector>

using namespace std;

class Chunk {
public:
    static constexpr int size_in_tiles = 32;

    void add_object(ObjectId id, bool is_dynamic);
    void remove_object(ObjectId id, bool is_dynamic);
    void update(double delta_time);

    const vector<ObjectId>& static_object_ids() const;
    const vector<ObjectId>& dynamic_object_ids() const;

private:
    vector<ObjectId> static_object_ids_;
    vector<ObjectId> dynamic_object_ids_;
};
