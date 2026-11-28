#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iostream>
#include <libzippp/libzippp.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

#include "IndexManager.hpp"
#include "Logger.hpp"
#include "ZestDB.hpp"
#include "lib/json.hpp"
#include "lib/node.hpp"

namespace Zest {

    namespace fs = std::filesystem;

    using json = nlohmann::json;

    /// Copies the first `size` bytes of `from` to `to`. Used by archiving: the files keep growing while they are copied,
    /// and only the part that was flushed when the snapshot was taken is consistent.
    static bool copyPrefix(const fs::path& from, const fs::path& to, std::uintmax_t size) {
        std::ifstream in(from, std::ios::binary);
        std::ofstream out(to, std::ios::binary | std::ios::trunc);
        std::vector<char> buffer(1 << 20);
        while (size > 0) {
            const auto chunk = static_cast<std::streamsize>(std::min<std::uintmax_t>(size, buffer.size()));
            if (!in.read(buffer.data(), chunk) || !out.write(buffer.data(), chunk)) {
                return false;
            }
            size -= static_cast<std::uintmax_t>(chunk);
        }
        return static_cast<bool>(out.flush());
    }

    /// Clears `flag` when it goes out of scope, on every return path (the `archiving` flag of save and restore).
    struct ResetFlag {
        std::atomic<bool>& flag;

        ~ResetFlag() {
            flag = false;
        }
    };

    bool ZestDB::createArchive() {
        if (this->archiving.exchange(true)) {
            ZestLog(LogLevel::Warning, "An archive is already being created");
            return false;
        }
        ResetFlag resetFlag{ this->archiving };

        fs::path archivePath;
        unsigned int retention;
        {
            std::shared_lock<std::shared_mutex> lock(this->writeMtx);
            archivePath = this->settings.ArchiveStoragePath;
            retention = this->settings.ArchiveRetention;
        }
        fs::path written;
        if (!this->writeArchive("", std::format("zestdb_archive_{:%Y-%m-%d_%H-%M-%S}.zip", std::chrono::system_clock::now()), written, nullptr)) {
            return false;
        }

        std::error_code ec;
        if (retention > 0) {
            std::vector<fs::path> archives;
            for (const auto& entry : fs::directory_iterator(archivePath, ec)) {
                const std::string name = entry.path().filename().string();
                if (entry.is_regular_file() && name.starts_with("zestdb_archive_") && name.ends_with(".zip")) {
                    archives.push_back(entry.path());
                }
            }
            std::sort(archives.begin(), archives.end());
            for (size_t i = 0; i + retention < archives.size(); i++) {
                fs::remove(archives[i], ec);
            }
        }
        return true;
    }

    bool ZestDB::createReplicaSnapshot(fs::path& zipPath, uint64_t& seq) {
        if (this->archiving.exchange(true)) {
            ZestLog(LogLevel::Warning, "Replication: an archive is being created or restored, the full sync is put off");
            return false;
        }
        ResetFlag resetFlag{ this->archiving };
        std::random_device rd;
        return this->writeArchive(".replication", std::format("snapshot_{:08x}{:08x}.zip", rd(), rd()), zipPath, &seq);
    }

    bool ZestDB::writeArchive(const fs::path& subfolder, const std::string& name, fs::path& written, uint64_t* seq) {
        fs::path snapshot;
        std::error_code ec;
        try {
            fs::path folder_to_zip;
            fs::path archivePath;
            std::vector<std::pair<fs::path, std::uintmax_t>> toCopy;
            auto compactionLocks = this->shardManager->pauseCompaction();
            {
                std::unique_lock<std::shared_mutex> lock(this->writeMtx);
                this->flushLocked();
                // No write runs meanwhile: the archive holds exactly the writes numbered before this one
                if (seq) {
                    *seq = this->replicationLog->nextSeq();
                }
                folder_to_zip = this->settings.DbPath;
                archivePath = this->settings.ArchiveStoragePath;

                if (!fs::is_directory(folder_to_zip)) {
                    ZestLog(LogLevel::Error, std::format("Source folder does not exist : {}", folder_to_zip.string()));
                    return false;
                }
                fs::create_directories(archivePath);
                snapshot = archivePath / ".snapshot";
                fs::remove_all(snapshot);

                const fs::path archiveDir = fs::weakly_canonical(archivePath);
                for (auto it = fs::recursive_directory_iterator(folder_to_zip); it != fs::recursive_directory_iterator(); ++it) {
                    if (it->is_directory() && fs::weakly_canonical(it->path()) == archiveDir) {
                        it.disable_recursion_pending();
                        continue;
                    }
                    if (it->is_regular_file()) {
                        fs::path target = snapshot / fs::relative(it->path(), folder_to_zip);
                        fs::create_directories(target.parent_path());
                        // The WALs were just cleared by the flush, so they are copied now; the other files are only
                        // appended to, so recording their size here is enough to copy a consistent state after unlocking
                        if (it->path().filename() == "WAL") {
                            fs::copy_file(it->path(), target);
                        } else {
                            toCopy.emplace_back(it->path(), it->file_size());
                        }
                    }
                }
            }

            for (const auto& [source, size] : toCopy) {
                if (!copyPrefix(source, snapshot / fs::relative(source, folder_to_zip), size)) {
                    ZestLog(LogLevel::Error, std::format("Error while copying : {}", source.string()));
                    fs::remove_all(snapshot, ec);
                    return false;
                }
            }
            compactionLocks.clear();

            fs::create_directories(archivePath / subfolder);
            fs::path archive_full_path = archivePath / subfolder / name;
            libzippp::ZipArchive archive(archive_full_path.string());
            if (!archive.open(libzippp::ZipArchive::New)) {
                ZestLog(LogLevel::Error, std::format("Impossible to create the archive : {}", archive_full_path.string()));
                fs::remove_all(snapshot, ec);
                return false;
            }
            if (fs::exists(snapshot)) {
                for (const auto& entry : fs::recursive_directory_iterator(snapshot)) {
                    if (entry.is_regular_file() && !archive.addFile(fs::relative(entry.path(), snapshot).generic_string(), entry.path().string())) {
                        ZestLog(LogLevel::Error, std::format("Error while adding : {}", entry.path().string()));
                        archive.close();
                        fs::remove_all(snapshot, ec);
                        return false;
                    }
                }
            }
            int close_result = archive.close();
            fs::remove_all(snapshot, ec);
            if (close_result != LIBZIPPP_OK) {
                ZestLog(LogLevel::Error, std::format("Error while saving the archive : Error code : {}", close_result));
                return false;
            }
            written = archive_full_path;
            return true;
        } catch (const std::exception& e) {
            ZestLog(LogLevel::Error, std::format("Error while creating the archive : {}", e.what()));
            if (!snapshot.empty()) {
                fs::remove_all(snapshot, ec);
            }
            return false;
        }
    }

    /// Extracts the `shards/` entries of an archive under `target`. Refuses archives with an absolute path or a `..`
    /// entry (zip slip), so nothing is written outside `target`. The WAL files are created empty: an archive is taken
    /// right after a flush, and a non-empty WAL is not replayed.
    /// @return Success, InvalidArchive (unreadable, no shard data, unsafe entry) or RestoreFailed (write error).
    static ResultType::Code extractShards(const fs::path& archivePath, const fs::path& target) {
        libzippp::ZipArchive archive(archivePath.string());
        if (!archive.open(libzippp::ZipArchive::ReadOnly)) {
            return ResultType::Code::InvalidArchive;
        }

        size_t files = 0;
        for (const auto& entry : archive.getEntries()) {
            const fs::path name = fs::path(entry.getName()).lexically_normal();
            if (name.has_root_path() || std::find(name.begin(), name.end(), fs::path("..")) != name.end()) {
                archive.close();
                return ResultType::Code::InvalidArchive;
            }
            if (name.empty() || *name.begin() != "shards") {
                continue;
            }

            const fs::path out = target / name;
            if (entry.isDirectory()) {
                fs::create_directories(out);
                continue;
            }
            fs::create_directories(out.parent_path());
            std::ofstream file(out, std::ios::binary | std::ios::trunc);
            if (out.filename() == "WAL") {
                if (entry.getSize() > 0) {
                    ZestLog(LogLevel::Warning, std::format("Restore: {} is not empty and is not replayed", name.generic_string()));
                }
            } else if (entry.readContent(file) != LIBZIPPP_OK || !file.flush()) {
                archive.close();
                return ResultType::Code::RestoreFailed;
            }
            files++;
        }
        archive.close();
        return files > 0 ? ResultType::Code::Success : ResultType::Code::InvalidArchive;
    }

    /// Replaces the `live` shards folder by `restored` with two renames, keeping the old one as `previous`.
    /// If the second rename fails, the old folder is put back. @return true if `restored` is now live.
    static bool swapShards(const fs::path& live, const fs::path& restored, const fs::path& previous) {
        std::error_code ec;
        fs::remove_all(previous, ec);
        const bool hadLive = fs::exists(live, ec);
        if (hadLive) {
            fs::rename(live, previous, ec);
            if (ec) {
                return false;
            }
        }
        fs::rename(restored, live, ec);
        if (ec && hadLive) {
            std::error_code ignored;
            fs::rename(previous, live, ignored);
        }
        return !ec;
    }

    ResultType ZestDB::restore(const std::string& zipname) {
        if (zipname.empty() || fs::path(zipname).filename().string() != zipname || fs::path(zipname).extension() != ".zip") {
            return { ResultType::Code::ArchiveNotFound };
        }

        fs::path archivePath;
        {
            std::shared_lock<std::shared_mutex> lock(this->writeMtx);
            archivePath = this->settings.ArchiveStoragePath / zipname;
        }
        return this->restoreArchive(archivePath);
    }

    ResultType ZestDB::restoreArchive(const fs::path& archivePath) {
        if (!fs::is_regular_file(archivePath)) {
            return { ResultType::Code::ArchiveNotFound };
        }
        fs::path dbPath;
        {
            std::shared_lock<std::shared_mutex> lock(this->writeMtx);
            dbPath = this->settings.DbPath;
        }

        if (this->archiving.exchange(true)) {
            ZestLog(LogLevel::Warning, "Restore: an archive is being created or restored");
            return { ResultType::Code::RestoreFailed };
        }
        ResetFlag resetFlag{ this->archiving };

        const fs::path staging = dbPath / ".restore";
        const fs::path previous = dbPath / ".restore-previous";
        std::error_code ec;
        try {
            fs::remove_all(staging, ec);
            const ResultType::Code extracted = extractShards(archivePath, staging);
            if (extracted != ResultType::Code::Success) {
                fs::remove_all(staging, ec);
                return { extracted };
            }

            bool restored = false;
            {
                std::unique_lock<std::shared_mutex> lock(this->writeMtx);
                this->shardManager.reset();
                restored = swapShards(dbPath / "shards", staging / "shards", previous);
                try {
                    this->shardManager = std::make_unique<ShardManager>(this->settings, NUM_SHARDS);
                } catch (const std::exception& e) {
                    ZestLog(LogLevel::Error, std::format("Restore: the restored shards cannot be opened ({}), going back to the previous data", e.what()));
                    this->shardManager.reset();
                    if (restored) {
                        fs::remove_all(dbPath / "shards", ec);
                        fs::rename(previous, dbPath / "shards", ec);
                    }
                    restored = false;
                    this->shardManager = std::make_unique<ShardManager>(this->settings, NUM_SHARDS);
                }
            }

            fs::remove_all(staging, ec);
            fs::remove_all(previous, ec);
            if (!restored) {
                return { ResultType::Code::RestoreFailed };
            }
            this->newHistory();
            ZestLog(LogLevel::Info, std::format("Restored the database from {}", archivePath.string()));
            return { ResultType::Code::Success };
        } catch (const std::exception& e) {
            ZestLog(LogLevel::Error, std::format("Restore failed: {}", e.what()));
            fs::remove_all(staging, ec);
            return { ResultType::Code::RestoreFailed };
        }
    }

    ResultType ZestDB::listSaves() const {
        std::vector<std::string> names;
        std::error_code ec;
        for (auto it = fs::directory_iterator(this->settings.ArchiveStoragePath, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == ".zip") {
                names.push_back(it->path().filename().string());
            }
        }
        std::sort(names.begin(), names.end());

        return { ResultType::Code::Success, json(names).dump(-1, ' ', false, json::error_handler_t::replace), static_cast<long long>(names.size()) };
    }

} // namespace Zest
