
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 添加可执行文件
add_executable(jthread17 jthread17.cpp)

# 可选：设置编译选项
if(MSVC)
    # Visual Studio 编译器选项
    target_compile_options(jthread17 PRIVATE /W4)
else()
    # GCC/Clang 编译器选项
    target_compile_options(jthread17 PRIVATE -Wall -Wextra -Wpedantic)
endif()