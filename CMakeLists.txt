cmake_minimum_required(VERSION 3.25)
project(FSCameraStudio VERSION 0.1.0 LANGUAGES CXX)

# Native build: the portable libraries and their tests, run on the development PC (design 2.0 §15 allows host-side
# tests of pure code as development tools; nothing native ships). The shipping WASM module and MSFS package are
# built by tools/build.ps1 with the MSFS 2024 SDK toolchain.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_library(fscs_json INTERFACE)
target_include_directories(fscs_json SYSTEM INTERFACE "${PROJECT_SOURCE_DIR}/third_party")

add_subdirectory(src/CameraCore)
add_subdirectory(src/Profiles)
add_subdirectory(src/Protocol)
add_subdirectory(src/WasmModule)

enable_testing()
add_subdirectory(tests)
