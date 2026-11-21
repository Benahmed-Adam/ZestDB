#include <chrono>
#include <csignal>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Logger.hpp"
#include "ZestDB.hpp"

/// Demo load started by `zestdb pop`: writes one million keys (`key_<i>` = `val_<i>`) and logs how long it took.
void populate(Zest::ZestDB* db) {
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < 1000000 && db->isRunning(); ++i) {
        db->execCmd(std::format("s key_{} \"val_{}\"", i, i));
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    Zest::ZestLog(Zest::LogLevel::Info, std::format("1 Million keys in : {} seconds", std::to_string(diff.count())));
}

/// The interactive console: reads commands from stdin and runs them as the admin user `ZestLocalAdmin`.
/// `q` / `quit` stops the database; end of input (Ctrl+D, a closed pipe) leaves the loop.
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
            std::cout << "Goodbye!\n" << std::flush;
            workGuard->reset();
            db->stop();
            break;
        } else {
            try {
                std::cout << Zest::ZestDB::responseToJson(db->execCmd(line, Zest::Role::Admin, true, "ZestLocalAdmin")) << '\n' << std::flush;
            } catch (const std::exception& e) {
                Zest::ZestLog(Zest::LogLevel::Error, std::format("Command failed with an exception: {}", e.what()));
            }
        }
    }
}

/// Starts the database, the console, the signal handler and the HTTP stats server.
/// Threads: the console reads stdin, `ioThread` runs the signal handler, and the main thread serves HTTP until stop().
static int run(int argc, char** argv) {
    Zest::ZestDB db;

    // Keeps ioThread's run() alive while nothing else is queued; released on exit
    auto work = asio::make_work_guard(db.ioContext());

    asio::signal_set signals(db.ioContext(), SIGINT, SIGTERM);

    signals.async_wait([&](const asio::error_code& error, int) {
        if (!error) {
            Zest::ZestLog(Zest::LogLevel::Warning, "Interrupt signal detected, exiting...");
            work.reset();
            db.stop();
            Zest::ZestLog(Zest::LogLevel::Warning, "Press Enter to quit");
        }
    });

    std::thread popThread;
    if (argc > 1 && std::string(argv[1]) == "pop") {
        popThread = std::thread(populate, &db);
    }

    std::thread cmdThread(cmd, &db, &work);

    std::thread ioThread([&db]() { db.ioContext().run(); });

    db.httpServer().listen("0.0.0.0", db.getSettings().WebPort);

    cmdThread.join();
    ioThread.join();
    if (popThread.joinable()) {
        popThread.join();
    }

    return 0;
}

/// Runs the server; a startup failure (missing config.yaml, invalid setting, port in use...) is logged instead of
/// ending in std::terminate.
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        Zest::ZestLog(Zest::LogLevel::Critical, std::format("ZestDB stopped: {}", e.what()));
        return 1;
    }
}
