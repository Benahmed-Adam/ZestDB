# Building and testing

## Requirements

- A C++20 compiler: GCC or Clang on Linux/macOS, MSVC (Visual Studio 2022 or 2026) or MinGW on Windows.
- CMake 3.25+, `make` for the Makefile shortcuts (Linux/macOS), and Perl 5 when OpenSSL has to be built (plus NASM with MSVC).

No library has to be installed: on the first build, CMake (`FetchContent`) downloads and builds pinned versions of the dependencies. OpenSSL is the exception: an installed OpenSSL 3 (e.g. `libssl-dev`) is used when found, otherwise it is built from source.

| Library | Version | Used for |
| --- | --- | --- |
| OpenSSL | system 3.x, else 3.5.9 (via openssl-cmake 4.1.0) | TLS, SHA-256 |
| Asio (standalone) | 1.38.2 | TCP/SSL server |
| libzip + zlib | 1.12.1 + 1.3.2 | archives |
| libzippp | 7.1-1.10.1 | archives (C++ wrapper) |
| Lua | 5.4.8 | scripting (compiled as C++, so Lua errors unwind as C++ exceptions) |
| sol2 | 3.5.0 | C++ binding for Lua |
| Catch2 | 3.16.1 | test suite |

The vendored headers in `src/lib/` (httplib, nlohmann/json, fkyaml) are part of the repository. The first build takes about a minute, or two when OpenSSL is built from source; the dependencies are kept in `build/<Release|Debug>/_deps` and reused by later builds (`make clean` removes them).

## Compile

```bash
make
```
The resulting binary is placed in `bin/zestdb`. `make` is a thin wrapper around CMake; the equivalent direct commands are:
```bash
cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release --parallel
```

Debug build:
```bash
make DEBUG=1
```

### Windows

Use CMake directly (the Makefile relies on Unix tools), from a PowerShell where the **x64** MSVC environment is loaded (the default "Developer PowerShell" is x86):
```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
cmake -S . -B build\msvc -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR="C:/Program Files/OpenSSL-Win64"
cmake --build build\msvc --parallel
$env:PATH = "C:\Program Files\OpenSSL-Win64\bin;$env:PATH"   # the OpenSSL DLLs, needed to run zestdb.exe and the tests
ctest --test-dir build\msvc --output-on-failure
```
OpenSSL 3 and 4 both work. Building OpenSSL from source there needs Strawberry Perl and NASM; installing OpenSSL 3 beforehand (and setting `OPENSSL_ROOT_DIR` if CMake does not find it) avoids that step.

## Tests

The project ships a test suite built with Catch2, covering every subsystem:

| Area | File |
| --- | --- |
| Data segments | `tests/test_DataSegment.cpp` |
| Index manager | `tests/test_IndexManager.cpp` |
| Storage manager | `tests/test_StorageManager.cpp` |
| Shard | `tests/test_Shard.cpp` |
| Shard manager | `tests/test_ShardManager.cpp` |
| WAL | `tests/test_WAL.cpp` |
| LRU cache | `tests/test_LRUCache.cpp` |
| Thread pool | `tests/test_ThreadPool.cpp` |
| Performance monitoring | `tests/test_PerfMonitoring.cpp` |
| Logger | `tests/test_Logger.cpp` |
| Server protocol | `tests/test_Server_Protocol.cpp` |
| Server sessions (auth, payload limit) | `tests/test_Server_Session.cpp` |
| ZestDB commands | `tests/test_ZestDB_Commands.cpp` |
| ZestDB utilities | `tests/test_ZestDB_Utils.cpp` |
| ZestDB validation | `tests/test_ZestDB_Validation.cpp` |
| Lua scripting, including every file of `scripts_examples/` | `tests/test_ScriptEngine.cpp` |

```bash
make tests
```
All test files are compiled into a single executable, `build/Release/zestdb_tests` (`build/Debug/` with `DEBUG=1`), and `make tests` runs each test case through `ctest`. To run a subset directly, use Catch2 tags or names:
```bash
./build/Release/zestdb_tests "[shard]"
```

## Code formatting

```bash
make format
```
Uses `clang-format` with the provided `.clang-format` configuration.

## Continuous integration

- `c-cpp.yml` (workflow `CI`) runs on every push and pull request to `main`: build and tests on Linux and on Windows (MSVC). On pull requests it then checks that the Docker image builds.
- `docker.yml` runs once `CI` passes on a push to `main` (a direct commit or a merged pull request), and builds the image once and pushes it to Docker Hub. It needs the `DOCKERHUB_USERNAME` and `DOCKERHUB_TOKEN` repository secrets.

## Project structure

```
├── CMakeLists.txt                # build definition, fetches the dependencies
├── Makefile                      # build / test / run / format targets (wraps CMake)
├── config.yaml                   # runtime configuration
├── Dockerfile                    # multi-stage image build
├── docker/config.yaml            # default configuration of the image
├── .github/workflows/            # CI and Docker Hub pipelines
├── docs/                         # documentation
├── src/                          # C++ source
│   ├── ZestDB.{hpp,cpp}          # main facade, commands, config, archiving
│   ├── ScriptEngine.{hpp,cpp}    # Lua scripts, functions and triggers
│   ├── Server.{hpp,cpp}          # Asio TCP/SSL server and protocol
│   ├── Settings.hpp              # settings, result codes, validation rules
│   ├── ShardManager.{hpp,cpp}    # shard routing and parallel bulk ops
│   ├── Shard.{hpp,cpp}           # per-shard storage engine + compactor
│   ├── IndexManager.{hpp,cpp}    # in-memory key index
│   ├── StorageManager.{hpp,cpp}  # segment lifecycle management
│   ├── DataSegment.{hpp,cpp}     # append-only value segments
│   ├── LRUCache.{hpp,cpp}        # per-shard LRU cache
│   ├── WAL.{hpp,cpp}             # write-ahead log
│   ├── PerfMonitoring.{hpp,cpp}  # statistics counters
│   ├── ThreadPool.{hpp,cpp}      # parallel task execution
│   ├── Logger.{hpp,cpp}          # multi-level colored logging
│   ├── File.hpp                  # fsync helper
│   ├── main.cpp                  # entry point, REPL, signal handling
│   └── lib/                      # vendored third-party headers
├── tests/                        # Catch2 unit tests
├── scripts_examples/             # Lua script examples
└── benchmark/                    # Python benchmark scripts and results
```
