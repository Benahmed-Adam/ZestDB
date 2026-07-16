#include <atomic>
#include <catch2/catch_all.hpp>
#include <filesystem>

#include "Compactor.hpp"
#include "IndexManager.hpp"
#include "StorageManager.hpp"

using namespace Zest;
namespace fs = std::filesystem;

class CompactorFixture {
public:
    Settings settings;
    fs::path testDir;
    CompactorFixture() {
        testDir = fs::temp_directory_path() / ("zestdb_test_comp_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(testDir / "seg");
        settings.DbPath = testDir;
        settings.IndexPath = testDir / "INDEX";
        settings.WalPath = testDir / "WAL";
        settings.SegSize = 10000;
        settings.CompactingInterval = 0;
    }
    ~CompactorFixture() { fs::remove_all(testDir); }
};

TEST_CASE_METHOD(CompactorFixture, "Compactor stop flag causes exit", "[compactor]") {
    Compactor comp(settings);
    IndexManager im(settings);
    StorageManager sm(settings);
    std::atomic<bool> stopFlag{ true };
    std::thread t([&]() { comp.run(im, sm, stopFlag); });
    t.join();
    SUCCEED();
}

TEST_CASE_METHOD(CompactorFixture, "Compactor runs one cycle then stops", "[compactor]") {
    Compactor comp(settings);
    IndexManager im(settings);
    StorageManager sm(settings);
    std::atomic<bool> stopFlag{ false };
    std::thread t([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        stopFlag.store(true);
    });
    comp.run(im, sm, stopFlag);
    t.join();
    SUCCEED();
}
