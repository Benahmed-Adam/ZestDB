#pragma once

#define CPPHTTPLIB_OPENSSL_SUPPORT

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "RateLimiter.hpp"
#include "ReplicationLog.hpp"
#include "Replicator.hpp"
#include "ScriptEngine.hpp"
#include "Server.hpp"
#include "Settings.hpp"
#include "ShardManager.hpp"
#include "lib/httplib.hpp"

// Fixed at compile time: the shard of a key depends on it, so changing it makes an existing database unreadable
#define NUM_SHARDS 32

namespace Zest {

    /// A matching rule built from a mode and a pattern; `result` is false when the mode or the pattern is invalid.
    struct CreationValidationRuleResult {
        ValidationRule rule;
        bool result;
    };

    /// A user of config.yaml.
    struct UserEntry {
        std::string tokenHash; // sha256 of the token the client sends
        Role role;
        UserLimits limits; // already resolved against the top-level limits
    };

    /// The database: parses commands, checks permissions and values, runs the triggers, writes the WAL and coordinates the shards.
    /// It also owns the TCP server, the HTTP stats server, the script engine and the background flush and archive threads.
    ///
    /// Locking: `writeMtx` protects the configuration and the shard manager's lifetime, not the data. execCmd holds it
    /// in shared mode for the whole command (exclusive for `reload` and `scfg`; `restore` and archiving take it exclusively
    /// for a moment). The public data methods below do not take it, which is why scripts can call them from inside a command.
    ///
    /// Each write method checks the value, runs the `before` triggers, writes to its shard, appends its effect to the WAL
    /// (buffered: execCmd syncs it once per command), then runs the `after` triggers.
    class ZestDB {
    public:
        /// Reads config.yaml, opens the shards, loads the scripts, replays the WAL, starts the TCP server, then starts
        /// the flush and archive threads and prepares the HTTP stats server (main() makes it listen).
        ZestDB();
        /// Reads `config.yaml` from `configDir` instead of the current folder.
        explicit ZestDB(std::filesystem::path configDir);
        /// Stops the database if stop() was not called.
        ~ZestDB();

        /// Value of `key`. KeyNotFound for a missing or expired key, InvalidKey for a key longer than MaxKeySize.
        ResultType get(const std::string& key);
        /// Stores `value` (expiry date in ms, 0 for none), with validation, triggers and WAL.
        ResultType set(const std::string& key, const std::string& value, uint64_t expiresAt = 0);
        /// Deletes `key`, with the `del` triggers and WAL.
        ResultType del(const std::string& key);

        /// Sets (absolute date in ms) or removes (0) the expiry of an existing key. Logged in the WAL; runs no trigger.
        ResultType setTTL(const std::string& key, uint64_t expiresAt);
        /// Seconds before `key` expires, `-1` without expiry, `-2` if it does not exist.
        ResultType getTTL(const std::string& key);

        /// The matching keys and their values as a JSON array of `{key: value}` objects.
        ResultType getBy(ValidationRule& valid);
        /// Writes `value` to every matching key. Goes key by key through set() while triggers are loaded.
        ResultType setBy(ValidationRule& valid, const std::string& value);
        /// Deletes every matching key. Goes key by key through del() while triggers are loaded.
        ResultType delBy(ValidationRule& valid);

        /// Adds `delta` to an integer value (a missing key counts as 0); the new value is the response. Runs the `after` triggers only.
        ResultType incr(const std::string& key, int64_t delta);
        /// Stores `value` only if `key` does not exist; KeyExists otherwise.
        ResultType setnx(const std::string& key, const std::string& value);
        /// Replaces the value only if it equals `expected`; ValueMismatch otherwise. The expiry date is kept.
        ResultType cas(const std::string& key, const std::string& expected, const std::string& value);

        /// Replaces the data with an archive of ArchiveStoragePath (a file name, not a path). The archive is extracted
        /// first; commands are blocked only while the shards are swapped. The previous data is kept if anything fails.
        ResultType restore(const std::string& zipname);
        /// The archives of ArchiveStoragePath as a sorted JSON array.
        ResultType listSaves() const;
        /// `user add`: creates `name` with `password` and `roleName` (`write` when empty), in memory and in config.yaml.
        ResultType addUser(const std::string& name, const std::string& password, const std::string& roleName);
        /// `user passwd`: sets a new password and closes the user's open sessions.
        ResultType setUserPassword(const std::string& name, const std::string& password);
        /// `user role`: changes the role; open sessions get it from their next command.
        ResultType setUserRole(const std::string& name, const std::string& roleName);
        /// `user del`: removes the user and closes its open sessions.
        ResultType deleteUser(const std::string& name);
        /// `user ls`: the users and their roles as a JSON array, sorted by name.
        ResultType listUsers() const;

        /// `info` / `stats`: the number of keys, the size of DbPath on disk, the cache hit ratio of the reads and the most
        /// recent archive, as a JSON object. The caller holds `writeMtx`.
        ResultType info() const;

        /// `"true"` or `"false"`, from the index only.
        ResultType exists(const std::string& key);
        /// Number of live keys matching `valid`.
        ResultType count(const ValidationRule& valid);
        /// Sorted live keys matching `valid` as a JSON array.
        ResultType keys(const ValidationRule& valid);

        /// Validates and saves a Lua script, then registers its functions and triggers (see ScriptEngine::load).
        ResultType loadScript(const std::string& name, const std::string& code);
        /// Deletes a script with its functions and triggers.
        ResultType dropScript(const std::string& name);
        /// Reloads `<DbPath>/scripts/<name>.lua` after it was edited on disk.
        ResultType reloadScript(const std::string& name);
        /// Reloads the whole scripts folder; the response lists the scripts loaded and those that failed.
        ResultType reloadScripts();
        /// The scripts, functions and triggers as JSON.
        ResultType listScripts() const;
        /// Runs a function registered by a script; its return value is the response.
        ResultType call(const std::string& name, const std::vector<std::string>& args);

        /// Builds the key matcher of a mode (`sw`, `ct`, `ew`, `re`) and a pattern. A regex is refused when it is invalid,
        /// uses back-references or looks too expensive (see isRegexTooComplex in ZestDB.cpp).
        CreationValidationRuleResult createValidationRule(std::string_view mode, std::string_view pattern) const;

        /// Parses and runs one command line, after checking that `role` allows it.
        /// @param syncWal  sync the WAL before returning; the server passes false and syncs once per batch (group commit).
        /// @param user     the authenticated user, visible to scripts through `zest.caller()`.
        ResultType execCmd(const std::string& command, Role role = Role::Admin, bool syncWal = true, const std::string& user = "");
        /// Syncs the WAL of every shard. @return false if one failed, in which case the batch must not be acknowledged.
        bool syncWAL();
        /// True if `token` is valid for `username`.
        bool validateToken(const std::string& username, const std::string& token) const;
        /// The role of `username` if `sha256(token)` matches its tokenHash (compared in constant time), nullopt otherwise.
        std::optional<Role> authenticate(const std::string& username, const std::string& token) const;
        /// The current role of `username`, nullopt if the user was removed (checked on every command, so a reload applies at once).
        std::optional<Role> roleOf(const std::string& username) const;
        /// Like roleOf(), but nullopt as well once the user's password changed: `tokenHash` is the one the session logged in with.
        std::optional<Role> roleOf(const std::string& username, const std::string& tokenHash) const;
        /// The rate limiter of `username`, shared by all of its sessions; null when the user has no limit.
        std::shared_ptr<UserLimiter> limiterOf(const std::string& username) const;
        /// False while `username` is locked out after too many failed authentications.
        bool authAllowed(const std::string& username);
        /// Counts a failed authentication (5 in a row lock the user out for 60 s), or forgets the failures after a success.
        void recordAuthResult(const std::string& username, bool success);
        /// True if `ip` matches the `NetworkValidation` regex.
        bool isAddressAllowed(const std::string& ip);
        bool isRunning() const;
        /// Stops the servers (letting each session send the reply of the request in progress), then flushes and stops
        /// the shards. Does nothing the second time.
        void stop();

        /// Starts keeping the writes for the replicas (see ReplicationLog), from the next one on. Does nothing on a
        /// replica, or the second time.
        void startReplicationLog();

        ReplicationLog& getReplicationLog() {
            return *this->replicationLog;
        }

        /// Random id of the current history of writes: sequence numbers only mean something within one history id.
        /// A new one is drawn at startup and by a `restore`.
        std::string getHistoryId() const {
            std::lock_guard<std::mutex> lock(this->historyMtx);
            return this->historyId;
        }

        /// `replicate <historyId> <fromSeq>` from a replica at `remoteIp`: checks it may replicate, starts the
        /// replication log, then answers `{"mode":"stream"|"fullsync","historyId":..,"nextSeq":..}`. `stream` when the
        /// log still holds every write from `fromSeq` of the same history, `fullsync` otherwise. `knownHistoryId` is this
        /// primary's id as the replica got it at its last handshake (`-` for none).
        ResultType replicationHandshake(const std::string& remoteIp, const std::string& knownHistoryId, uint64_t fromSeq, bool* streamOut = nullptr);
        /// For a full sync: an archive of the database in `<ArchiveStoragePath>/.replication/`, and the sequence number
        /// of the first write it does not hold, from which the replica streams. The caller deletes the file.
        /// @return false if another archive or a restore is in progress, or the archive could not be written.
        bool createReplicaSnapshot(std::filesystem::path& zipPath, uint64_t& seq);
        /// On a replica: runs one write received from the primary, like the WAL replay does (no trigger, no check of
        /// the value or of read-only mode), but logged in the local WAL.
        ResultType applyReplicated(const std::string& command);
        /// Replaces the database with the archive at `archivePath` (`restore` takes it from ArchiveStoragePath).
        ResultType restoreArchive(const std::filesystem::path& archivePath);

        /// The replica's link to its primary, or null on a primary.
        const Replicator* getReplicator() const {
            return this->replicator.get();
        }

        /// The active configuration. Read-only from outside: it changes through `scfg` and `reload`.
        const Settings& getSettings() const {
            return this->settings;
        }

        /// The context main() runs the signal handler on.
        asio::io_context& ioContext() {
            return this->ioCtx;
        }

        /// The HTTP stats server, prepared by the constructor; main() makes it listen on `WebPort`.
        httplib::Server& httpServer() {
            return *this->srv;
        }

        /// Every statistic in the Prometheus text format, served at `/metrics` by the HTTP server.
        std::string prometheusMetrics();

        /// Serialises a result as `{"affectedRows":..,"code":..,"response":..}`. A response that is valid JSON (array or
        /// object) is embedded as is, anything else becomes a JSON string.
        static std::string responseToJson(const ResultType& resp);

    private:
        /// A command line split into words, handed to the handler of its command.
        struct Command {
            std::string_view line; // the command without its surrounding blanks
            std::vector<std::string_view> words;
            std::string name;        // the first word, lowercased, with its alias resolved (`get` -> `g`)
            bool rowsLogged = false; // setby / delby logged each row themselves, not the command
        };

        /// The role a command needs.
        enum class Access {
            Read,
            Write, // also makes execCmd sync the WAL after it
            Admin
        };

        /// How a command holds `writeMtx`: shared by most, exclusive to replace the settings, not at all for the
        /// commands that take it themselves (flush, save, restore).
        enum class Locking {
            Shared,
            Exclusive,
            None
        };

        using Handler = ResultType (ZestDB::*)(Command&);

        struct CommandSpec {
            Handler handler;
            Access access;
            Locking locking;
        };

        /// Every command name (short forms, after alias resolution) and how execCmd runs it.
        static const std::unordered_map<std::string_view, CommandSpec>& commandTable();
        /// Trims `raw`, splits it into words and resolves the command name. False for a blank line.
        static bool parseCommand(const std::string& raw, Command& out);

        // One handler per command (Commands.cpp): parse the arguments, call the method, return its result

        ResultType cmdGet(Command& c);
        ResultType cmdSet(Command& c);
        ResultType cmdSetex(Command& c);
        ResultType cmdExpire(Command& c);
        ResultType cmdPersist(Command& c);
        ResultType cmdTtl(Command& c);
        ResultType cmdSetnx(Command& c);
        ResultType cmdCas(Command& c);
        ResultType cmdIncr(Command& c);
        ResultType cmdDel(Command& c);
        ResultType cmdGetBy(Command& c);
        ResultType cmdSetBy(Command& c);
        ResultType cmdDelBy(Command& c);
        ResultType cmdExists(Command& c);
        ResultType cmdCount(Command& c);
        ResultType cmdKeys(Command& c);
        ResultType cmdHelp(Command& c);
        ResultType cmdInfo(Command& c);
        ResultType cmdUser(Command& c);
        ResultType cmdFlush(Command& c);
        ResultType cmdReload(Command& c);
        ResultType cmdSetConfig(Command& c);
        ResultType cmdGetConfig(Command& c);
        ResultType cmdRestore(Command& c);
        ResultType cmdSave(Command& c);
        ResultType cmdListSaves(Command& c);
        ResultType cmdScript(Command& c);
        ResultType cmdCall(Command& c);

        /// True when triggers must run: some are loaded and the WAL is not being replayed.
        bool scripted() const;
        /// True while the WAL is replayed or a replicated write runs: the values were checked when first written.
        bool unchecked() const;
        /// True when setby / delby must log each key they touch, not the command: for the triggers, or for the replicas.
        bool logsRows() const;
        /// Runs the `before set` triggers on `value` (which they may change), then checks the new value again.
        ResultType beforeWrite(const std::string& key, std::string& value);
        /// After a successful write: appends `walCommand` to the WAL. Called under the shard's write order lock, so the
        /// records are numbered in the order the writes were applied. Returns `result`, or WalWriteFailed.
        ResultType logWrite(const std::string& key, const std::string& walCommand, ResultType result);
        /// After a successful, logged write: runs the `after` triggers, without the write order lock (they may write).
        /// Returns `result`, or the trigger error (the write itself stays done).
        ResultType afterWrite(TriggerEvent event, const std::string& key, const std::string& value, ResultType result);
        /// setBy with an explicit choice: `perRow` goes key by key through set(), so triggers run and each row is logged.
        ResultType setBy(ValidationRule& valid, const std::string& value, bool perRow);
        /// delBy with an explicit choice, like setBy.
        ResultType delBy(ValidationRule& valid, bool perRow);
        /// One row of setBy when triggers are loaded: set() for an existing key, keeping its expiry date (also logged).
        ResultType replace(const std::string& key, const std::string& value);
        /// incr that also returns the key's expiry date, needed to log the new value with it.
        ResultType incrBy(const std::string& key, int64_t delta, uint64_t& expiresAt);
        /// cas that also returns the key's expiry date, needed to log the new value with it.
        ResultType casBy(const std::string& key, const std::string& expected, const std::string& value, uint64_t& expiresAt);
        /// Checks a write: read-only mode, key and value sizes, empty value, jsonOnly. Most checks are skipped during the
        /// WAL replay, so a stricter configuration does not drop data that was accepted before.
        ResultType checkWrite(const std::string& key, const std::string& value) const;
        /// Loads config.yaml and creates DbPath.
        void boot();
        bool validateKey(std::string_view key) const;
        bool validateValue(std::string_view value) const;
        bool isJsonValid(std::string_view value) const;
        /// Flushes every shard under `writeMtx`.
        void flush();
        /// Flushes every shard; the caller holds `writeMtx` exclusively.
        void flushLocked();
        /// Replays the WAL left by the previous run, with the checks and triggers off, then flushes.
        void replayWAL();
        /// The text of the `help` command.
        std::string help() const;
        /// Appends a record to the WAL of `key`'s shard; does nothing during the replay.
        bool appendToWAL(const std::string& key, const std::string& command);

        /// Parses config.yaml into Settings and fills `usersOut`; throws on a missing file or an invalid setting.
        Settings loadConfig(std::unordered_map<std::string, UserEntry>& usersOut);
        /// `reload`: rereads config.yaml. Refuses changes to the paths, ports, SSL and connection limits.
        ResultType reloadConfig();
        /// Writes the current settings back to config.yaml (after `scfg`).
        void setConfig();
        /// The current settings as JSON (`gcfg`).
        std::string getConfig() const;

        /// Zips DbPath into ArchiveStoragePath, then applies ArchiveRetention.
        /// Commands are blocked only while the shards are flushed and the file sizes recorded; the files are then copied
        /// up to those sizes (compaction paused), and the zip is built from the copy.
        bool createArchive();
        /// Flushes, then zips the database to `<ArchiveStoragePath>/<subfolder>/<name>` (see createArchive()). `seq`, if
        /// not null, receives the sequence number of the first write the archive does not hold. The caller holds the
        /// `archiving` flag.
        bool writeArchive(const std::filesystem::path& subfolder, const std::string& name, std::filesystem::path& written, uint64_t* seq);

        // Declared first so they are built before, and destroyed after, everything that uses them
        Settings settings;
        asio::io_context ioCtx;
        std::unique_ptr<httplib::Server> srv;

        std::unordered_map<std::string, UserEntry> users;
        mutable std::shared_mutex usersMtx;
        std::unordered_map<std::string, std::shared_ptr<UserLimiter>> limiters; // users with a limit; guarded by usersMtx

        /// Makes `limiters` match `users`, keeping the buckets of the users still limited; the caller holds usersMtx exclusively.
        void rebuildLimiters();

        /// Applies `change` to a copy of the users, writes it to config.yaml, then makes it the active list. When `revoke`
        /// is set, the open sessions of `name` are closed afterwards. Nothing changes if `change` or the write fails.
        ResultType changeUsers(const std::string& name, bool revoke, const std::function<ResultType(std::unordered_map<std::string, UserEntry>&)>& change);
        /// Rewrites the `users` list of config.yaml from `updated`, keeping the other fields of each entry (its limits),
        /// through a temporary file and a rename. @return false if the file could not be written.
        bool writeUsers(const std::unordered_map<std::string, UserEntry>& updated);

        std::mutex authFailuresMtx;
        std::unordered_map<std::string, std::pair<unsigned int, std::chrono::steady_clock::time_point>> authFailures; // user -> failures, locked until

        std::atomic<bool> running{ true };
        std::atomic<bool> archiving{ false }; // an archive or a restore is in progress: only one at a time
        std::unique_ptr<ShardManager> shardManager;
        std::unique_ptr<ReplicationLog> replicationLog;
        std::unique_ptr<Replicator> replicator; // on a replica only
        std::filesystem::path configDir;
        std::string historyId;
        mutable std::mutex historyMtx;
        /// Draws a new historyId and closes the replication links: the data changed without numbered writes (restore).
        void newHistory();
        std::unique_ptr<ScriptEngine> scriptEngine; // declared after shardManager, so it is destroyed first
        std::unique_ptr<Server> socket;

        std::atomic<bool> replaying; // the WAL is being replayed: no new WAL records, no checks, no triggers

        std::shared_mutex writeMtx;
        std::atomic<bool> isFlushing;

        std::mutex flushThreadMtx;
        std::mutex saveThreadMtx;
        std::condition_variable_any threadCV; // wakes the flush and archive threads on stop() or a configuration change

        std::jthread flushThread;
        std::jthread saveThread;
    };

    /// Hex-encoded SHA-256 of `str`.
    std::string sha256(const std::string& str);

} // namespace Zest
