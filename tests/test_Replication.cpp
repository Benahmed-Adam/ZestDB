#include <catch2/catch_all.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "ZestDB.hpp"
#include "lib/json.hpp"

using namespace Zest;
namespace fs = std::filesystem;

namespace {
    /// One instance with its own folder, which survives a restart.
    class Node {
    public:
        fs::path dir;
        std::unique_ptr<ZestDB> db;
        std::vector<std::thread> ioThreads;

        Node(const fs::path& folder, unsigned short port, const std::string& replication)
            : dir(folder) {
            fs::create_directories(dir);
            std::ofstream cfg(dir / "config.yaml");
            cfg << "DbPath: " << (dir / "db").string() << "\n";
            cfg << "DBPort: " << port << "\n";
            cfg << "WebPort: " << port + 1 << "\n";
            cfg << "ArchiveStoragePath: " << (dir / "archive").string() << "\n";
            cfg << "AutoArchiveSaving: false\n";
            cfg << "users:\n";
            cfg << "  - user: repl\n";
            cfg << "    role: replica\n";
            cfg << "    password: p\n";
            cfg << "replication:\n" << replication;
        }

        ~Node() {
            this->stop();
        }

        void start() {
            this->db = std::make_unique<ZestDB>(this->dir);
            for (int i = 0; i < 2; i++) {
                this->ioThreads.emplace_back([this] { this->db->ioContext().run(); });
            }
        }

        void stop() {
            if (!this->db) {
                return;
            }
            this->db->stop();
            for (auto& t : this->ioThreads) {
                t.join();
            }
            this->ioThreads.clear();
            this->db.reset();
        }

        /// True once `get key` answers a response holding `value`, within 20 s.
        bool eventuallyHas(const std::string& key, const std::string& value) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (std::chrono::steady_clock::now() < deadline) {
                const ResultType r = this->db->execCmd("g " + key);
                if (r.code == ResultType::Code::Success && r.response.find(value) != std::string::npos) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            return false;
        }
    };
} // namespace

TEST_CASE("A replica follows its primary across restarts of both", "[replication][e2e]") {
    const fs::path root = fs::temp_directory_path() / "zestdb_test_replication";
    fs::remove_all(root);
    {
        Node primary(root / "primary", 29631, "  historySize: 1000\n");
        Node replica(root / "replica", 29633, "  role: replica\n  primary: 127.0.0.1:29631\n  user: repl\n  password: p\n");
        primary.start();
        replica.start();

        REQUIRE(primary.db->execCmd(R"(s a "one")").code == ResultType::Code::Success);
        CHECK(replica.eventuallyHas("a", "one"));
        REQUIRE(replica.db->getReplicator() != nullptr);
        CHECK(replica.db->getReplicator()->connected());

        // The replica resumes from its saved position
        replica.stop();
        REQUIRE(primary.db->execCmd(R"(s b "two")").code == ResultType::Code::Success);
        replica.start();
        CHECK(replica.eventuallyHas("b", "two"));
        CHECK(replica.eventuallyHas("a", "one"));

        // A restarted primary has a new history: the replica gets a full sync
        primary.stop();
        primary.start();
        REQUIRE(primary.db->execCmd(R"(s c "three")").code == ResultType::Code::Success);
        CHECK(replica.eventuallyHas("c", "three"));
        CHECK(replica.eventuallyHas("a", "one"));
        CHECK(replica.eventuallyHas("b", "two"));

        REQUIRE(primary.db->execCmd("d a").code == ResultType::Code::Success);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (replica.db->execCmd("g a").code != ResultType::Code::KeyNotFound && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(replica.db->execCmd("g a").code == ResultType::Code::KeyNotFound);
    }
    fs::remove_all(root);
}

TEST_CASE("A replica refuses its clients' writes", "[replication][e2e]") {
    const fs::path root = fs::temp_directory_path() / "zestdb_test_replica_readonly";
    fs::remove_all(root);
    {
        // No primary listens on 29635: the replicator keeps retrying, and stop() must not wait for its backoff
        Node replica(root / "replica", 29637, "  role: replica\n  primary: 127.0.0.1:29635\n  user: repl\n  password: p\n");
        replica.start();
        CHECK(replica.db->execCmd(R"(s a "one")").code == ResultType::Code::ReplicaReadOnly);
        CHECK(replica.db->execCmd("d a").code == ResultType::Code::ReplicaReadOnly);
        CHECK(replica.db->execCmd(R"(sb "a.*" "x")").code == ResultType::Code::ReplicaReadOnly);
        CHECK(replica.db->execCmd("incr n").code == ResultType::Code::ReplicaReadOnly);
        CHECK(replica.db->execCmd("call f").code == ResultType::Code::ReplicaReadOnly);
        CHECK(replica.db->execCmd("restore x.zip").code == ResultType::Code::ReplicaReadOnly);
        CHECK(replica.db->execCmd("g a").code == ResultType::Code::KeyNotFound);
        CHECK(replica.db->execCmd("f").code == ResultType::Code::Success);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const auto before = std::chrono::steady_clock::now();
        replica.stop();
        CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(1));
    }
    fs::remove_all(root);
}

TEST_CASE("A replica keeps the primary's absolute expiry dates", "[replication][e2e]") {
    const fs::path root = fs::temp_directory_path() / "zestdb_test_replication_ttl";
    fs::remove_all(root);
    {
        Node primary(root / "primary", 29641, "  historySize: 1000\n");
        Node replica(root / "replica", 29643, "  role: replica\n  primary: 127.0.0.1:29641\n  user: repl\n  password: p\n");
        primary.start();
        replica.start();
        const auto ttl = [](Node& node, const std::string& key) { return std::stoll(node.db->execCmd("ttl " + key).response); };
        const auto eventually = [](const std::function<bool()>& done) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (!done() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            return done();
        };

        REQUIRE(primary.db->execCmd(R"(setex short 2 "v")").code == ResultType::Code::Success);
        REQUIRE(primary.db->execCmd(R"(s long "w")").code == ResultType::Code::Success);
        REQUIRE(primary.db->execCmd("expire long 1000").code == ResultType::Code::Success);
        CHECK(replica.eventuallyHas("short", "v"));
        CHECK(eventually([&] { return ttl(replica, "long") > 2; }));
        CHECK(ttl(replica, "long") == ttl(primary, "long"));
        CHECK(ttl(replica, "short") <= 2);

        REQUIRE(primary.db->execCmd("persist long").code == ResultType::Code::Success);
        CHECK(eventually([&] { return ttl(replica, "long") == -1; }));

        // The key expires on the replica at the same date, without the primary sending anything
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        CHECK(replica.db->execCmd("g short").code == ResultType::Code::KeyNotFound);
        CHECK(primary.db->execCmd("g short").code == ResultType::Code::KeyNotFound);
        CHECK(replica.eventuallyHas("long", "w"));
    }
    fs::remove_all(root);
}

TEST_CASE("info and /metrics show the replication state", "[replication][e2e]") {
    const fs::path root = fs::temp_directory_path() / "zestdb_test_replication_info";
    fs::remove_all(root);
    {
        Node primary(root / "primary", 29651, "  historySize: 1000\n");
        Node replica(root / "replica", 29653, "  role: replica\n  primary: 127.0.0.1:29651\n  user: repl\n  password: p\n");
        primary.start();
        replica.start();
        const auto info = [](Node& node) { return nlohmann::json::parse(node.db->execCmd("info").response).at("replication"); };

        REQUIRE(primary.db->execCmd(R"(s a "one")").code == ResultType::Code::Success);
        CHECK(replica.eventuallyHas("a", "one"));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (info(primary).at("replicas") != 1 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        const auto p = info(primary);
        CHECK(p.at("role") == "primary");
        CHECK(p.at("replicas") == 1);
        CHECK(p.at("historyId").get<std::string>().size() == 32);
        const auto r = info(replica);
        CHECK(r.at("role") == "replica");
        CHECK(r.at("primary") == "127.0.0.1:29651");
        CHECK(r.at("connected") == true);
        CHECK(r.at("nextSeq") == p.at("nextSeq"));

        const std::string seq = std::format("zestdb_replication_next_seq {}\n", p.at("nextSeq").get<uint64_t>());
        const std::string primaryMetrics = primary.db->prometheusMetrics();
        CHECK(primaryMetrics.find(seq) != std::string::npos);
        CHECK(primaryMetrics.find("zestdb_replicas 1\n") != std::string::npos);
        const std::string replicaMetrics = replica.db->prometheusMetrics();
        CHECK(replicaMetrics.find(seq) != std::string::npos);
        CHECK(replicaMetrics.find("zestdb_replication_connected 1\n") != std::string::npos);
    }
    fs::remove_all(root);
}
