#include "Logger.hpp"
#include "ZestDB.hpp"
#include <iostream>
#include <sstream>
#include <string>

int main()
{
    ZestDB db;

    std::cout << std::endl;

    std::string line;
    while (true) {
        std::cout << "zestdb> ";
        if (!std::getline(std::cin, line)) {
            break;
        }

        if (line.empty()) {
            continue;
        }

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "q" || cmd == "quit") {
            std::cout << "Goodbye!" << std::endl;
            break;
        } else if (cmd == "g") {
            std::string key;
            if (!(iss >> key)) {
                std::cerr << "Error: missing key" << std::endl;
                std::cout << "Usage: g <key>" << std::endl;
                continue;
            }
            ResultType result = db.get(key);
            if (result.code == ResultType::Code::SUCCESS) {
                std::cout << result.message << std::endl;
            } else {
                std::cout << "(not found): " << result.message << std::endl;
            }
        } else if (cmd == "s") {
            std::string key, value;
            if (!(iss >> key)) {
                std::cerr << "Error: missing key" << std::endl;
                std::cout << "Usage: s <key> <value>" << std::endl;
                continue;
            }
            if (!(iss >> value)) {
                std::cerr << "Error: missing value" << std::endl;
                std::cout << "Usage: s <key> <value>" << std::endl;
                continue;
            }
            std::string rest;
            while (iss >> rest) {
                value += " " + rest;
            }
            ResultType result = db.set(key, value);
            if (result.code == ResultType::Code::SUCCESS) {
                std::cout << "OK: " << result.message << std::endl;
            } else {
                std::cout << "ERROR: " << result.message << std::endl;
            }
        } else if (cmd == "d") {
            std::string key;
            if (!(iss >> key)) {
                std::cerr << "Error: missing key" << std::endl;
                std::cout << "Usage: d <key>" << std::endl;
                continue;
            }
            ResultType result = db.del(key);
            if (result.code == ResultType::Code::SUCCESS) {
                std::cout << "OK: " << result.message << std::endl;
            } else {
                std::cout << "ERROR: " << result.message << std::endl;
            }
        } else if (cmd == "gb") {
            std::string pattern;
            if (!(iss >> pattern)) {
                std::cerr << "Error: missing pattern" << std::endl;
                std::cout << "Usage: gb <pattern>" << std::endl;
                continue;
            }
            ResultType result = db.getBy(pattern);
            if (result.code == ResultType::Code::SUCCESS) {
                std::cout << result.message << std::endl;
            } else {
                std::cout << "(not found): " << result.message << std::endl;
            }
        }
    }

    return 0;
}
