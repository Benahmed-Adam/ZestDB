#include "Logger.hpp"
#include "ZestDB.hpp"
#include <iostream>
#include <sstream>
#include <string>

void populate(ZestDB& db)
{
    for (int i = 0; i < 1000; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string value = "value_" + std::to_string(i) + "_" + std::string(100, 'x');
        db.set(key, value);
    }

    for (int i = 0; i < 100; ++i) {
        std::string key = "dup_" + std::to_string(i);
        db.set(key, "first");
        db.set(key, "second");
        db.set(key, "third");
    }

    std::string longKey(300, 'k');
    db.set(longKey, "long key test");

    std::string longValue(10000, 'v');
    db.set("long_value", longValue);

    for (int i = 0; i < 50; ++i) {
        db.del("key_" + std::to_string(i));
    }

    db.set("empty", "");
    db.set("special", "abc\ndef\tghi\r\n");
    db.set("unicode", "émojis: 🎉 € 中文");
}

int main(int argc, char** argv)
{
    (void)argv;

    ZestDB db;

    std::cout << std::endl;

    if (argc > 1)
        populate(db);

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
        } else if (cmd == "sb") {
            std::string pattern, value;
            if (!(iss >> pattern)) {
                std::cerr << "Error: missing pattern" << std::endl;
                std::cout << "Usage: sb <pattern> <value>" << std::endl;
                continue;
            }
            if (!(iss >> value)) {
                std::cerr << "Error: missing value" << std::endl;
                std::cout << "Usage: sb <pattern> <value>" << std::endl;
                continue;
            }
            std::string rest;
            while (iss >> rest) {
                value += " " + rest;
            }
            ResultType result = db.setBy(pattern, value);
            if (result.code == ResultType::Code::SUCCESS) {
                std::cout << "OK: " << result.message << std::endl;
            } else {
                std::cout << "ERROR: " << result.message << std::endl;
            }
        } else if (cmd == "db") {
            std::string pattern;
            if (!(iss >> pattern)) {
                std::cerr << "Error: missing pattern" << std::endl;
                std::cout << "Usage: db <pattern>" << std::endl;
                continue;
            }
            ResultType result = db.delBy(pattern);
            if (result.code == ResultType::Code::SUCCESS) {
                std::cout << "OK: " << result.message << std::endl;
            } else {
                std::cout << "ERROR: " << result.message << std::endl;
            }
        }
    }

    return 0;
}
