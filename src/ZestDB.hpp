#pragma once

#include "Settings.hpp"

class ZestDB {
public:
    ZestDB();
private:
    void boot();

    Settings settings;
};