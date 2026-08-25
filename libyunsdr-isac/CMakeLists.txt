cmake_minimum_required(VERSION 3.20)

project(libyunsdr_isac VERSION 0.1.0 LANGUAGES CXX)

option(LIBYUNSDR_ISAC_BUILD_TOOLS "Build command-line hardware tools" ON)
option(LIBYUNSDR_ISAC_BUILD_TESTS "Build baseline tests" ON)
option(LIBYUNSDR_ISAC_ENABLE_HARDWARE "Build the real libyunsdr backend" OFF)

set(
    LIBYUNSDR_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/out/y240-sdk-26-01-00.1"
    CACHE PATH
    "Staged YunSDR Y240 SDK root"
)

set(
    OPENISAC_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    CACHE PATH
    "Path to the OpenISAC source tree that provides RadioBackend.hpp"
)

if(NOT EXISTS "${OPENISAC_ROOT}/include/RadioBackend.hpp")
    message(FATAL_ERROR
        "OPENISAC_ROOT does not contain include/RadioBackend.hpp: ${OPENISAC_ROOT}")
endif()

if(NOT EXISTS "${OPENISAC_ROOT}/cpp_phy/CMakeLists.txt")
    message(FATAL_ERROR
        "OPENISAC_ROOT does not contain cpp_phy/CMakeLists.txt: ${OPENISAC_ROOT}")
endif()

set(OPENISAC_PHY_LIBRARY_ONLY ON CACHE BOOL
    "Build only the reusable OpenISAC PHY library" FORCE)
add_subdirectory(
    "${OPENISAC_ROOT}/cpp_phy"
    "${CMAKE_CURRENT_BINARY_DIR}/openisac_phy"
)

add_library(libyunsdr_isac_radio_contract INTERFACE)
target_include_directories(
    libyunsdr_isac_radio_contract
    INTERFACE
        "${OPENISAC_ROOT}/include"
)
target_compile_options(
    libyunsdr_isac_radio_contract
    INTERFACE
        "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>"
)

add_library(libyunsdr_isac_adapter STATIC
    src/backend_stub.cpp
    src/openisac_phy_codec.cpp
    src/transceiver.cpp
)
add_library(libyunsdr_isac::adapter ALIAS libyunsdr_isac_adapter)

target_compile_features(libyunsdr_isac_adapter PUBLIC cxx_std_17)
target_include_directories(
    libyunsdr_isac_adapter
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    libyunsdr_isac_adapter
    PUBLIC
        libyunsdr_isac_radio_contract
        openisac_phy
)
target_compile_definitions(
    libyunsdr_isac_adapter
    PRIVATE
        "LIBYUNSDR_ISAC_MATRIX_DIR=\"${OPENISAC_ROOT}\""
)

if(MSVC)
    target_compile_options(libyunsdr_isac_adapter PRIVATE /W4 /permissive-)
else()
    target_compile_options(libyunsdr_isac_adapter PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(LIBYUNSDR_ISAC_ENABLE_HARDWARE)
    set(LIBYUNSDR_INCLUDE_DIR "${LIBYUNSDR_SDK_ROOT}/include")
    set(LIBYUNSDR_IMPORT_LIBRARY "${LIBYUNSDR_SDK_ROOT}/lib/yunsdr_ss.lib")
    set(LIBYUNSDR_RUNTIME_LIBRARY "${LIBYUNSDR_SDK_ROOT}/bin/libyunsdr_ss.dll")
    if(NOT EXISTS "${LIBYUNSDR_INCLUDE_DIR}/yunsdr_api_ss.h" OR
       NOT EXISTS "${LIBYUNSDR_IMPORT_LIBRARY}" OR
       NOT EXISTS "${LIBYUNSDR_RUNTIME_LIBRARY}")
        message(FATAL_ERROR
            "The staged Y240 SDK is incomplete: ${LIBYUNSDR_SDK_ROOT}")
    endif()

    add_library(libyunsdr_ss SHARED IMPORTED)
    set_target_properties(libyunsdr_ss PROPERTIES
        IMPORTED_IMPLIB "${LIBYUNSDR_IMPORT_LIBRARY}"
        IMPORTED_LOCATION "${LIBYUNSDR_RUNTIME_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBYUNSDR_INCLUDE_DIR}"
    )
    target_sources(libyunsdr_isac_adapter PRIVATE src/libyunsdr_transport.cpp)
    target_link_libraries(libyunsdr_isac_adapter PUBLIC libyunsdr_ss)
    target_compile_definitions(
        libyunsdr_isac_adapter PUBLIC LIBYUNSDR_ISAC_HAVE_HARDWARE=1)
endif()

if(LIBYUNSDR_ISAC_BUILD_TOOLS)
    add_executable(yunsdr_probe tools/yunsdr_probe.cpp)
    target_compile_features(yunsdr_probe PRIVATE cxx_std_17)
    target_link_libraries(yunsdr_probe PRIVATE libyunsdr_isac::adapter)

    if(LIBYUNSDR_ISAC_ENABLE_HARDWARE)
        add_executable(yunsdr_phy_loopback tools/yunsdr_phy_loopback.cpp)
        target_compile_features(yunsdr_phy_loopback PRIVATE cxx_std_17)
        target_link_libraries(
            yunsdr_phy_loopback PRIVATE libyunsdr_isac::adapter)
        add_custom_command(TARGET yunsdr_phy_loopback POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LIBYUNSDR_RUNTIME_LIBRARY}"
                "$<TARGET_FILE_DIR:yunsdr_phy_loopback>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LIBYUNSDR_SDK_ROOT}/bin/libusb-1.0.dll"
                "$<TARGET_FILE_DIR:yunsdr_phy_loopback>"
        )

        add_executable(yunsdr_video_bridge tools/yunsdr_video_bridge.cpp)
        target_compile_features(yunsdr_video_bridge PRIVATE cxx_std_17)
        target_link_libraries(
            yunsdr_video_bridge PRIVATE libyunsdr_isac::adapter ws2_32)
        add_custom_command(TARGET yunsdr_video_bridge POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LIBYUNSDR_RUNTIME_LIBRARY}"
                "$<TARGET_FILE_DIR:yunsdr_video_bridge>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LIBYUNSDR_SDK_ROOT}/bin/libusb-1.0.dll"
                "$<TARGET_FILE_DIR:yunsdr_video_bridge>"
        )
    endif()
endif()

if(LIBYUNSDR_ISAC_BUILD_TESTS)
    enable_testing()
    add_executable(libyunsdr_isac_baseline_test tests/test_baseline.cpp)
    target_compile_features(libyunsdr_isac_baseline_test PRIVATE cxx_std_17)
    target_link_libraries(libyunsdr_isac_baseline_test PRIVATE libyunsdr_isac::adapter)
    add_test(NAME libyunsdr_isac_baseline COMMAND libyunsdr_isac_baseline_test)

    add_executable(libyunsdr_isac_transceiver_test tests/test_transceiver.cpp)
    target_compile_features(libyunsdr_isac_transceiver_test PRIVATE cxx_std_17)
    target_link_libraries(
        libyunsdr_isac_transceiver_test
        PRIVATE libyunsdr_isac::adapter)
    add_test(
        NAME libyunsdr_isac_transceiver
        COMMAND libyunsdr_isac_transceiver_test)

    add_executable(libyunsdr_isac_phy_codec_test tests/test_phy_codec.cpp)
    target_compile_features(libyunsdr_isac_phy_codec_test PRIVATE cxx_std_17)
    target_link_libraries(
        libyunsdr_isac_phy_codec_test PRIVATE libyunsdr_isac::adapter)
    add_test(
        NAME libyunsdr_isac_phy_codec
        COMMAND libyunsdr_isac_phy_codec_test)
endif()
