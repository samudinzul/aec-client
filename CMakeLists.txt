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
    src/hush_wrapper.cpp
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
#  dtln_smoke / aec3_smoke — offline harnesses for the desktop
#  DTLN-AEC and WebRTC AEC3 output paths (engine-only, no NS).
#  Mirror tools/nkf_smoke.cpp pump-for-pump (160-sample frames);
#  compare against the web side with
#  `python -m web.test_parity --run mic ref <this out> --engine ...`.
#  Console apps (no window). dtln_smoke loads tensorflowlite_c.dll
#  at runtime (same as the GUI — staged next to the exe by the
#  aec_gui rules below when present); aec3_smoke links the MSYS2
#  WebRTC import library like aec_gui does.
#  Build: cmake --build build --target dtln_smoke aec3_smoke
#  Run:   build/dtln_smoke.exe mic.wav ref.wav out.wav
#         build/aec3_smoke.exe mic.wav ref.wav out.wav
# ============================================================
add_executable(dtln_smoke
    tools/dtln_smoke.cpp
    src/dtln_wrapper.cpp
)

target_include_directories(dtln_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${NKF_DIR}
    ${MSYS2_ROOT}/include/onnxruntime
)

target_link_libraries(dtln_smoke PRIVATE onnxruntime)

if(WIN32)
    target_link_libraries(dtln_smoke PRIVATE ole32 uuid winmm)
endif()

add_custom_command(TARGET dtln_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/libs/onnxruntime.dll
        $<TARGET_FILE_DIR:dtln_smoke>)

add_executable(aec3_smoke
    tools/aec3_smoke.cpp
    src/aec3_wrapper.cpp
)

target_include_directories(aec3_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${MSYS2_ROOT}/include/webrtc-audio-processing-1
)

target_link_libraries(aec3_smoke PRIVATE
    ${MSYS2_ROOT}/lib/libwebrtc-audio-processing-1.dll.a
)

if(WIN32)
    target_link_libraries(aec3_smoke PRIVATE ole32 uuid winmm)
endif()

add_custom_command(TARGET aec3_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/libwebrtc-audio-processing-1-3.dll
        $<TARGET_FILE_DIR:aec3_smoke>)

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
# Hush NS backend (optional — the radio fail-closes at Start without
# these, and the release script stages them when present).
if(EXISTS "${CMAKE_SOURCE_DIR}/libs/weya_nc.dll")
    add_custom_command(TARGET aec_gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/libs/weya_nc.dll"
            "$<TARGET_FILE_DIR:aec_gui>"
        COMMENT "Copying Hush runtime (weya_nc.dll)")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/models/hush_16k_onnx.tar.gz")
    add_custom_command(TARGET aec_gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/models/hush_16k_onnx.tar.gz"
            "$<TARGET_FILE_DIR:aec_gui>/models/hush_16k_onnx.tar.gz"
        COMMENT "Copying Hush model bundle")
endif()

# ============================================================
#  aec_exp — the experimental fork (exp/main.cpp, "AEC Exp").
#  Same app shape as aec_gui, PLUS the experimental engines the
#  official build never sees: the LocalVQE v1.4-AEC engine profile
#  and the GTCRN NS backend. Separate exe, separate window title /
#  mutex / config file (see the header comment in exp/main.cpp), so
#  the two can be installed side by side; shared bugfixes must be
#  mirrored into src/main.cpp by hand.
#  Own bundle staging below (exp/models/* + GTCRN); the release
#  scripts never ship any of it.
#  Build: cmake --build build --target aec_exp
#  Run:   build/aec_exp.exe
# ============================================================
add_executable(aec_exp
    exp/main.cpp
    src/aec3_wrapper.cpp
    src/nkf_wrapper.cpp
    src/dtln_wrapper.cpp
    src/dtln_ns_wrapper.cpp
    src/hush_wrapper.cpp
    src/gtcrn_wrapper.cpp
    src/localvqe_wrapper.cpp
    third_party/localvqe/localvqe_aec_frontend.cpp
    exp/version_exp.rc
    exp/aec_exp.manifest
    ${NKF_DIR}/NKFImpl.cpp
)

target_include_directories(aec_exp PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/localvqe
    ${NKF_DIR}
    ${MSYS2_ROOT}/include/onnxruntime
    ${MSYS2_ROOT}/include/webrtc-audio-processing-1
)

target_link_libraries(aec_exp PRIVATE
    imgui
    glfw
    OpenGL::GL
    ${MSYS2_ROOT}/lib/libwebrtc-audio-processing-1.dll.a
    onnxruntime
)

if(WIN32)
    set_target_properties(aec_exp PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(aec_exp PRIVATE imm32 ole32 uuid winmm shell32)
    set_target_properties(aec_exp PROPERTIES
        CMAKE_EXE_LINKER_FLAGS "-s -mwindows")
endif()

add_custom_command(TARGET aec_exp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/libwebrtc-audio-processing-1-3.dll
        $<TARGET_FILE_DIR:aec_exp>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/libs/onnxruntime.dll
        $<TARGET_FILE_DIR:aec_exp>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/glfw3.dll
        $<TARGET_FILE_DIR:aec_exp>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/libwinpthread-1.dll
        $<TARGET_FILE_DIR:aec_exp>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/libgcc_s_seh-1.dll
        $<TARGET_FILE_DIR:aec_exp>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSYS2_ROOT}/bin/libstdc++-6.dll
        $<TARGET_FILE_DIR:aec_exp>
    ${VC_COPY_COMMANDS}
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:aec_exp>/models
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/models/nkf.onnx
        $<TARGET_FILE_DIR:aec_exp>/models/nkf.onnx
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/models/dtln_ns_128_1.tflite
        $<TARGET_FILE_DIR:aec_exp>/models/dtln_ns_128_1.tflite
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/models/dtln_ns_128_2.tflite
        $<TARGET_FILE_DIR:aec_exp>/models/dtln_ns_128_2.tflite
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:aec_exp>/exp/models
    COMMENT "Copying runtime DLLs and models (exp)"
)
# DTLN-AEC + TFLite runtime + Hush for the exp build (same
# optional-file rules as the official build above).
foreach(_exp_models
    models/dtln_aec_128_1.tflite
    models/dtln_aec_128_2.tflite
    models/dtln_aec_128_1.onnx
    models/dtln_aec_128_2.onnx
    models/hush_16k_onnx.tar.gz)
    if(EXISTS "${CMAKE_SOURCE_DIR}/${_exp_models}")
        add_custom_command(TARGET aec_exp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/${_exp_models}"
                "$<TARGET_FILE_DIR:aec_exp>/${_exp_models}"
            COMMENT "Copying ${_exp_models} (exp)")
    endif()
endforeach()
foreach(_exp_dll
    "${CMAKE_SOURCE_DIR}/libs/tensorflowlite_c.dll"
    "${CMAKE_SOURCE_DIR}/libs/libtensorflowlite_c.dll"
    "${CMAKE_SOURCE_DIR}/libs/weya_nc.dll")
    if(EXISTS "${_exp_dll}")
        add_custom_command(TARGET aec_exp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_exp_dll}"
                "$<TARGET_FILE_DIR:aec_exp>"
            COMMENT "Copying ${_exp_dll} (exp)")
    endif()
endforeach()
# GTCRN NS backend -> build/models/ (fork loads
# "models/gtcrn_simple.onnx"); LocalVQE v1.4-AEC engine ->
# build/exp/models/ (fork loads "exp/models/LocalVQE*"). All
# guarded: without the downloads the engines fail closed at Start
# with a pointer at exp/get_models.sh.
if(EXISTS "${CMAKE_SOURCE_DIR}/exp/models/gtcrn_simple.onnx")
    add_custom_command(TARGET aec_exp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/exp/models/gtcrn_simple.onnx"
            "$<TARGET_FILE_DIR:aec_exp>/models/gtcrn_simple.onnx"
        COMMENT "Copying GTCRN model (exp)")
endif()
foreach(_lv exp/models/LocalVQEAECResidualMask.onnx
             exp/models/LocalVQEAECFrontend.json
             exp/models/LocalVQEAECConfig.json)
    if(EXISTS "${CMAKE_SOURCE_DIR}/${_lv}")
        add_custom_command(TARGET aec_exp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/${_lv}"
                "$<TARGET_FILE_DIR:aec_exp>/${_lv}"
            COMMENT "Copying ${_lv} (exp)")
    endif()
endforeach()

# ============================================================
#  exp/ — GTCRN + LocalVQE v1.4 experiment harnesses.
#  Desktop-only, EXCLUDE_FROM_ALL: the default build is untouched.
#  Build explicitly:
#    cmake --build build --target gtcrn_smoke localvqe_smoke
#  Never shipped: the release scripts do not stage exp/.
# ============================================================
add_executable(gtcrn_smoke EXCLUDE_FROM_ALL
    exp/gtcrn/gtcrn_smoke.cpp
    src/gtcrn_wrapper.cpp
)

target_include_directories(gtcrn_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${MSYS2_ROOT}/include/onnxruntime
)

target_link_libraries(gtcrn_smoke PRIVATE onnxruntime)

if(WIN32)
    target_link_libraries(gtcrn_smoke PRIVATE ole32 uuid winmm)
endif()

add_custom_command(TARGET gtcrn_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/libs/onnxruntime.dll
        $<TARGET_FILE_DIR:gtcrn_smoke>)

add_executable(localvqe_smoke EXCLUDE_FROM_ALL
    exp/localvqe/localvqe_smoke.cpp
    src/localvqe_wrapper.cpp
    third_party/localvqe/localvqe_aec_frontend.cpp
)

target_include_directories(localvqe_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/localvqe
    ${MSYS2_ROOT}/include/onnxruntime
)

target_link_libraries(localvqe_smoke PRIVATE onnxruntime)

if(WIN32)
    target_link_libraries(localvqe_smoke PRIVATE ole32 uuid winmm)
endif()

add_custom_command(TARGET localvqe_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/libs/onnxruntime.dll
        $<TARGET_FILE_DIR:localvqe_smoke>)

# LocalVQE bundle (downloaded by exp/get_models.sh) next to the smoke exe.
add_custom_command(TARGET localvqe_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:localvqe_smoke>/models
)
foreach(_lv exp/models/LocalVQEAECResidualMask.onnx
             exp/models/LocalVQEAECFrontend.json
             exp/models/LocalVQEAECConfig.json
             exp/models/config.json)
    if(EXISTS "${CMAKE_SOURCE_DIR}/${_lv}")
        add_custom_command(TARGET localvqe_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/${_lv}"
                "$<TARGET_FILE_DIR:localvqe_smoke>/models/")
    endif()
endforeach()

# Offline DSP check for the high-band splice resampler pair
# (exp/resampler.h). No models, no audio device, no ONNX Runtime:
# pure C++, exits non-zero on failure.
# Build: cmake --build build --target resampler_test
# Run:   build/resampler_test.exe
add_executable(resampler_test EXCLUDE_FROM_ALL
    exp/resampler_test.cpp
)

target_include_directories(resampler_test PRIVATE
    ${CMAKE_SOURCE_DIR}/exp
)


