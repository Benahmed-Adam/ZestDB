#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <csignal>

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

void cmd(ZestDB* db, asio::executor_work_guard<asio::io_context::executor_type>* workGuard)
{
    std::string line;
    while (db->settings.isRunning) {
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
            workGuard->reset();
            db->stop();
            break;
        } else {
            std::cout << db->execCmd(line);
        }
    }
}

int main(int argc, char** argv)
{
    ZestDB db;

    auto work = asio::make_work_guard(db.ioCtx);

    asio::signal_set signals(db.ioCtx, SIGINT, SIGTERM);

    signals.async_wait([&](const asio::error_code& error, int) {
        if (!error) {
            ZestLog(LogLevel::WARNING, "Interrupt signal detected, exiting...");
            work.reset();
            db.stop();
            ZestLog(LogLevel::WARNING, "Press Enter to quit");
        }
    });

    if (argc > 1 && std::string(argv[1]) == "pop") {
        std::thread t(populate, &db);
        t.detach();
    }

    std::thread t(cmd, &db, &work);

    std::thread ioThread([&db]() {
        db.ioCtx.run();
    });
    
    db.srv->listen("0.0.0.0", db.settings.WebPort);

    t.join();
    ioThread.join();

    return 0;
}