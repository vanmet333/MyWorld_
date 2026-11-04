#pragma once

#include "Render.h"
#include "World.h"

class Game
{
public:
    void run();
    void start();

private:
    World world_;
    Render render_;
};
