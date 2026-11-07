# Host tools the 3DS build needs from the desktop project, without configuring the whole game:
# the RSPRecomp output fixup and the game-state table generator.
cmake_minimum_required(VERSION 3.20)
project(rs64_3ds_helpers CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(RS64 "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_executable(fixup_factor5_ucode "${RS64}/tools/fixup_factor5_ucode.cpp")
add_executable(gen_state_table_tool "${RS64}/tools/state/gen_state_table.cpp")
target_include_directories(gen_state_table_tool PRIVATE "${RS64}/lib/N64ModernRuntime/N64Recomp/lib/tomlplusplus/include")
