#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

using ObjectId = std::uint32_t;

enum class WorldObjectType {
    Tree,
    Mineral

};

enum class MineralVariant : int {
    Rock = 0,
    Copper = 1,
    Iron = 2,
    Gold = 3,
    Platinum = 4,
};

// Один экземпляр объекта хранится в World. Чанки содержат только его ID.
struct WorldObject {
    ObjectId id{};
    WorldObjectType type{};
    int x{};
    int y{};
    int width{ 1 };
    int height{ 1 };
    bool is_dynamic{};
    int variant{};
};

using InitialItem = std::pair<WorldObjectType, std::size_t>;
