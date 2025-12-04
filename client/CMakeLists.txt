cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 定义版本号
project(MYMQ VERSION 3.1.0 LANGUAGES CXX)



configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
)

set(SRC_FILES
    src/MYMQ_C.cpp
    src/MYMQ_Client.cpp
)

set(TP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty")
set(INTERNAL_HEADERS
    src/MYMQ_Client.h
    src/MYMQ_innercodes.h
)

add_library(mymq_lib STATIC
    ${SRC_FILES}
    ${INTERNAL_HEADERS}
)

target_include_directories(mymq_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        # 3. 将自动生成的头文件目录加入搜索路径
        # 这样你在代码里直接写 #include "version.h" 就能找到了
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>

    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src

        # --- 第三方库头文件路径 ---
        "${TP_DIR}/tbb/include"
        "${TP_DIR}/tbb/include/oneapi"
        "${TP_DIR}/zlib/include"
        "${TP_DIR}/zstd/include"
        "${TP_DIR}/openssl3/include"
        "${TP_DIR}/readerwriterqueue/include"
        "${TP_DIR}/nlohmann"
        "${TP_DIR}/concurrentqueue/include"
)

target_link_libraries(mymq_lib
    PRIVATE
    # --- Zlib (静态链接) ---
    "${TP_DIR}/zlib/lib/libz.a"

    # --- Zstd (静态链接) ---
    "${TP_DIR}/zstd/lib/libzstd.a"

    # --- TBB (动态链接导入库) ---
    "${TP_DIR}/tbb/lib/libtbb12.dll.a"

    "${TP_DIR}/openssl3/lib/libcrypto.dll.a"
    "${TP_DIR}/openssl3/lib/libssl.dll.a"
    # --- 系统库 (Windows 网络库) ---
    ws2_32
    ssp
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples")
    add_subdirectory(examples)
endif()
