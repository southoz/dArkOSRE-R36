# SPDX-License-Identifier: GPL-3.0-or-later
#
# RetroAchievements support, as its own library: keeps dsperate_core and the
# headless frontend free of the network transport, and lets tests link
# frontend translation units one at a time.
#
# Not built: rc_client_raintegration.c (Windows-only authoring toolkit),
# rc_libretro.c (libretro integration).
set(RC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rcheevos)

# Whole-directory: these translation units reference each other freely
# (trigger -> condset -> condition -> operand -> memref), so a subset would be
# guesswork that breaks on an update.
file(GLOB RC_RUNTIME_SOURCES ${RC_DIR}/src/rcheevos/*.c)

file(GLOB RC_API_SOURCES ${RC_DIR}/src/rapi/*.c)

add_library(dsperate_cheevos STATIC
  cheevos_hash.cpp
  cheevos_memory.cpp
  cheevos_http.cpp
  cheevos_client.cpp
  ${RC_DIR}/src/rc_client.c
  ${RC_API_SOURCES}
  ${RC_RUNTIME_SOURCES}
  ${RC_DIR}/src/rhash/hash.c
  ${RC_DIR}/src/rhash/hash_rom.c
  ${RC_DIR}/src/rhash/md5.c
  ${RC_DIR}/src/rc_util.c
  ${RC_DIR}/src/rc_compat.c
  ${RC_DIR}/src/rc_version.c
)

target_include_directories(dsperate_cheevos
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/..
  PRIVATE ${RC_DIR}/include ${RC_DIR}/src ${RC_DIR}/src/rhash)

# Disc/encrypted(3DS)/zip hash paths are for consoles we never ask about,
# dropping cdreader.c, aes.c, hash_disc.c, hash_encrypted.c, hash_zip.c.
# RC_HASH_NO_ZIP costs nothing even for zipped ROMs: those reach rcheevos
# already unpacked/mapped through our own RomSource (see cheevos_hash.cpp).
target_compile_definitions(dsperate_cheevos PRIVATE
  RC_HASH_NO_DISC RC_HASH_NO_ENCRYPTED RC_HASH_NO_ZIP RC_DISABLE_LUA)
target_include_directories(dsperate_cheevos PRIVATE ${RC_DIR}/src/rapi)

# User-Agent version DSperate reports (load-bearing, see cheevos_memory.h).
# Taken from the latest git tag; the literal below is the fallback for a
# tarball build or a checkout with no tags.
set(DSPERATE_CHEEVOS_VERSION "" CACHE STRING "Version DSperate reports to RetroAchievements (default: latest git tag)")
if(NOT DSPERATE_CHEEVOS_VERSION)
  execute_process(COMMAND git -C ${CMAKE_SOURCE_DIR} describe --tags --abbrev=0
                  OUTPUT_VARIABLE _ds_tag OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE _ds_tag_rc)
  if(_ds_tag_rc EQUAL 0 AND _ds_tag MATCHES "^v?([0-9]+\\.[0-9]+(\\.[0-9]+)?)$")
    set(DSPERATE_CHEEVOS_VERSION "${CMAKE_MATCH_1}")
  else()
    set(DSPERATE_CHEEVOS_VERSION "1.13.1")
  endif()
endif()
message(STATUS "DSperate: RetroAchievements client version ${DSPERATE_CHEEVOS_VERSION}")
target_compile_definitions(dsperate_cheevos PRIVATE
  DSPERATE_CHEEVOS_VERSION="${DSPERATE_CHEEVOS_VERSION}")

# dsperate_core knows nothing about achievements; the dependency runs this way.
target_link_libraries(dsperate_cheevos PUBLIC dsperate_core ${CMAKE_DL_LIBS})
