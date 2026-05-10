cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
project(MYMQ VERSION 4.0.0 LANGUAGES CXX)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
)

set(SRC_FILES
    src/MYMQ_C.cpp
    src/MYMQ_Client.cpp
    src/ClientProtocol.cpp
)

set(TP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty")

# ==========================================
# [修改] 告诉 CMake 去哪里找你刚刚导出的 mimalloc 包
# ==========================================
list(APPEND CMAKE_PREFIX_PATH "${TP_DIR}/mimalloc")
find_package(mimalloc CONFIG REQUIRED)

set(MYMQ_THIRDPARTY_INCLUDE_DIRS
    "${TP_DIR}/tbb/include"
    "${TP_DIR}/tbb/include/oneapi"
    "${TP_DIR}/zlib/include"
    "${TP_DIR}/zstd/include"
    "${TP_DIR}/openssl3/include"
    "${TP_DIR}/readerwriterqueue/include"
    "${TP_DIR}/nlohmann"
    "${TP_DIR}/concurrentqueue/include"
    # 这里不需要手动加 mimalloc/include 了，find_package 会自动处理
)

set(INTERNAL_HEADERS
    src/MYMQ_Client.h
    src/MYMQ_innercodes.h
    src/ClientProtocol.h
)

add_library(mymq_lib STATIC
    ${SRC_FILES}
    ${INTERNAL_HEADERS}
)

target_include_directories(mymq_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${MYMQ_THIRDPARTY_INCLUDE_DIRS}
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(mymq_lib
    PRIVATE
    # --- Zlib ---
    "${TP_DIR}/zlib/lib/libz.a"
    # --- Zstd ---
    "${TP_DIR}/zstd/lib/libzstd.a"
    # --- TBB ---
    "${TP_DIR}/tbb/lib/libtbb12.dll.a"
    # --- OpenSSL ---
    "${TP_DIR}/openssl3/lib/libcrypto.dll.a"
    "${TP_DIR}/openssl3/lib/libssl.dll.a"

    $<IF:$<TARGET_EXISTS:mimalloc-static>,mimalloc-static,mimalloc>

    # --- Windows 网络库 ---
    ws2_32
    iphlpapi
    ssp
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples")
    add_subdirectory(examples)
endif()
