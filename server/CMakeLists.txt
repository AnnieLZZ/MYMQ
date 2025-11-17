# --- 1. 基本项目设置 ---
cmake_minimum_required(VERSION 3.15)
project(MYMQ CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 2. 查找系统库 ---

# 找到 PkgConfig (用于 zstd)
find_package(PkgConfig REQUIRED)

# 找到 ZLIB
find_package(ZLIB REQUIRED)

# 使用 pkg-config 查找 libzstd
pkg_check_modules(Zstd REQUIRED libzstd)
# --- 查找完毕 (我们已经移除了 find_package(Threads)) ---


# --- 3. 为您本地的 Nlohmann JSON 创建目标 ---
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
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
)

# 链接所有依赖
target_link_libraries(MYMQ
  PRIVATE
    nlohmann_json
    ZLIB::ZLIB
    ${Zstd_LIBRARIES}
    pthread           # <-- (已修改) 直接链接 pthread
    stdc++fs          # (链接 Filesystem 库)
    tbb
    uuid
)

# --- 6. 创建可执行文件 ---
add_executable(MyMQ_App main.cpp)

# --- (新) 6a. 复制 config 目录到构建目录 ---
#
# 这会创建一个自定义目标 CopyConfig，它会把源目录的 config 文件夹
# 复制到构建目录 (可执行文件所在的地方)
#
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
