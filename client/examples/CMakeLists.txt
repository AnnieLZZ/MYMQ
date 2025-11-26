# 1. 第一个程序：主测试程序 (基于 main.cpp)
add_executable(mymq_app
    main.cpp
    test_common_header.cpp  # 公共实现文件
    test_common_header.h    # 头文件(可选，方便IDE展示)
)
# 链接主库
target_link_libraries(mymq_app PRIVATE mymq_lib)

# 2. 第二个程序：Seek功能测试程序 (基于 example_test_seek.cpp)
add_executable(mymq_seek_test
    example_test_seek.cpp
    test_common_header.cpp  # 这里也要加入公共实现文件！
    test_common_header.h
)
# 链接主库
target_link_libraries(mymq_seek_test PRIVATE mymq_lib)

# 为 mymq_app 复制配置
add_custom_command(TARGET mymq_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/config
    $<TARGET_FILE_DIR:mymq_app>/config
)

# 为 mymq_seek_test 复制配置 (如果它也需要)
add_custom_command(TARGET mymq_seek_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/config
    $<TARGET_FILE_DIR:mymq_seek_test>/config
)
