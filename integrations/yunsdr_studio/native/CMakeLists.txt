cmake_minimum_required(VERSION 3.15)
project(OpenIsacStudioAdapter LANGUAGES CXX)

get_filename_component(default_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(OPENISAC_ROOT "${default_root}" CACHE PATH "OpenISAC-YunSDR source checkout")
set(OPENISAC_PHY_LIBRARY_ONLY ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory("${OPENISAC_ROOT}/cpp_phy" openisac EXCLUDE_FROM_ALL)

add_library(studio_openisac SHARED studio_phy.cpp rank8_frame.cpp)
target_link_libraries(studio_openisac PRIVATE openisac_phy)

include(CTest)
if(BUILD_TESTING)
    add_executable(verify_studio_phy verify_studio_phy.cpp)
    target_link_libraries(verify_studio_phy PRIVATE studio_openisac openisac_phy)
    add_test(NAME studio_phy_816 COMMAND verify_studio_phy "${OPENISAC_ROOT}")
endif()

install(TARGETS studio_openisac RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES "${OPENISAC_ROOT}/LDPC_504_1008.alist"
              "${OPENISAC_ROOT}/LDPC_504_1008G.alist" "${OPENISAC_ROOT}/LICENSE" DESTINATION .)
