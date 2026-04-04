#pragma once

#include <memory>

#include "IndexManager.hpp"
#include "Settings.hpp"

class ZestDB {
public:
    ZestDB();
    ~ZestDB();
private:
    void boot();

    Settings settings;
    IndexManager* indexManager;
};