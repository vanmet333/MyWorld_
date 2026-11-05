#include "Chunk.h"

#include <algorithm>

using namespace std;

void Chunk::add_object(ObjectId id, bool is_dynamic) {
    auto& ids = is_dynamic ? dynamic_object_ids_ : static_object_ids_;
    if (find(ids.begin(), ids.end(), id) == ids.end())
        ids.push_back(id);
}

void Chunk::remove_object(ObjectId id, bool is_dynamic) {
    auto& ids = is_dynamic ? dynamic_object_ids_ : static_object_ids_;
    ids.erase(remove(ids.begin(), ids.end(), id), ids.end());
}

void Chunk::update(double delta_time) {
    // Здесь будет логика обновления динамических объектов этого чанка.
    // delta_time передаётся в секундах; статические объекты не обходятся.
    (void)delta_time;
}

const vector<ObjectId>& Chunk::static_object_ids() const {
    return static_object_ids_;
}

const vector<ObjectId>& Chunk::dynamic_object_ids() const {
    return dynamic_object_ids_;
}
