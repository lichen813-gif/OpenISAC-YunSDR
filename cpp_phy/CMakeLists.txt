cmake_minimum_required(VERSION 3.15)
project(OpenISACCppPhy VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OPENISAC_ENABLE_AVX2 "Enable AVX2 for optimized targets" OFF)
set(
    OPENISAC_GOLDEN_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../measurement/cpp_frame_vectors"
    CACHE PATH
    "Directory containing Python-generated formal-frame golden vectors"
)

add_library(openisac_phy STATIC
    src/capture_io.cpp
    src/channel_estimation.cpp
    src/crc16.cpp
    src/dynamic_frame.cpp
    src/dynamic_frame_pipeline.cpp
    src/dynamic_link.cpp
    src/dynamic_link_pipeline.cpp
    src/frame.cpp
    src/ldpc.cpp
    src/ldpc_frame_decoder.cpp
    src/ldpc_framing.cpp
    src/link_adaptation.cpp
    src/mimo2x2.cpp
    src/mimo_nxn.cpp
    src/ofdm.cpp
    src/preamble_sync.cpp
    src/sampling_offset.cpp
    src/sensing.cpp
    src/tdl_channel.cpp
)
target_include_directories(openisac_phy PUBLIC include)
target_compile_features(openisac_phy PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(openisac_phy PRIVATE
        /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8
    )
    if(OPENISAC_ENABLE_AVX2)
        target_compile_options(openisac_phy PRIVATE /arch:AVX2)
    endif()
else()
    target_compile_options(openisac_phy PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion
    )
    if(OPENISAC_ENABLE_AVX2)
        target_compile_options(openisac_phy PRIVATE -mavx2)
    endif()
endif()

enable_testing()
add_executable(openisac_phy_tests tests/test_main.cpp)
target_link_libraries(openisac_phy_tests PRIVATE openisac_phy)
target_compile_definitions(openisac_phy_tests PRIVATE
    OPENISAC_GOLDEN_DIR="${OPENISAC_GOLDEN_DIR}"
    OPENISAC_MATRIX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
if(MSVC)
    target_compile_options(openisac_phy_tests PRIVATE
        /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8
    )
endif()
add_test(NAME openisac_phy_tests COMMAND openisac_phy_tests)

add_executable(openisac_phy_bench benchmarks/bench_phy.cpp)
target_link_libraries(openisac_phy_bench PRIVATE openisac_phy)
target_compile_definitions(openisac_phy_bench PRIVATE
    OPENISAC_MATRIX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
if(MSVC)
    target_compile_options(openisac_phy_bench PRIVATE
        /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8
    )
endif()

add_executable(openisac_phy_realtime_benchmark benchmarks/bench_dynamic_link.cpp)
target_link_libraries(openisac_phy_realtime_benchmark PRIVATE openisac_phy)
target_compile_definitions(openisac_phy_realtime_benchmark PRIVATE
    OPENISAC_MATRIX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
if(MSVC)
    target_compile_options(openisac_phy_realtime_benchmark PRIVATE
        /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8
    )
endif()

add_executable(
    openisac_phy_pipeline_benchmark
    benchmarks/bench_dynamic_frame_pipeline.cpp)
target_link_libraries(openisac_phy_pipeline_benchmark PRIVATE openisac_phy)
target_compile_definitions(openisac_phy_pipeline_benchmark PRIVATE
    OPENISAC_MATRIX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
if(MSVC)
    target_compile_options(openisac_phy_pipeline_benchmark PRIVATE
        /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8)
endif()

add_executable(
    openisac_phy_full_pipeline_benchmark
    benchmarks/bench_dynamic_link_pipeline.cpp)
target_link_libraries(openisac_phy_full_pipeline_benchmark PRIVATE openisac_phy)
target_compile_definitions(openisac_phy_full_pipeline_benchmark PRIVATE
    OPENISAC_MATRIX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
if(MSVC)
    target_compile_options(openisac_phy_full_pipeline_benchmark PRIVATE
        /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8)
endif()

add_executable(
    openisac_phy_iq_pipeline_benchmark
    benchmarks/bench_dynamic_iq_pipeline.cpp)
target_link_libraries(openisac_phy_iq_pipeline_benchmark PRIVATE openisac_phy)
target_compile_definitions(openisac_phy_iq_pipeline_benchmark PRIVATE
    OPENISAC_MATRIX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
if(MSVC)
    target_compile_options(openisac_phy_iq_pipeline_benchmark PRIVATE
        /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8)
endif()

add_executable(
    openisac_phy_capture_io_benchmark
    benchmarks/bench_capture_io.cpp)
target_link_libraries(openisac_phy_capture_io_benchmark PRIVATE openisac_phy)
target_compile_definitions(openisac_phy_capture_io_benchmark PRIVATE
    OPENISAC_MATRIX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
if(MSVC)
    target_compile_options(openisac_phy_capture_io_benchmark PRIVATE
        /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8)
endif()

add_executable(
    openisac_phy_sensing_benchmark
    benchmarks/bench_sensing.cpp)
target_link_libraries(openisac_phy_sensing_benchmark PRIVATE openisac_phy)
target_compile_definitions(openisac_phy_sensing_benchmark PRIVATE
    OPENISAC_MATRIX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
if(MSVC)
    target_compile_options(openisac_phy_sensing_benchmark PRIVATE
        /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8)
endif()

add_executable(openisac_phy_diagnostics tools/export_diagnostics.cpp)
target_link_libraries(openisac_phy_diagnostics PRIVATE openisac_phy)
target_compile_definitions(openisac_phy_diagnostics PRIVATE
    OPENISAC_GOLDEN_DIR="${OPENISAC_GOLDEN_DIR}"
    OPENISAC_MATRIX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
if(MSVC)
    target_compile_options(openisac_phy_diagnostics PRIVATE
        /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8
    )
endif()

add_executable(openisac_phy_adaptive_regression tools/adaptive_tdl_regression.cpp)
target_link_libraries(openisac_phy_adaptive_regression PRIVATE openisac_phy)
target_compile_definitions(openisac_phy_adaptive_regression PRIVATE
    OPENISAC_MATRIX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
if(MSVC)
    target_compile_options(openisac_phy_adaptive_regression PRIVATE
        /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8
    )
endif()

add_executable(openisac_phy_replay_iq tools/replay_iq_capture.cpp)
target_link_libraries(openisac_phy_replay_iq PRIVATE openisac_phy)
target_compile_definitions(openisac_phy_replay_iq PRIVATE
    OPENISAC_MATRIX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
if(MSVC)
    target_compile_options(openisac_phy_replay_iq PRIVATE
        /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8)
endif()

if(WIN32)
    add_executable(openisac_phy_video_bridge tools/video_channel_bridge.cpp)
    target_link_libraries(openisac_phy_video_bridge PRIVATE openisac_phy ws2_32)
    target_compile_definitions(openisac_phy_video_bridge PRIVATE
        OPENISAC_MATRIX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
    )
    if(MSVC)
        target_compile_options(openisac_phy_video_bridge PRIVATE
            /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8)
    endif()
endif()
