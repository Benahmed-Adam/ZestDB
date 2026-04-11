#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "Logger.hpp"
#include "ZestDB.hpp"

void populate(ZestDB* db)
{
    for (int i = 0; i < 1000; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string value = "value_" + std::to_string(i) + "_" + std::string(100, 'x');
        db->set(key, value);
    }

    for (int i = 0; i < 100; ++i) {
        std::string key = "dup_" + std::to_string(i);
        db->set(key, "first");
        db->set(key, "second");
        db->set(key, "third");
    }

    std::string longKey(300, 'k');
    db->set(longKey, "long key test");

    std::string longValue(10000, 'v');
    db->set("long_value", longValue);

    for (int i = 0; i < 50; ++i) {
        db->del("key_" + std::to_string(i));
    }

    db->set("empty", "");
    db->set("special", "abc\ndef\tghi\r\n");
    db->set("unicode", "émojis: 🎉 € 中文");
}

void cmd(ZestDB* db)
{
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
            db->srv.stop();
            break;
        } else if (cmd == "h" || cmd == "help") {
            std::cout << "HELP !!" << std::endl;
        } else {
            std::cout << db->execCmd(line);
        }
    }
}

int main(int argc, char** argv)
{
    ZestDB db;

    if (argc > 1 && std::string(argv[1]) == "pop") {
        std::thread t(populate, &db);
        t.detach();
    }

    std::thread t(cmd, &db);

    db.srv.listen("0.0.0.0", 8080);

    t.join();

    return 0;
}