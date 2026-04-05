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
            if (!iss.eof()) {
                std::cerr << "Warning: extra text ignored after key" << std::endl;
            }
            std::string result = db.get(key);
            if (result == "") {
                std::cout << "(not found)" << std::endl;
            } else {
                std::cout << result << std::endl;
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
            if (result == ResultType::SUCCESS) {
                std::cout << "OK" << std::endl;
            } else {
                std::cout << "ERROR" << std::endl;
            }
        } else if (cmd == "d") {
            std::string key;
            if (!(iss >> key)) {
                std::cerr << "Error: missing key" << std::endl;
                std::cout << "Usage: d <key>" << std::endl;
                continue;
            }
            if (!iss.eof()) {
                std::cerr << "Warning: extra text ignored after key" << std::endl;
            }
            ResultType result = db.del(key);
            if (result == ResultType::SUCCESS) {
                std::cout << "OK" << std::endl;
            } else {
                std::cout << "ERROR" << std::endl;
            }
        } else {
            std::cerr << "Unknown command: " << cmd << std::endl;
            std::cout << "Type 'h' for help" << std::endl;
        }
    }

    return 0;
}
