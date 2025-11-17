add_executable(mymq_app main.cpp)
target_link_libraries(mymq_app PRIVATE mymq_lib)

# 1. 定义源文件夹 (源码根目录下的 config 文件夹)
set(CONFIG_SRC_DIR "${CMAKE_SOURCE_DIR}/config")

# 2. 定义目标文件夹 (构建目录下的 run/config)
set(CONFIG_DST_DIR "${CMAKE_CURRENT_BINARY_DIR}/run/config")

# 3. 添加构建后命令
add_custom_command(TARGET mymq_app POST_BUILD
    # 使用 copy_directory 命令，把源文件夹里的东西全部倒进目标文件夹
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CONFIG_SRC_DIR}
            ${CONFIG_DST_DIR}
            
    COMMENT "Copying all config files to ${CONFIG_DST_DIR}"
)