cmake_minimum_required(VERSION 3.16)
project(ZestDB VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wsign-conversion -Wshadow -Wunused -Werror)
endif()

find_package(OpenSSL REQUIRED)

file(GLOB SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(zestdb ${SOURCES})

target_include_directories(zestdb PRIVATE "${CMAKE_SOURCE_DIR}/src")

if(WIN32)
    target_link_libraries(zestdb OpenSSL::SSL OpenSSL::Crypto ws2_32)
elseif(APPLE)
    target_link_libraries(zestdb OpenSSL::SSL OpenSSL::Crypto pthread)
else()
    target_link_libraries(zestdb OpenSSL::SSL OpenSSL::Crypto pthread)
endif()

option(DEBUG "Build with debug symbols" OFF)
if(DEBUG)
    target_compile_options(zestdb PRIVATE -g -O0)
else()
    target_compile_options(zestdb PRIVATE -O3)
endif()