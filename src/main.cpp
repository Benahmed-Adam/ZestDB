#include "ZestDB.hpp"
#include <iostream>

int main()
{
    ZestDB db;
    //db.set("caca", "ccaca");
    std::cout << db.get("caca") << std::endl;
    return 0;
}