#include <chrono>
#include <csignal>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "Logger.hpp"
#include "ZestDB.hpp"

void populate(ZestDB* db)
{
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < 1000000; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string value = "val_" + std::to_string(i);

        db->set(key, value);
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    ZestLog(LogLevel::INFO, std::format("1 Million keys in : {} seconds", std::to_string(diff.count())));
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
            auto resp = db->execCmd(line);
            std::cout << ZestDB::responseToJson(resp) << std::endl;
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