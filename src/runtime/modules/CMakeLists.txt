# Console modules both consoles have. Each console's runtime installs them under the name
# the DLL has there.
# BCPack's tables are built into the runtime where the tree has them: as bcpack_tables.cpp
# beside this file, which defines bcpack::builtin_tables(), or as bcpack-tables.bin, which such
# a file is made of here. Nothing then has to be put beside the executable. With neither the
# runtime has no tables of its own. Configure again after adding or removing one.
set(xv_bcpack_source "${CMAKE_CURRENT_SOURCE_DIR}/bcpack_tables.cpp")
if(NOT EXISTS "${xv_bcpack_source}")
set(xv_bcpack_source "${CMAKE_CURRENT_BINARY_DIR}/bcpack_builtin.cpp")
set(xv_bcpack_tables "${CMAKE_CURRENT_SOURCE_DIR}/bcpack-tables.bin")
set(xv_bcpack_builtin "std::span<const std::uint8_t> builtin_tables() { return {}; }")
if(EXISTS "${xv_bcpack_tables}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${xv_bcpack_tables}")
  file(READ "${xv_bcpack_tables}" xv_bcpack_hex HEX)
  string(REGEX REPLACE "(..)" "0x\\1," xv_bcpack_bytes "${xv_bcpack_hex}")
  string(REPEAT "." 80 xv_bcpack_row)
  string(REGEX REPLACE "(${xv_bcpack_row})" "\\1\n" xv_bcpack_bytes "${xv_bcpack_bytes}")
  set(xv_bcpack_builtin "namespace {
const std::uint8_t bytes[] = {
${xv_bcpack_bytes}
};
} // namespace
std::span<const std::uint8_t> builtin_tables() { return bytes; }")
endif()
file(CONFIGURE OUTPUT "${xv_bcpack_source}" CONTENT
"// BCPack's tables, built into the runtime
#include \"runtime/modules/bcpack.hpp\"
namespace xv::runtime::modules::bcpack {
@xv_bcpack_builtin@
} // namespace xv::runtime::modules::bcpack
" @ONLY)
endif()
add_library(xv_runtime_modules xg.cpp texture_layout.cpp kernel.cpp memory.cpp dstorage.cpp bcpack.cpp
  "${xv_bcpack_source}" console_shader.cpp console_shader_rdna.cpp watchdog.cpp acphal.cpp window.cpp system.cpp placement.cpp storage.cpp gameinput.cpp locale.cpp network.cpp
  d3d12x/device.cpp d3d12x/present.cpp
  gameruntime/runtime.cpp gameruntime/slots.cpp gameruntime/task_queue.cpp gameruntime/async.cpp
  gameruntime/user.cpp gameruntime/save.cpp gameruntime/system.cpp gameruntime/store.cpp
  gameruntime/ui.cpp gameruntime/streaming.cpp)
target_include_directories(xv_runtime_modules PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(xv_runtime_modules PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xv_runtime_modules PRIVATE xv_options xv_core xv_host xv_game_config xv_runtime_common
  xv_graphics xv_input xv_surface d3d12 dxgi shlwapi onecore ole32 sapi ws2_32)
