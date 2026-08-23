
cmake_minimum_required(VERSION 3.14)
project(mapledb VERSION 0.1.0 LANGUAGES CXX)

# 需要使用部分C++17特性
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译选项
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# 自动收集源文件，方便增量开发
file(GLOB_RECURSE MAPLEDB_SOURCES "src/*.cpp")

add_library(mapledb STATIC ${MAPLEDB_SOURCES})

# TODO：确认构建项目和安装项目时的头文件路径是否正确
target_include_directories(mapledb
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src>
)

# target_include_directories(mapledb
#     PUBLIC
#     $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
#     $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
# )

# 测试目标

enable_testing()

function(add_mapledb_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE mapledb)
    target_include_directories(${name}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_mapledb_test(daily_test_01)
