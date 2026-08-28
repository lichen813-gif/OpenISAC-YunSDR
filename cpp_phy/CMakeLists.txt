cmake_minimum_required(VERSION 3.15)
project(OpenISACCppPhy VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OPENISAC_ENABLE_AVX2 "Enable AVX2 for optimized targets" OFF)
option(OPENISAC_ENABLE_CUDA "Build the optional CUDA PHY backend" OFF)
option(OPENISAC_PHY_LIBRARY_ONLY
    "Build only the reusable openisac_phy library" OFF)
set(
    OPENISAC_GOLDEN_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors"
    CACHE PATH
    "Directory containing the committed Python-generated formal-frame vectors"
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
    src/mimo_nxn_link.cpp
    src/ofdm.cpp
    src/preamble_sync.cpp
    src/rank4_formal_link.cpp
    src/rank4_time_link.cpp
    src/rank4_time_pipeline.cpp
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

if(OPENISAC_PHY_LIBRARY_ONLY)
    return()
endif()

enable_testing()

if(OPENISAC_ENABLE_CUDA)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    add_library(openisac_phy_cuda STATIC
        cuda/cuda_backend.cpp
        cuda/cuda_fdm_mimo.cu
        cuda/cuda_mimo.cu
        cuda/cuda_ofdm.cu
    )
    target_include_directories(openisac_phy_cuda PUBLIC include)
    target_link_libraries(
        openisac_phy_cuda PUBLIC CUDA::cudart CUDA::cufft)
    set_target_properties(openisac_phy_cuda PROPERTIES
        CUDA_STANDARD 17
        CUDA_STANDARD_REQUIRED ON
        CUDA_EXTENSIONS OFF)
    add_executable(openisac_phy_cuda_tests tests/test_cuda_ofdm.cpp)
    target_link_libraries(
        openisac_phy_cuda_tests PRIVATE openisac_phy openisac_phy_cuda)
    add_test(NAME openisac_phy_cuda_ofdm COMMAND openisac_phy_cuda_tests)
    add_executable(openisac_phy_cuda_mimo_tests tests/test_cuda_mimo.cpp)
    target_link_libraries(
        openisac_phy_cuda_mimo_tests PRIVATE openisac_phy openisac_phy_cuda)
    add_test(NAME openisac_phy_cuda_mimo COMMAND openisac_phy_cuda_mimo_tests)
endif()

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
    openisac_phy_4x4_time_pipeline_benchmark
    benchmarks/bench_rank4_time_pipeline.cpp)
target_link_libraries(
    openisac_phy_4x4_time_pipeline_benchmark PRIVATE openisac_phy)
target_compile_definitions(
    openisac_phy_4x4_time_pipeline_benchmark PRIVATE
    OPENISAC_MATRIX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
if(MSVC)
    target_compile_options(
        openisac_phy_4x4_time_pipeline_benchmark PRIVATE
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

add_executable(
    openisac_phy_4x4_diagnostics
    tools/mimo4x4_diagnostics.cpp)
target_link_libraries(openisac_phy_4x4_diagnostics PRIVATE openisac_phy)
if(MSVC)
    target_compile_options(openisac_phy_4x4_diagnostics PRIVATE
        /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8)
endif()

add_executable(
    openisac_phy_nxn_diagnostics
    tools/mimo4x4_diagnostics.cpp)
target_link_libraries(openisac_phy_nxn_diagnostics PRIVATE openisac_phy)
if(MSVC)
    target_compile_options(openisac_phy_nxn_diagnostics PRIVATE
        /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8)
else()
    target_compile_options(openisac_phy_nxn_diagnostics PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion)
endif()
add_test(
    NAME openisac_nxn_8x8_algorithm_compatibility
    COMMAND openisac_phy_nxn_diagnostics --streams 8 --frames 2
            --modulation 64QAM --snr 50 --tx-correlation 0.02
            --rx-correlation 0.02 --flat-channel --check)

add_executable(
    openisac_phy_4x4_formal_diagnostics
    tools/rank4_formal_diagnostics.cpp)
target_link_libraries(openisac_phy_4x4_formal_diagnostics PRIVATE openisac_phy)
target_compile_definitions(openisac_phy_4x4_formal_diagnostics PRIVATE
    OPENISAC_MATRIX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
if(MSVC)
    target_compile_options(openisac_phy_4x4_formal_diagnostics PRIVATE
        /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8)
endif()

add_executable(
    openisac_phy_4x4_time_diagnostics
    tools/rank4_time_diagnostics.cpp)
target_link_libraries(openisac_phy_4x4_time_diagnostics PRIVATE openisac_phy)
target_compile_definitions(openisac_phy_4x4_time_diagnostics PRIVATE
    OPENISAC_MATRIX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
if(MSVC)
    target_compile_options(openisac_phy_4x4_time_diagnostics PRIVATE
        /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8)
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

find_package(Threads REQUIRED)
add_executable(openisac_phy_video_bridge tools/video_channel_bridge.cpp)
target_link_libraries(
    openisac_phy_video_bridge PRIVATE openisac_phy Threads::Threads)
if(WIN32)
    target_link_libraries(openisac_phy_video_bridge PRIVATE ws2_32)
endif()
if(OPENISAC_ENABLE_CUDA)
    target_link_libraries(openisac_phy_video_bridge PRIVATE openisac_phy_cuda)
    target_compile_definitions(
        openisac_phy_video_bridge PRIVATE OPENISAC_HAS_CUDA=1)
endif()
target_compile_definitions(openisac_phy_video_bridge PRIVATE
    OPENISAC_MATRIX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
if(MSVC)
    target_compile_options(openisac_phy_video_bridge PRIVATE
        /W4 /permissive- /EHsc /fp:precise /Zc:__cplusplus /utf-8)
else()
    target_compile_options(openisac_phy_video_bridge PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion)
endif()
add_test(
        NAME openisac_video_siso_compatibility
        COMMAND openisac_phy_video_bridge --self-test 5 --rank 1
                --tx-ports 1 --rx-ports 1 --mimo-mode spatial
                --modulation 64QAM --snr 45 --sensing-coherent 0)
add_test(
        NAME openisac_video_siso_dmrs_compatibility
        COMMAND openisac_phy_video_bridge --self-test 5 --rank 1
                --tx-ports 1 --rx-ports 1 --mimo-mode spatial
                --modulation 64QAM --pilot-mode nr-dmrs --snr 45
                --sensing-coherent 0)
add_test(
        NAME openisac_video_stbc_compatibility
        COMMAND openisac_phy_video_bridge --self-test 5 --rank 1
                --mimo-mode stbc --modulation 64QAM --snr 45
                --sensing-coherent 0)
add_test(
        NAME openisac_video_stbc_dmrs_compatibility
        COMMAND openisac_phy_video_bridge --self-test 5 --rank 1
                --mimo-mode stbc --modulation 64QAM --pilot-mode nr-dmrs
                --snr 45 --sensing-coherent 0)
add_test(
        NAME openisac_video_rank2_compatibility
        COMMAND openisac_phy_video_bridge --self-test 5 --rank 2
                --modulation 64QAM --snr 45 --sensing-coherent 0)
add_test(
        NAME openisac_video_4port_rank2_compatibility
        COMMAND openisac_phy_video_bridge --self-test 5 --rank 2
                --tx-ports 4 --rx-ports 4 --modulation 64QAM
                --snr 45 --sensing-coherent 0)
add_test(
        NAME openisac_video_4port_rank2_dmrs_compatibility
        COMMAND openisac_phy_video_bridge --self-test 5 --rank 2
                --tx-ports 4 --rx-ports 4 --modulation 64QAM
                --pilot-mode nr-dmrs --snr 45 --sensing-coherent 0)
add_test(
        NAME openisac_video_rank4_compatibility
        COMMAND openisac_phy_video_bridge --self-test 10 --rank 4
                --modulation 64QAM --workers 12 --snr 45
                --sensing-coherent 0)

if(OPENISAC_ENABLE_CUDA)
    function(add_openisac_cuda_video_test test_name)
        add_test(
            NAME ${test_name}
            COMMAND openisac_phy_video_bridge --self-test 3
                    --modulation 64QAM --snr 45 --sensing-coherent 0
                    --backend cuda ${ARGN})
    endfunction()
    foreach(pilot fdm nr-dmrs)
        add_openisac_cuda_video_test(
            openisac_video_cuda_siso_${pilot}
            --rank 1 --tx-ports 1 --rx-ports 1 --mimo-mode spatial
            --pilot-mode ${pilot})
        add_openisac_cuda_video_test(
            openisac_video_cuda_mimo2_${pilot}
            --rank 2 --tx-ports 2 --rx-ports 2 --mimo-mode spatial
            --pilot-mode ${pilot})
        add_openisac_cuda_video_test(
            openisac_video_cuda_stbc_${pilot}
            --rank 1 --tx-ports 2 --rx-ports 2 --mimo-mode stbc
            --pilot-mode ${pilot})
        add_openisac_cuda_video_test(
            openisac_video_cuda_rank2_4port_${pilot}
            --rank 2 --tx-ports 4 --rx-ports 4 --mimo-mode spatial
            --pilot-mode ${pilot})
        add_openisac_cuda_video_test(
            openisac_video_cuda_rank4_${pilot}
            --rank 4 --tx-ports 4 --rx-ports 4 --mimo-mode spatial
            --pilot-mode ${pilot})
    endforeach()
endif()
