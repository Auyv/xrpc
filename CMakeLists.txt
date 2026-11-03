# XRPC — PC 构建
#
# 核心库只依赖部署方提供的 xrpc_port.h。没有这个头文件时，打开
# XRPC_BUILD_TESTS 会使用 tests/stubs/ 下的最小桩，桩只用于测试，不进发布。
#
#   cmake -S . -B build -G Ninja -DXRPC_BUILD_TESTS=ON
#   cmake --build build
#   ctest --test-dir build --output-on-failure

cmake_minimum_required(VERSION 3.16)

# 版本号只写在 xrpc_common.h
file(STRINGS common/include/xrpc/xrpc_common.h XRPC_VERSION_LINES
     REGEX "^#define XRPC_VERSION_(MAJOR|MINOR|PATCH) ")
foreach(part MAJOR MINOR PATCH)
    string(REGEX MATCH "XRPC_VERSION_${part} +\\(([0-9]+)\\)" _ "${XRPC_VERSION_LINES}")
    set(XRPC_VERSION_${part} ${CMAKE_MATCH_1})
endforeach()

project(xrpc
        VERSION ${XRPC_VERSION_MAJOR}.${XRPC_VERSION_MINOR}.${XRPC_VERSION_PATCH}
        LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(XRPC_BUILD_TESTS "Build unit and end-to-end tests" OFF)
option(XRPC_ENABLE_UBSAN "Enable UBSan in trap mode (GCC/Clang)" OFF)

# 提供 xrpc_port.h 的目录，可用 -DXRPC_PORT_DIR=<dir> 指定。
# 构建测试且未指定时使用 tests/stubs。
set(XRPC_PORT_DIR "" CACHE PATH "Directory providing platform headers")
if(XRPC_BUILD_TESTS AND NOT XRPC_PORT_DIR)
    set(XRPC_PORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/stubs")
endif()

set(XRPC_SOURCES
    common/fdn.c
    common/xrpc_crc16.c
    common/xrpc_link.c
    common/xrpc_msg_param_list.c
    common/xrpc_node.c
    common/xrpc_node_inner.c
    common/xrpc_notify.c
    common/xrpc_peer.c
    common/xrpc_process.c
    common/xrpc_relay.c
    common/xrpc_wire.c
    common/xrpc_status.c
    common/xrpc_delivery.c
)

add_library(xrpc STATIC ${XRPC_SOURCES})

target_include_directories(xrpc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/common/include
        ${CMAKE_CURRENT_SOURCE_DIR}/common/include/xrpc
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/common
)

if(NOT XRPC_PORT_DIR)
    message(FATAL_ERROR "核心库要用部署方提供的 xrpc_port.h。"
                        "用 -DXRPC_PORT_DIR=<目录> 指定，只跑测试时用 -DXRPC_BUILD_TESTS=ON。"
                        "见 port/README.md")
endif()
target_include_directories(xrpc PUBLIC ${XRPC_PORT_DIR})

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(xrpc PRIVATE -Wall -Wextra)
    if(XRPC_ENABLE_UBSAN)
        # 陷阱模式不依赖运行库，MinGW 上也能用
        set(XRPC_UBSAN_FLAGS -fsanitize=undefined -fsanitize-trap=undefined)
        target_compile_options(xrpc PUBLIC ${XRPC_UBSAN_FLAGS})
        target_link_options(xrpc PUBLIC ${XRPC_UBSAN_FLAGS})
    endif()
endif()

if(XRPC_BUILD_TESTS AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    message(FATAL_ERROR "这份代码不含测试工程，去掉 -DXRPC_BUILD_TESTS=ON")
endif()

if(XRPC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
