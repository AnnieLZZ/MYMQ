# --- 1. 基本项目设置 ---
cmake_minimum_required(VERSION 3.15)
project(MYMQ VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
)
# --- 2. 查找系统库 ---

# 找到 PkgConfig (用于 zstd)
find_package(PkgConfig REQUIRED)

# 找到 ZLIB
find_package(ZLIB REQUIRED)

# 使用 pkg-config 查找 libzstd
pkg_check_modules(Zstd REQUIRED libzstd)

# ============================================================
# (核心修改) OpenSSL 查找逻辑：优先手动查找 OpenSSL 3
# ============================================================

# 1. 定义我们期望的路径
set(MY_SSL_INC_DIR "/usr/include/openssl3")

# 2. 尝试手动查找库文件 (不要写死小版本号，使用 .so.3)
find_library(SSL_LIB3 NAMES ssl.3 libssl.so.3 PATHS /usr/lib64 /usr/lib NO_DEFAULT_PATH)
find_library(CRYPTO_LIB3 NAMES crypto.3 libcrypto.so.3 PATHS /usr/lib64 /usr/lib NO_DEFAULT_PATH)

if(SSL_LIB3 AND CRYPTO_LIB3 AND EXISTS "${MY_SSL_INC_DIR}")
    message(STATUS "✅ [Manual Override] Found OpenSSL 3 Libraries:")
    message(STATUS "    SSL Lib:    ${SSL_LIB3}") # 修正了变量名
    message(STATUS "    Crypto Lib: ${CRYPTO_LIB3}")
    message(STATUS "    Includes:   ${MY_SSL_INC_DIR}")

    # 手动设置标准变量，以防有其他依赖用了这些变量
    set(OPENSSL_FOUND TRUE)
    set(OPENSSL_INCLUDE_DIR "${MY_SSL_INC_DIR}")
    set(OPENSSL_SSL_LIBRARY "${SSL_LIB3}")
    set(OPENSSL_CRYPTO_LIBRARY "${CRYPTO_LIB3}")
    set(OPENSSL_VERSION "3.0.0")

    # 手动创建 IMPORTED 目标
    add_library(OpenSSL::SSL UNKNOWN IMPORTED)
    set_target_properties(OpenSSL::SSL PROPERTIES
        IMPORTED_LOCATION "${SSL_LIB3}"
        INTERFACE_INCLUDE_DIRECTORIES "${MY_SSL_INC_DIR}"
    )

    add_library(OpenSSL::Crypto UNKNOWN IMPORTED)
    set_target_properties(OpenSSL::Crypto PROPERTIES
        IMPORTED_LOCATION "${CRYPTO_LIB3}"
        INTERFACE_INCLUDE_DIRECTORIES "${MY_SSL_INC_DIR}"
    )

else()
    # 只有在手动查找失败时，才调用标准查找 (可能会回退到 1.1.1)
    message(WARNING " Could not find OpenSSL 3 manually at ${MY_SSL_INC_DIR} or /usr/lib64. Falling back to system default...")
    find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
endif()

# ============================================================

# --- 3. 为您本地的 Nlohmann JSON 和 concurrentqueue 创建目标 (已修正路径) ---

# Nlohmann JSON 库：
# 路径应指向包含 'nlohmann' 文件夹的父目录：nlohmann/include
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json
    INTERFACE
      ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/nlohmann/include
)

# concurrentqueue 库：
# 路径应指向包含 'concurrentqueue.h' 等文件的目录：concurrentqueue/include
add_library(concurrentqueue INTERFACE)
target_include_directories(concurrentqueue
    INTERFACE
      ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/concurrentqueue/include
)

# --- 4. 创建您的 MYMQ 库 ---
file(GLOB_RECURSE MYMQ_SOURCES "src/*.cpp")
add_library(MYMQ ${MYMQ_SOURCES})

# --- 5. 设置包含路径和链接 ---
target_include_directories(MYMQ
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${Zstd_INCLUDE_DIRS}
      # 显式添加 OpenSSL include 路径 (如果是手动找到的，这一步很关键)
      ${OPENSSL_INCLUDE_DIR}
)

# 链接所有依赖
target_link_libraries(MYMQ
    PRIVATE
      # 链接 INTERFACE 库，头文件路径会自动传递
      nlohmann_json
      concurrentqueue

      ZLIB::ZLIB
      ${Zstd_LIBRARIES}
      pthread         # (链接 POSIX 线程库)
      stdc++fs        # (链接 Filesystem 库)
      tbb
      uuid
      OpenSSL::SSL    # 链接我们上面定义的目标
      OpenSSL::Crypto
      dl              # OpenSSL 3 在 Linux 上通常需要链接 libdl
)

# --- 6. 创建可执行文件 ---
add_executable(MyMQ_App main.cpp)

# --- 6a. 复制 config 目录到构建目录 ---
add_custom_target(CopyConfig
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/config"
      "${CMAKE_CURRENT_BINARY_DIR}/config"
    COMMENT "Copying config directory to build directory"
)

# 确保在构建 MyMQ_App 之前，先执行 CopyConfig
add_dependencies(MyMQ_App CopyConfig)

# --- 7. 链接您的库到可执行文件 ---
target_link_libraries(MyMQ_App PRIVATE MYMQ)
