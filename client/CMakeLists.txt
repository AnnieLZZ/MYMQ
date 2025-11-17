cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
project(MYMQ VERSION 1.0 LANGUAGES CXX)


set(TP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty")

# 定义源代码文件 (位于 src 目录)
set(SRC_FILES
    src/MYMQ_C.cpp
    src/MYMQ_Client.cpp
    # 如果有其他 .cpp 实现文件，继续加在这里
    # 例如: src/Communication.cpp
)

# 定义内部头文件 (位于 src 目录，虽然不编译，但列出来方便IDE展示)
set(INTERNAL_HEADERS
    src/MYMQ_Client.h
    src/MYMQ_innercodes.h
    # ... 其他 src 下的 .h 文件
)

# =============================================================
# 2. 创建库目标 (Static Library)
# =============================================================
# 生成静态库 mymq_lib.lib (Windows) 或 libmymq_lib.a (Linux)
add_library(mymq_lib STATIC 
    ${SRC_FILES} 
    ${INTERNAL_HEADERS}
)

# =============================================================
# 3. 配置头文件路径 (Include Directories)
# =============================================================
target_include_directories(mymq_lib
    # PUBLIC: 用户需要看到的 (include/MYMQ)
    PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}/include

    # PRIVATE: 只有你的源代码需要看到的 (src + 第三方库)
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        
        # --- 第三方库头文件路径 ---
        "${TP_DIR}/tbb/include"
        "${TP_DIR}/tbb/include/oneapi"                # TBB
        "${TP_DIR}/zlib/include"                # Zlib (包含 zlib.h, zconf.h)
        "${TP_DIR}/zstd/include"                # Zstd (包含 zstd.h)
        "${TP_DIR}/readerwriterqueue/include"   # ReaderWriterQueue
        "${TP_DIR}/nlohmann"                    # JSON (假设 json.hpp 在这里)
)

# =============================================================
# 4. 链接第三方库 (Link Libraries)
# =============================================================
target_link_libraries(mymq_lib 
    PRIVATE
    # --- Zlib (静态链接) ---
    "${TP_DIR}/zlib/lib/libzlibstatic.a"
    
    # --- Zstd (静态链接) ---
    "${TP_DIR}/zstd/lib/libzstd.a"
    
    # --- TBB (动态链接导入库) ---
    # 注意：TBB 是动态库，这里链接的是 .dll.a 路标文件
    "${TP_DIR}/tbb/lib/libtbb12.dll.a"
    
    # --- 系统库 (Windows 网络库) ---
    ws2_32
    ssp
)

# ReaderWriterQueue 和 Nlohmann Json 是 Header-only，不需要链接库文件

# =============================================================
# 5. 添加测试/示例程序 (可选)
# =============================================================
# 检查 examples 目录是否存在，存在则构建
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples")
    add_subdirectory(examples)
endif()
