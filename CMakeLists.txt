cmake_minimum_required(VERSION 3.21)
project(renderer C)

# The renderer was being built with -g and nothing else: no optimisation at all,
# because CMAKE_BUILD_TYPE was empty so none of CMake's *_RELEASE flags applied.
# That alone was costing roughly half the frame time (measured: 237.7 -> 126.6 ms
# on woman_seated_v12.glb, and 5.85 -> 2.62 s to decode its textures).
#
# Default to RelWithDebInfo rather than Release so a stack trace still means
# something -- this is a project for reading in a debugger.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()


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
    src/utils.c
    src/transform.c
    src/rasterization.c
    src/mesh.c
    src/mesh_ply.c
    src/mesh_obj.c
    src/mesh_gltf.c
    src/json.c
    src/image.c
    src/png.c
    src/jpeg.c
    src/inflate.c
)

target_include_directories(renderer PRIVATE include)


target_link_libraries(renderer PRIVATE SDL3::SDL3 m)

target_compile_options(renderer PRIVATE -g -Wall -Wextra
    -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function)

# NOT -ffast-math. The mesh loaders lean on isfinite() to reject NaN vertices
# (mesh.c bbox_fold, fit_params, mesh_wrap_uv); -ffast-math tells the compiler
# NaNs cannot happen and those guards fold away to true.