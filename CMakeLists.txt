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
    src/dtln_wrapper.cpp
    src/pvad_wrapper.cpp
    src/silero_wrapper.cpp
    src/dec_wrapper.cpp
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

# ============================================================
#  Resolve Absl DLLs at configure time (versioned filenames)
# ============================================================
file(GLOB ABSL_DLLS "${MSYS2_ROOT}/bin/libabsl_*.dll")
set(ABSL_COPY_COMMANDS "")
foreach(_absl ${ABSL_DLLS})
    list(APPEND ABSL_COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_absl}" "$<TARGET_FILE_DIR:aec_gui>")
endforeach()

# ============================================================
#  Copy every runtime DLL next to aec_gui.exe
#  Includes transitive deps that load dynamically at runtime
#  (ggml backends, ONNX Runtime, WebRTC, GLFW, Absl, OpenMP).
# ============================================================
add_custom_command(TARGET aec_gui POST_BUILD
    # Our own engine DLLs
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/libs/aec.dll
        $<TARGET_FILE_DIR:aec_gui>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${NKF_DLL}
        $<TARGET_FILE_DIR:aec_gui>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${LOCALVQE_BIN}/liblocalvqe.dll
        $<TARGET_FILE_DIR:aec_gui>

    # GGML runtime (all CPU backend variants)
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${LOCALVQE_BIN}
        $<TARGET_FILE_DIR:aec_gui>

    # MSYS2 runtime — direct deps
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/libwebrtc-audio-processing-1-3.dll
        $<TARGET_FILE_DIR:aec_gui>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/onnxruntime.dll
        $<TARGET_FILE_DIR:aec_gui>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/glfw3.dll
        $<TARGET_FILE_DIR:aec_gui>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/libwinpthread-1.dll
        $<TARGET_FILE_DIR:aec_gui>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/libgcc_s_seh-1.dll
        $<TARGET_FILE_DIR:aec_gui>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/libstdc++-6.dll
        $<TARGET_FILE_DIR:aec_gui>

    # MSYS2 runtime — transitive deps (load dynamically)
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/libgomp-1.dll
        $<TARGET_FILE_DIR:aec_gui>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/libonnx.dll
        $<TARGET_FILE_DIR:aec_gui>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/libprotobuf-lite.dll
        $<TARGET_FILE_DIR:aec_gui>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/libre2-11.dll
        $<TARGET_FILE_DIR:aec_gui>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/libutf8_validity.dll
        $<TARGET_FILE_DIR:aec_gui>

    # Absl runtime (copied individually — list built above)
    ${ABSL_COPY_COMMANDS}

    # Models
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

# ============================================================
#  DTLN-AEC models (optional — engine reports "Failed to load
#  DTLN model" if absent). Guarded so a fresh clone without the
#  model files still configures and builds.
# ============================================================
foreach(_dtln
    models/dtln_aec_512_1.tflite
    models/dtln_aec_512_2.tflite
    models/dtln_aec_512_1.onnx
    models/dtln_aec_512_2.onnx)
    if(EXISTS "${CMAKE_SOURCE_DIR}/${_dtln}")
        add_custom_command(TARGET aec_gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/${_dtln}"
                "$<TARGET_FILE_DIR:aec_gui>/${_dtln}"
            COMMENT "Copying ${_dtln}")
    endif()
endforeach()
# ============================================================
#  Silero VAD model (optional — voice gate reports "model not found"
#  until vendored). Guarded so a fresh clone without it still builds.
#  Download (~2.2 MB, MIT), sha256-verified v6.2.1:
#    https://github.com/snakers4/silero-vad/blob/master/src/silero_vad/data/silero_vad.onnx
# ============================================================
if(EXISTS "${CMAKE_SOURCE_DIR}/models/silero_vad.onnx")
    add_custom_command(TARGET aec_gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/models/silero_vad.onnx"
            "$<TARGET_FILE_DIR:aec_gui>/models/silero_vad.onnx"
        COMMENT "Copying models/silero_vad.onnx")
endif()
# ============================================================
#  DEC baseline residual-echo cleanup (optional, experimental —
#  checkbox reports "model missing" until vendored). Microsoft
#  AEC-Challenge ICASSP 2022 baseline (MIT, ~5.2 MB).
#  Guarded so a fresh clone without it still builds.
# ============================================================
if(EXISTS "${CMAKE_SOURCE_DIR}/models/dec-baseline-icassp2022.onnx")
    add_custom_command(TARGET aec_gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/models/dec-baseline-icassp2022.onnx"
            "$<TARGET_FILE_DIR:aec_gui>/models/dec-baseline-icassp2022.onnx"
        COMMENT "Copying models/dec-baseline-icassp2022.onnx")
endif()
# ============================================================
#  ECAPA speaker embedding for the owner-only voice gate
#  (optional — feature reports "model missing" until vendored).
#  vedk00/ecapa-voxceleb-speaker-embedding-onnx (Apache-2.0).
#  Guarded so a fresh clone without it still builds.
# ============================================================
if(EXISTS "${CMAKE_SOURCE_DIR}/models/ecapa-speaker-v1.onnx")
    add_custom_command(TARGET aec_gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/models/ecapa-speaker-v1.onnx"
            "$<TARGET_FILE_DIR:aec_gui>/models/ecapa-speaker-v1.onnx"
        COMMENT "Copying models/ecapa-speaker-v1.onnx")
endif()
# TFLite runtime (optional, loaded at runtime by dtln_wrapper)
foreach(_tflite_dll
    "${CMAKE_SOURCE_DIR}/libs/tensorflowlite_c.dll"
    "${CMAKE_SOURCE_DIR}/libs/libtensorflowlite_c.dll")
    if(EXISTS "${_tflite_dll}")
        add_custom_command(TARGET aec_gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_tflite_dll}"
                "$<TARGET_FILE_DIR:aec_gui>")
    endif()
endforeach()