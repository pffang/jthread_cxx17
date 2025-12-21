cmake_minimum_required(VERSION 3.10)
project(jthread_cxx17)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer for data race detection" ON)

if(ENABLE_THREAD_SANITIZER AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
    message(STATUS "ThreadSanitizer enabled")
endif()

include_directories(include)

add_subdirectory(test)
