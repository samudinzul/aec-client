cmake_minimum_required(VERSION 3.20)
project(aec VERSION 1.10.1 LANGUAGES CXX)

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

add_executable(aec_gui
    src/main.cpp
    src/aec3_wrapper.cpp
    src/nkf_wrapper.cpp
    src/dtln_wrapper.cpp
    src/dtln_ns_wrapper.cpp
    version.rc
    aec_gui.manifest
    # NKF engine compiled straight in — a prebuilt libnkf_aec.dll went
    # stale against NKFImpl.h and crashed the app on first ProcessBlock
    # (member offsets shifted). No separate DLL build anymore.
    ${NKF_DIR}/NKFImpl.cpp
)

target_include_directories(aec_gui PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${NKF_DIR}
    ${MSYS2_ROOT}/include/onnxruntime
    ${MSYS2_ROOT}/include/webrtc-audio-processing-1
)

target_link_libraries(aec_gui PRIVATE
    imgui
    glfw
    OpenGL::GL
    ${MSYS2_ROOT}/lib/libwebrtc-audio-processing-1.dll.a
    onnxruntime
)

if(WIN32)
    set_target_properties(aec_gui PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(aec_gui PRIVATE imm32 ole32 uuid winmm shell32)
    # Strip debug symbols (smaller exe, no .pdb) and target the
    # Windows GUI subsystem: this app has a window and system tray,
    # so it should not attach a console window on launch.
    set_target_properties(aec_gui PROPERTIES
        CMAKE_EXE_LINKER_FLAGS "-s -mwindows")
endif()

# ============================================================
#  nkf_smoke — offline harness for the desktop NKF output path.
#  Runs WAV (or synthetic) mic/ref pairs through the exact pump the
#  GUI runs (NkfProcess -> DTLN-NS) and prints per-second
#  engine telemetry (TDC lock, guard resets, backstop trim dB).
#  Console app (no window), shares the vendored ONNX Runtime.
#  Build: cmake --build build --target nkf_smoke
#  Run:   build/nkf_smoke.exe mic.wav ref.wav out.wav
# ============================================================
add_executable(nkf_smoke
    tools/nkf_smoke.cpp
    src/nkf_wrapper.cpp
    src/dtln_ns_wrapper.cpp
    ${NKF_DIR}/NKFImpl.cpp
)

target_include_directories(nkf_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${NKF_DIR}
    ${MSYS2_ROOT}/include/onnxruntime
)

target_link_libraries(nkf_smoke PRIVATE onnxruntime)

if(WIN32)
    target_link_libraries(nkf_smoke PRIVATE ole32 uuid winmm)
endif()

add_custom_command(TARGET nkf_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/libs/onnxruntime.dll
        $<TARGET_FILE_DIR:nkf_smoke>)

# ============================================================
#  ONNX Runtime — official Microsoft build (v1.29.0), vendored
#  uncompressed (~16 MB). Replaces the MSYS2 build (27.5 MB) and
#  its 97-DLL absl/onnx/protobuf/re2 dependency cluster.
#  Uncompressed on purpose: UPX-packing trips ML heuristics
#  (see SECURITY.md), so the raw vendor DLL ships instead.
#  Provenance: LICENSES/THIRD-PARTY.txt
# ============================================================
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/libs/onnxruntime.dll")
    message(FATAL_ERROR
        "libs/onnxruntime.dll missing — vendored Microsoft onnxruntime 1.29.0 (uncompressed)")
endif()

# ============================================================
#  VC++ 14 runtime — required by onnxruntime.dll (MSVC-built).
#  Copied app-local from the build machine's System32 so the app
#  starts on machines without the VC++ redistributable.
# ============================================================
if(DEFINED ENV{WINDIR})
    file(TO_CMAKE_PATH "$ENV{WINDIR}" _WIN_DIR)
else()
    set(_WIN_DIR "C:/Windows")
endif()
set(VC_COPY_COMMANDS "")
foreach(_vc msvcp140 msvcp140_1 vcruntime140 vcruntime140_1)
    if(EXISTS "${_WIN_DIR}/System32/${_vc}.dll")
        list(APPEND VC_COPY_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_WIN_DIR}/System32/${_vc}.dll" "$<TARGET_FILE_DIR:aec_gui>")
    else()
        message(WARNING "VC++ runtime not found: ${_WIN_DIR}/System32/${_vc}.dll "
            "(app may fail to start without it)")
    endif()
endforeach()

# ============================================================
#  Copy every runtime DLL next to aec_gui.exe
#  Includes transitive deps that load dynamically at runtime
#  (ONNX Runtime + VC++ CRT, WebRTC, GLFW).
# ============================================================
add_custom_command(TARGET aec_gui POST_BUILD
    # MSYS2 runtime — direct deps
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/libwebrtc-audio-processing-1-3.dll
        $<TARGET_FILE_DIR:aec_gui>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/libs/onnxruntime.dll
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

    # VC++ 14 runtime for onnxruntime.dll (built above)
    ${VC_COPY_COMMANDS}

    # Models
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:aec_gui>/models
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/models/nkf.onnx
        $<TARGET_FILE_DIR:aec_gui>/models/nkf.onnx
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/models/dtln_ns_128_1.tflite
        $<TARGET_FILE_DIR:aec_gui>/models/dtln_ns_128_1.tflite
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/models/dtln_ns_128_2.tflite
        $<TARGET_FILE_DIR:aec_gui>/models/dtln_ns_128_2.tflite

    COMMENT "Copying runtime DLLs and models"
)

# ============================================================
#  DTLN-AEC models (optional — engine reports "Failed to load
#  DTLN model" if absent). Guarded so a fresh clone without the
#  model files still configures and builds.
# ============================================================
foreach(_dtln
    models/dtln_aec_128_1.tflite
    models/dtln_aec_128_2.tflite
    models/dtln_aec_128_1.onnx
    models/dtln_aec_128_2.onnx)
    if(EXISTS "${CMAKE_SOURCE_DIR}/${_dtln}")
        add_custom_command(TARGET aec_gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/${_dtln}"
                "$<TARGET_FILE_DIR:aec_gui>/${_dtln}"
            COMMENT "Copying ${_dtln}")
    endif()
endforeach()
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