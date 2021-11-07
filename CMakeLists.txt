cmake_minimum_required(VERSION 3.0.0)
project(co_uring_demo VERSION 0.1.0)

include(CTest)
enable_testing()

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
set(CMAKE_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} -v -fsanitize=thread -stdlib=libc++ -std=c++20")

add_executable(io_uring_coroutine_echo_demo io_uring_coroutine_echo_demo.cpp)

target_link_libraries(io_uring_coroutine_echo_demo uring)
