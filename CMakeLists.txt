cmake_minimum_required(VERSION 3.20)
project(aec VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set(MSYS2_ROOT "C:/msys64/ucrt64")

find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

set(NKF_DIR ${CMAKE_SOURCE_DIR}/third_party/REAL_TIME_NKF_AEC/c)
set(NKF_DLL ${NKF_DIR}/build/libnkf_aec.dll)

set(LOCALVQE_DIR    ${CMAKE_SOURCE_DIR}/third_party/LocalVQE)
set(LOCALVQE_GGML   ${LOCALVQE_DIR}/ggml)
set(LOCALVQE_BIN    ${LOCALVQE_GGML}/build/bin)
set(LOCALVQE_IMPLIB ${LOCALVQE_GGML}/build/liblocalvqe.dll.a)

add_executable(aec_gui
    src/main.cpp
    src/aec3_wrapper.cpp
    src/nkf_wrapper.cpp
    src/localvqe_wrapper.cpp
)

target_include_directories(aec_gui PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${NKF_DIR}
    ${LOCALVQE_GGML}
    ${MSYS2_ROOT}/include/onnxruntime
    ${MSYS2_ROOT}/include/webrtc-audio-processing-1
)

target_link_libraries(aec_gui PRIVATE
    imgui
    glfw
    OpenGL::GL
    ${CMAKE_SOURCE_DIR}/libs/libaec.a
    ${NKF_DLL}
    ${LOCALVQE_IMPLIB}
    ${MSYS2_ROOT}/lib/libwebrtc-audio-processing-1.dll.a
    onnxruntime
)

if(WIN32)
    set_target_properties(aec_gui PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(aec_gui PRIVATE imm32 ole32 uuid winmm shell32)
endif()

add_custom_command(TARGET aec_gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/libs/aec.dll
        $<TARGET_FILE_DIR:aec_gui>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/libwebrtc-audio-processing-1-3.dll
        $<TARGET_FILE_DIR:aec_gui>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${NKF_DLL}
        $<TARGET_FILE_DIR:aec_gui>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/onnxruntime.dll
        $<TARGET_FILE_DIR:aec_gui>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${LOCALVQE_BIN}
        $<TARGET_FILE_DIR:aec_gui>
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:aec_gui>/models
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/models/nkf.onnx
        $<TARGET_FILE_DIR:aec_gui>/models/nkf.onnx
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/models/localvqe-v1.4-aec-200K-f32.gguf
        $<TARGET_FILE_DIR:aec_gui>/models/localvqe-v1.4-aec-200K-f32.gguf
    COMMENT "Copying runtime DLLs and models"
)
