cmake_minimum_required(VERSION 3.25)

project(ZestDB LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
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
FetchContent_MakeAvailable(libzippp asio)

file(COPY ${libzippp_SOURCE_DIR}/src/libzippp.h DESTINATION ${CMAKE_BINARY_DIR}/include/libzippp)
add_library(libzippp STATIC ${libzippp_SOURCE_DIR}/src/libzippp.cpp)
target_include_directories(libzippp SYSTEM PUBLIC ${CMAKE_BINARY_DIR}/include ${libzippp_SOURCE_DIR}/src)
target_link_libraries(libzippp PUBLIC libzip::zip)

add_library(asio INTERFACE)
target_include_directories(asio SYSTEM INTERFACE ${asio_SOURCE_DIR}/asio/include)

FetchContent_Declare(Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.1.tar.gz
    SYSTEM)

find_package(Threads REQUIRED)

file(GLOB ZEST_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM ZEST_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

add_library(zestcore STATIC ${ZEST_SOURCES})
target_include_directories(zestcore PUBLIC src)
target_link_libraries(zestcore PUBLIC asio OpenSSL::SSL OpenSSL::Crypto libzippp Threads::Threads)
target_compile_options(zestcore PUBLIC
    -Wall -Wextra -Wpedantic -Wsign-conversion -Wshadow -Wunused -Werror -Wnon-virtual-dtor -Wno-deprecated-declarations
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Debug>:-g3 -O0>)

add_executable(zestdb src/main.cpp)
target_link_libraries(zestdb PRIVATE zestcore)
set_target_properties(zestdb PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

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
