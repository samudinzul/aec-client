cmake_minimum_required(VERSION 3.20)
project(NKF_AEC_C LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build as a shared library (DLL)
add_library(nkf_aec SHARED
    NKFImpl.cpp
)

# Headers are in the same directory
target_include_directories(nkf_aec PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Find and link ONNX Runtime installed via MSYS2
find_package(onnxruntime REQUIRED)
target_link_libraries(nkf_aec PRIVATE onnxruntime::onnxruntime)