# Console modules both consoles have. Each console's runtime installs them under the name
# the DLL has there. BCPack's constant tables are built in as bcpack-tables.cpp, so no file
# has to be put beside the executable.
add_library(xv_runtime_modules xg.cpp texture_layout.cpp kernel.cpp memory.cpp dstorage.cpp bcpack.cpp
  bcpack-tables.cpp console_shader.cpp console_shader_rdna.cpp watchdog.cpp acphal.cpp window.cpp system.cpp placement.cpp storage.cpp gameinput.cpp locale.cpp network.cpp
  d3d12x/device.cpp d3d12x/present.cpp d3d12x/capture.cpp
  gameruntime/runtime.cpp gameruntime/slots.cpp gameruntime/task_queue.cpp gameruntime/async.cpp
  gameruntime/user.cpp gameruntime/save.cpp gameruntime/system.cpp gameruntime/store.cpp
  gameruntime/ui.cpp gameruntime/streaming.cpp)
target_include_directories(xv_runtime_modules PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(xv_runtime_modules PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xv_runtime_modules PRIVATE xv_options xv_core xv_host xv_game_config xv_runtime_common
  xv_graphics xv_input xv_surface d3d12 dxgi shlwapi onecore ole32 sapi ws2_32)
