cmake_minimum_required(VERSION 3.21)
project(renderer C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(SDL3 REQUIRED)

add_executable(renderer
    src/main.c
    src/win_i_o.c
    src/renderer.c
    src/projection.c
    src/wireframe.c
    src/interpolation.c
)

target_include_directories(renderer PRIVATE include)

target_link_libraries(renderer PRIVATE SDL3::SDL3 m)