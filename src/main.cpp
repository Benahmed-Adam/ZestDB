#include <chrono>
#include <csignal>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Logger.hpp"
#include "ZestDB.hpp"

void populate(Zest::ZestDB* db) {
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < 1000000 && db->isRunning(); ++i) {
        db->execCmd(std::format("s key_{} val_{}", i, i));
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    Zest::ZestLog(Zest::LogLevel::INFO, std::format("1 Million keys in : {} seconds", std::to_string(diff.count())));
}

void cmd(Zest::ZestDB* db, asio::executor_work_guard<asio::io_context::executor_type>* workGuard) {
    std::string line;
    while (db->isRunning()) {
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
            try {
                std::cout << Zest::ZestDB::responseToJson(db->execCmd(line)) << std::endl;
            } catch (const std::exception& e) {
                Zest::ZestLog(Zest::LogLevel::ERROR, std::format("Command failed with an exception: {}", e.what()));
            }
        }
    }
}

int main(int argc, char** argv) {
    Zest::ZestDB db;

    auto work = asio::make_work_guard(db.ioCtx);

    asio::signal_set signals(db.ioCtx, SIGINT, SIGTERM);

    signals.async_wait([&](const asio::error_code& error, int) {
        if (!error) {
            Zest::ZestLog(Zest::LogLevel::WARNING, "Interrupt signal detected, exiting...");
            work.reset();
            db.stop();
            Zest::ZestLog(Zest::LogLevel::WARNING, "Press Enter to quit");
        }
    });

    std::thread popThread;
    if (argc > 1 && std::string(argv[1]) == "pop") {
        popThread = std::thread(populate, &db);
    }

    std::thread cmdThread(cmd, &db, &work);

    std::vector<std::thread> ioThreads;
    for (unsigned int i = 0; i < std::max(1u, std::thread::hardware_concurrency()); i++) {
        ioThreads.emplace_back([&db]() { db.ioCtx.run(); });
    }

    db.srv->listen("0.0.0.0", db.settings.WebPort);

    cmdThread.join();
    for (auto& t : ioThreads) {
        t.join();
    }
    if (popThread.joinable()) {
        popThread.join();
    }

    return 0;
}