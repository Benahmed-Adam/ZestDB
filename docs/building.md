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

## Code quality checks

Every one of these runs in CI; the commands below run them locally (Linux, GCC or Clang).

### Warnings

GCC builds with `-Wall -Wextra -Wpedantic -Wsign-conversion -Wshadow -Werror`, MSVC with `/W4 /WX`: a warning fails the build. To list every warning in one go instead of stopping at the first, configure with `-DZESTDB_WARNINGS_AS_ERRORS=OFF`.

### Formatting

```bash
make format
```
Formats the code with `clang-format` and the provided `.clang-format`. CI checks `src/` and `tests/` with clang-format 21 (`pip install clang-format==21.1.8`); another version may format some lines differently.

### Static analysis

`.clang-tidy` enables the `bugprone-*`, `performance-*` and `modernize-*` checks, minus a few that only restate a style choice. Any finding fails the CI job. Locally, after a configure (which writes `compile_commands.json`):
```bash
clang-tidy -p build/Release --extra-arg=-Wno-unknown-warning-option src/*.cpp
```

### Sanitizers

`ZESTDB_SANITIZE` builds everything, dependencies included, with the given sanitizers; the first error stops the test run.
```bash
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DZESTDB_SANITIZE=address,undefined
cmake --build build/asan --parallel && ./build/asan/zestdb_tests
```
```bash
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DZESTDB_SANITIZE=thread
cmake --build build/tsan --parallel && TSAN_OPTIONS=halt_on_error=1 ./build/tsan/zestdb_tests
```
On a recent kernel, TSan may stop with `unexpected memory mapping`: run `sudo sysctl vm.mmap_rnd_bits=28` first.

### Coverage

```bash
cmake -S . -B build/coverage -DCMAKE_BUILD_TYPE=Debug -DZESTDB_COVERAGE=ON
cmake --build build/coverage --parallel && (cd build/coverage && ./zestdb_tests)
gcovr --root . --filter src/ --exclude src/lib/ --gcov-ignore-errors=no_working_dir_found --html-details coverage/index.html build/coverage
```
In CI, the summary is shown on the run's page and the HTML report is attached as the `coverage` artifact. There is no minimum.

## Continuous integration

- `c-cpp.yml` (workflow `CI`) runs on every push to `main` and `dev`, and on every pull request into them.
  - The jobs `build` and `build-windows` build and test on Linux and on Windows (MSVC).
  - `format`, `clang-tidy`, `sanitize-asan`, `sanitize-tsan` and `coverage` run the [code quality checks](#code-quality-checks).
  - On pull requests, the job `docker` then checks that the image builds.
  - A new push to a pull request cancels the run of its previous commit.
- `docker.yml` runs once `CI` passes on a push to `main` or `dev` (a merged pull request), builds the image once and pushes it to Docker Hub:
  - from `main`: `latest` and `sha-<commit>`;
  - from `dev`: `dev` and `dev-sha-<commit>`.

  It needs the `DOCKERHUB_USERNAME` and `DOCKERHUB_TOKEN` repository secrets.

## Branches

| Branch | Role | Receives pull requests from |
| --- | --- | --- |
| `main` | Released code; each merge publishes `latest` | `release/*` and `hotfix/*` branches (or `dev`) |
| `dev` | Integration of the next release; each merge publishes `dev` | feature branches, and `main` after a hotfix |
| anything else | Work in progress, no rules | |

`main` and `dev` are protected by two repository rulesets:
- no direct push: every change goes through a pull request (no approval required);
- the checks `build`, `build-windows`, `docker`, `format`, `clang-tidy`, `sanitize-asan` and `sanitize-tsan` must pass (`coverage` is informative);
- review threads must be resolved;
- no force-push, no deletion.

Into `main`, the branch must be up to date with `main` and only merge commits are allowed. This keeps `main` and `dev` sharing their history, which squashing would break. Into `dev`, merge commits and squash merges are both allowed.

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
