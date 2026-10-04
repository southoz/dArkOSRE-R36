# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(test_page_table page_table_test.cpp)
target_link_libraries(test_page_table PRIVATE dsperate_core)
add_test(NAME page_table COMMAND test_page_table)

add_executable(test_scheduler scheduler_test.cpp)
target_link_libraries(test_scheduler PRIVATE dsperate_core)
add_test(NAME scheduler COMMAND test_scheduler)

add_executable(test_interp interp_test.cpp)
target_link_libraries(test_interp PRIVATE dsperate_core)
add_test(NAME interp COMMAND test_interp)

add_executable(test_gpu gpu_test.cpp)
target_link_libraries(test_gpu PRIVATE dsperate_core)
add_test(NAME gpu COMMAND test_gpu)

add_executable(test_layout layout_test.cpp)
target_link_libraries(test_layout PRIVATE dsperate_frontend_video)
add_test(NAME layout COMMAND test_layout)

add_executable(test_select select_test.cpp)
target_link_libraries(test_select PRIVATE dsperate_frontend_video)
add_test(NAME select COMMAND test_select)

add_executable(test_io io_test.cpp)
target_link_libraries(test_io PRIVATE dsperate_core)
add_test(NAME io COMMAND test_io)

add_executable(test_cart_save cart_save_test.cpp)
target_link_libraries(test_cart_save PRIVATE dsperate_core)
add_test(NAME cart_save COMMAND test_cart_save)

add_executable(test_gpu3d gpu3d_test.cpp)
target_link_libraries(test_gpu3d PRIVATE dsperate_core)
add_test(NAME gpu3d COMMAND test_gpu3d)

add_executable(test_fastmem fastmem_test.cpp)
target_link_libraries(test_fastmem PRIVATE dsperate_core)
add_test(NAME fastmem COMMAND test_fastmem)

add_executable(test_div64 div64_test.cpp)
target_link_libraries(test_div64 PRIVATE dsperate_core)
add_test(NAME div64 COMMAND test_div64)

add_executable(test_kernels kernels_test.cpp)
target_link_libraries(test_kernels PRIVATE dsperate_core)
add_test(NAME kernels COMMAND test_kernels)

if(DSPERATE_JIT)
  add_executable(test_jit jit_test.cpp)
  target_link_libraries(test_jit PRIVATE dsperate_core)
  add_test(NAME jit COMMAND test_jit)
endif()

add_executable(test_spu spu_test.cpp)
target_link_libraries(test_spu PRIVATE dsperate_core)
add_test(NAME spu COMMAND test_spu)

add_executable(test_aes aes_test.cpp)
target_link_libraries(test_aes PRIVATE dsperate_core)
add_test(NAME aes COMMAND test_aes)

add_executable(test_texcache texcache_test.cpp)
target_link_libraries(test_texcache PRIVATE dsperate_core)
add_test(NAME texcache COMMAND test_texcache)

# The pause menu is frontend code; the test links the one translation unit it
# needs rather than the SDL frontend (which is an executable, and needs a
# display to do anything).
add_executable(test_menu menu_test.cpp ../src/frontend/sdl/menu.cpp ../src/frontend/sdl/settings.cpp)
target_link_libraries(test_menu PRIVATE dsperate_core)
add_test(NAME menu COMMAND test_menu)

# The settings file, likewise frontend code: the menu and the layout hotkeys
# both rewrite single keys in it, and it had no tests at all.
add_executable(test_config config_test.cpp ../src/frontend/sdl/config.cpp)
target_link_libraries(test_config PRIVATE dsperate_core)
add_test(NAME config COMMAND test_config)

# Input, for the menu's way back from a broken binding: the fallback layer and
# the reachability warning both have to stay silent under the defaults, which
# only a test will keep true. Guarded because it needs SDL2's headers, which
# the core tests do not.
if(DSPERATE_SDL)
  add_executable(test_input input_test.cpp ../src/frontend/sdl/input.cpp ../src/frontend/sdl/pad_faces.cpp ../src/frontend/sdl/config.cpp)
  target_link_libraries(test_input PRIVATE dsperate_core ${SDL2_LIBRARIES})
  target_include_directories(test_input PRIVATE ${SDL2_INCLUDE_DIRS})
  add_test(NAME input COMMAND test_input)
endif()

# The console's own settings inside a firmware image, and the sidecar they are
# kept in when the firmware is a dump the emulator must not write to.
add_executable(test_firmware firmware_test.cpp)
target_link_libraries(test_firmware PRIVATE dsperate_core)
add_test(NAME firmware COMMAND test_firmware)

add_executable(test_cheat cheat_test.cpp)
target_link_libraries(test_cheat PRIVATE dsperate_core)
add_test(NAME cheat COMMAND test_cheat)

add_executable(test_cheat_db cheat_db_test.cpp)
target_link_libraries(test_cheat_db PRIVATE dsperate_core)
add_test(NAME cheat_db COMMAND test_cheat_db)

add_executable(test_zip zip_test.cpp)
target_link_libraries(test_zip PRIVATE dsperate_core)
target_include_directories(test_zip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME zip COMMAND test_zip)

add_executable(test_rom_source rom_source_test.cpp)
target_link_libraries(test_rom_source PRIVATE dsperate_core)
add_test(NAME rom_source COMMAND test_rom_source)

# The RetroAchievements hash.
# Guarded because the vendored rcheevos is only built with the feature.
if(DSPERATE_CHEEVOS)
  add_executable(test_cheevos_hash cheevos_hash_test.cpp)
  target_link_libraries(test_cheevos_hash PRIVATE dsperate_cheevos)
  add_test(NAME cheevos_hash COMMAND test_cheevos_hash)

  # The memory window and the condition runtime (phase 2). Reaches rcheevos'
  # own headers to drive rc_runtime directly, which the library does not expose
  # to its users, so the include path is added here rather than made PUBLIC.
  add_executable(test_cheevos_memory cheevos_memory_test.cpp)
  target_link_libraries(test_cheevos_memory PRIVATE dsperate_cheevos)
  target_include_directories(test_cheevos_memory PRIVATE ${CMAKE_SOURCE_DIR}/src/cheevos/rcheevos/include)
  add_test(NAME cheevos_memory COMMAND test_cheevos_memory)

  # The credential sidecar (phase 3). No network.
  add_executable(test_cheevos_client cheevos_client_test.cpp)
  target_link_libraries(test_cheevos_client PRIVATE dsperate_cheevos)
  add_test(NAME cheevos_client COMMAND test_cheevos_client)
endif()

# The internet driver: the DNS rewrite's
# checksum arithmetic, and the DS's first conversation with the user-mode
# stack. Guarded because libslirp is only vendored in with the feature; no
# traffic leaves the machine.
if(DSPERATE_NET)
  add_executable(test_slirp_driver slirp_driver_test.cpp)
  target_link_libraries(test_slirp_driver PRIVATE dsperate_net)
  add_test(NAME slirp_driver COMMAND test_slirp_driver)
endif()

add_executable(test_nand_fs nand_fs_test.cpp)
target_link_libraries(test_nand_fs PRIVATE dsperate_core)
add_test(NAME nand_fs COMMAND test_nand_fs)

add_executable(test_sd_card sd_card_test.cpp)
target_link_libraries(test_sd_card PRIVATE dsperate_core)
add_test(NAME sd_card COMMAND test_sd_card)

# GPU raster probe (gpu-raster branch): times a recorded frame on the Vulkan raster. Not a test.
if(DSPERATE_VULKAN AND DSPERATE_VULKAN_INCLUDE)   # the headers may be missing on a build host (CI), which switches the raster off
  add_executable(vk_probe vk_probe.cpp)
  target_link_libraries(vk_probe PRIVATE dsperate_core ${CMAKE_DL_LIBS})   # the Vulkan loader is dlopen'd; a static-libstdc++ toolchain needs dl named here
  target_include_directories(vk_probe PRIVATE ${DSPERATE_VULKAN_INCLUDE})
endif()

# RGA probe (gpu-raster branch): times the Rockchip 2D scaler (V4L2 mem2mem) on a dma-buf frame. Device-only, not a test.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(rga_probe rga_probe.cpp)
endif()
