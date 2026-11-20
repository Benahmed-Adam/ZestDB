cmake_minimum_required(VERSION 3.25)

project(ZestDB LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # compile_commands.json, read by clang-tidy

# Debugging builds (GCC / Clang): ZESTDB_SANITIZE=address,undefined or thread, ZESTDB_COVERAGE=ON
set(ZESTDB_SANITIZE "" CACHE STRING "Sanitizers to build with, e.g. address,undefined or thread")
option(ZESTDB_COVERAGE "Instrument zestcore for code coverage (gcov)" OFF)
if(ZESTDB_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "ZESTDB_SANITIZE needs GCC or Clang")
    endif()
    # Applied before the dependencies are added, so Lua, zlib and libzip are instrumented too (TSan needs that);
    # -fno-sanitize-recover makes the first error fail the test instead of only printing it
    add_compile_options(-fsanitize=${ZESTDB_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${ZESTDB_SANITIZE})
endif()

include(FetchContent)
set(BUILD_SHARED_LIBS OFF)

find_package(OpenSSL 3 QUIET)
if(OpenSSL_FOUND)
    message(STATUS "Using system OpenSSL ${OPENSSL_VERSION}")
else()
    message(STATUS "OpenSSL 3 not found on the system, building OpenSSL 3.5.9 from source")
    set(OPENSSL_TARGET_VERSION 3.5.9)
    FetchContent_Declare(openssl-cmake
        URL https://github.com/jimmy-park/openssl-cmake/archive/refs/tags/4.1.0.tar.gz
        SYSTEM)
    FetchContent_MakeAvailable(openssl-cmake)
endif()

set(ZLIB_BUILD_SHARED OFF)
set(ZLIB_BUILD_TESTING OFF)
set(ZLIB_INSTALL OFF)
FetchContent_Declare(ZLIB
    URL https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz
    SYSTEM
    OVERRIDE_FIND_PACKAGE)
FetchContent_MakeAvailable(ZLIB)
add_library(ZLIB::ZLIB ALIAS zlibstatic)

foreach(opt ENABLE_COMMONCRYPTO ENABLE_GNUTLS ENABLE_MBEDTLS ENABLE_OPENSSL ENABLE_WINDOWS_CRYPTO ENABLE_BZIP2 ENABLE_LZMA ENABLE_ZSTD
            BUILD_TOOLS BUILD_REGRESS BUILD_OSSFUZZ BUILD_EXAMPLES BUILD_DOC LIBZIP_DO_INSTALL)
    set(${opt} OFF)
endforeach()
FetchContent_Declare(libzip
    URL https://github.com/nih-at/libzip/archive/refs/tags/v1.12.1.tar.gz
    SYSTEM)
FetchContent_MakeAvailable(libzip)

FetchContent_Declare(libzippp
    URL https://github.com/ctabin/libzippp/archive/refs/tags/libzippp-v7.1-1.10.1.tar.gz
    SOURCE_SUBDIR none)
FetchContent_Declare(asio
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-38-2.tar.gz
    SOURCE_SUBDIR none)
FetchContent_Declare(lua
    URL https://www.lua.org/ftp/lua-5.4.8.tar.gz
    SOURCE_SUBDIR none)
FetchContent_Declare(sol2
    URL https://github.com/ThePhD/sol2/archive/refs/tags/v3.5.0.tar.gz
    SOURCE_SUBDIR none)
FetchContent_MakeAvailable(libzippp asio lua sol2)

file(COPY ${libzippp_SOURCE_DIR}/src/libzippp.h DESTINATION ${CMAKE_BINARY_DIR}/include/libzippp)
add_library(libzippp STATIC ${libzippp_SOURCE_DIR}/src/libzippp.cpp)
target_include_directories(libzippp SYSTEM PUBLIC ${CMAKE_BINARY_DIR}/include ${libzippp_SOURCE_DIR}/src)
target_link_libraries(libzippp PUBLIC libzip::zip)

add_library(asio INTERFACE)
# The archive's asio/include is a symlink to include, which MSVC cannot open on Windows: prefer the real folder
if(EXISTS ${asio_SOURCE_DIR}/include/asio.hpp)
    set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/include)
elseif(EXISTS ${asio_SOURCE_DIR}/asio/include/asio.hpp)
    set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include)
else()
    message(FATAL_ERROR "asio.hpp not found in ${asio_SOURCE_DIR}")
endif()
message(STATUS "Using asio headers from ${ASIO_INCLUDE_DIR}")
target_include_directories(asio SYSTEM INTERFACE ${ASIO_INCLUDE_DIR})

# Lua ships no CMake: build the library from its sources, without the lua/luac programs
file(GLOB LUA_SOURCES ${lua_SOURCE_DIR}/src/*.c)
list(REMOVE_ITEM LUA_SOURCES ${lua_SOURCE_DIR}/src/lua.c ${lua_SOURCE_DIR}/src/luac.c)
# Built as C++ so a Lua error is a C++ exception that unwinds our frames (destructors run) instead of a longjmp over them
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua SYSTEM PUBLIC ${lua_SOURCE_DIR}/src)
if(UNIX)
    target_compile_definitions(lua PRIVATE LUA_USE_POSIX)
    target_link_libraries(lua PUBLIC m)
endif()

add_library(sol2 INTERFACE)
target_include_directories(sol2 SYSTEM INTERFACE ${sol2_SOURCE_DIR}/include)
target_compile_definitions(sol2 INTERFACE SOL_ALL_SAFETIES_ON=1 SOL_USING_CXX_LUA=1)
target_link_libraries(sol2 INTERFACE lua)

FetchContent_Declare(Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.1.tar.gz
    SYSTEM)

find_package(Threads REQUIRED)

file(GLOB ZEST_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM ZEST_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

add_library(zestcore STATIC ${ZEST_SOURCES})
target_include_directories(zestcore PUBLIC src)
if(ZESTDB_COVERAGE)
    if(MSVC)
        message(FATAL_ERROR "ZESTDB_COVERAGE needs GCC or Clang")
    endif()
    target_compile_options(zestcore PRIVATE --coverage -O0)
    target_link_options(zestcore PUBLIC --coverage)
endif()
target_link_libraries(zestcore PUBLIC asio sol2 OpenSSL::SSL OpenSSL::Crypto libzippp Threads::Threads)
# OFF only to list every warning in one build instead of stopping at the first
option(ZESTDB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
if(MSVC)
    target_compile_options(zestcore PUBLIC /W4 /wd4996 /wd4068 $<$<BOOL:${ZESTDB_WARNINGS_AS_ERRORS}>:/WX> /permissive- /Zc:preprocessor /utf-8 /bigobj /Zc:__cplusplus)
    target_compile_definitions(zestcore PUBLIC _REGEX_MAX_COMPLEXITY_COUNT=100000)
else()
    target_compile_options(zestcore PUBLIC
        -Wall -Wextra -Wpedantic -Wsign-conversion -Wshadow -Wunused -Wnon-virtual-dtor -Wno-deprecated-declarations
        $<$<BOOL:${ZESTDB_WARNINGS_AS_ERRORS}>:-Werror>
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Debug>:-g3 -O0>)
    if(ZESTDB_SANITIZE)
        # The sanitizer instrumentation makes GCC report false positives inside std::regex
        target_compile_options(zestcore PUBLIC -Wno-maybe-uninitialized)
    endif()
    # GCC reports a false positive inside sol2's table access once inlined (stack_field.hpp)
    set_source_files_properties(src/ScriptEngine.cpp PROPERTIES COMPILE_OPTIONS -Wno-array-bounds)
endif()
if(WIN32)
    # windows.h defines min/max and many short macros (ERROR, small...): keep it lean
    target_compile_definitions(zestcore PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    target_link_libraries(zestcore PUBLIC ws2_32 mswsock crypt32)
endif()

add_executable(zestdb src/main.cpp)
target_link_libraries(zestdb PRIVATE zestcore)
# $<0:> stops multi-config generators (Visual Studio) from adding a bin/<Config>/ subfolder
set_target_properties(zestdb PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/$<0:>)

option(ZESTDB_BUILD_TESTS "Build the Catch2 test suite" ON)
if(ZESTDB_BUILD_TESTS)
    FetchContent_MakeAvailable(Catch2)
    enable_testing()
    file(GLOB ZEST_TESTS CONFIGURE_DEPENDS tests/test_*.cpp)
    add_executable(zestdb_tests ${ZEST_TESTS})
    target_link_libraries(zestdb_tests PRIVATE zestcore Catch2::Catch2WithMain)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(zestdb_tests)
endif()
