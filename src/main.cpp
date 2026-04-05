#include "ZestDB.hpp"
#include <iostream>

int main()
{
    ZestDB db;
    std::cout << db.get("caca");
    return 0;
}