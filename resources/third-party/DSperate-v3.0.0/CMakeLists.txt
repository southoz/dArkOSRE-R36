# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(DSperate VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(DSPERATE_JIT      "Build the recompiler (AArch64 and ARMv7 hosts)"          ON)
option(DSPERATE_NEON     "Build NEON renderer kernels (AArch64 or ARMv7+NEON)"    ON)
option(DSPERATE_TESTS    "Build unit tests"                                       ON)
option(DSPERATE_HEADLESS "Build the headless frontend"                            ON)
option(DSPERATE_SDL      "Build the SDL2 frontend (off when SDL2 is not found)"   ON)
option(DSPERATE_WAYLAND  "Build the Wayland dmabuf scanout tier (off when SDL2 has no Wayland)" ON)
option(DSPERATE_CHEEVOS  "Build RetroAchievements support"                  ON)
option(DSPERATE_NET      "Build the Wi-Fi transports (local wireless over the LAN)" ON)

# JIT and NEON kernels are gated separately: an ARMv7 host gets NEON kernels
# with its own JIT backend, a non-ARM host gets neither.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(DSPERATE_HOST_AARCH64 ON)
  set(DSPERATE_HOST_ARM32 OFF)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7|armv7l|armv7a|armhf)$")
  set(DSPERATE_HOST_AARCH64 OFF)
  set(DSPERATE_HOST_ARM32 ON)
else()
  set(DSPERATE_HOST_AARCH64 OFF)
  set(DSPERATE_HOST_ARM32 OFF)
endif()

if(NOT DSPERATE_HOST_AARCH64 AND NOT DSPERATE_HOST_ARM32 AND DSPERATE_JIT)
  message(STATUS "DSperate: host is ${CMAKE_SYSTEM_PROCESSOR}; JIT disabled (interpreter only)")
  set(DSPERATE_JIT OFF)
endif()

# ARMv7 NEON isn't IEEE-clean by default; pin VFPv4 + hard-float so results
# can't silently differ from AArch64.
if(DSPERATE_HOST_ARM32 AND DSPERATE_NEON)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-mfpu=neon-vfpv4 -mfloat-abi=hard")
  check_cxx_source_compiles("#include <arm_neon.h>
    int main() { return vgetq_lane_u8(vdupq_n_u8(0), 0); }" DSPERATE_ARM32_NEON_OK)
  unset(CMAKE_REQUIRED_FLAGS)
  if(DSPERATE_ARM32_NEON_OK)
    add_compile_options(-mfpu=neon-vfpv4 -mfloat-abi=hard)
    message(STATUS "DSperate: ARMv7 host; NEON kernels built from the A32 subset")
  else()
    message(STATUS "DSperate: ARMv7 host without usable NEON; ref kernels only")
    set(DSPERATE_NEON OFF)
  endif()
elseif(NOT DSPERATE_HOST_AARCH64 AND DSPERATE_NEON)
  set(DSPERATE_NEON OFF)
endif()

foreach(cfg RELEASE RELWITHDEBINFO)
  string(REPLACE "-O2" "-O3" CMAKE_C_FLAGS_${cfg}   "${CMAKE_C_FLAGS_${cfg}}")
  string(REPLACE "-O2" "-O3" CMAKE_CXX_FLAGS_${cfg} "${CMAKE_CXX_FLAGS_${cfg}}")
endforeach()

# OFF also cancels the distro-injected _FORTIFY_SOURCE, which turns small
# fixed-size memcpys in the pixel loops into __memcpy_chk calls.
option(DSPERATE_HARDEN "Build with -fstack-protector and _FORTIFY_SOURCE" OFF)
if(DSPERATE_HARDEN)
  add_compile_options(-fstack-protector)
else()
  add_compile_options(-fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)   # -U first: a toolchain that injects it warns about a redefinition otherwise
endif()

# e.g. "-mtune=cortex-a55" (scheduling only) or "-mcpu=cortex-a55" (also raises
# the ISA baseline). Passed at link too so LTO codegen sees it.
set(DSPERATE_TUNE "" CACHE STRING "Extra -mcpu/-mtune options for the target core")
if(DSPERATE_TUNE)
  separate_arguments(_ds_tune NATIVE_COMMAND "${DSPERATE_TUNE}")
  add_compile_options(${_ds_tune})
  add_link_options(${_ds_tune})
endif()

# PGO: the profile is committed under pgo/ so "use" builds need no training
# ROMs. It applies per function (edited functions just lose theirs), but is
# tied to compiler version and flags, which MANIFEST's fingerprint records.
set(DSPERATE_PGO "OFF" CACHE STRING "Profile-guided optimisation: OFF | generate | use")
set_property(CACHE DSPERATE_PGO PROPERTY STRINGS OFF generate use)
# pgo/<arch>-gcc<version>/ wins over pgo/<arch>/ when present, so other GCC
# versions can carry their own profile.
set(_ds_pgo_default "${CMAKE_SOURCE_DIR}/pgo/${CMAKE_SYSTEM_PROCESSOR}")
if(EXISTS "${_ds_pgo_default}-gcc${CMAKE_CXX_COMPILER_VERSION}/MANIFEST")
  set(_ds_pgo_default "${_ds_pgo_default}-gcc${CMAKE_CXX_COMPILER_VERSION}")
endif()
set(DSPERATE_PGO_DIR "${_ds_pgo_default}" CACHE PATH "Where the PGO profile (.gcda files and MANIFEST) lives")
option(DSPERATE_PGO_STRICT "PGO use: warn about every function without a matching profile" OFF)
# Flags every object sees; a flag added per target must be added here too.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _ds_build_type_uc)
string(SHA1 _ds_pgo_fingerprint
  "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} ${CMAKE_SYSTEM_PROCESSOR} ${CMAKE_BUILD_TYPE} ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_ds_build_type_uc}} harden=${DSPERATE_HARDEN} tune=${DSPERATE_TUNE} jit=${DSPERATE_JIT} neon=${DSPERATE_NEON}")
set(DSPERATE_PGO_FINGERPRINT "${_ds_pgo_fingerprint}" CACHE INTERNAL "")
file(WRITE "${CMAKE_BINARY_DIR}/pgo-fingerprint" "${_ds_pgo_fingerprint}\n")
# A different checkout path only trips GCC's location checksum (a warning);
# the counts still apply. Only control-flow mismatches drop a profile.
if(DSPERATE_PGO STREQUAL "generate")
  add_compile_options(-fprofile-generate=${DSPERATE_PGO_DIR} -fprofile-prefix-path=${CMAKE_BINARY_DIR} -fprofile-update=atomic)
  add_link_options(-fprofile-generate=${DSPERATE_PGO_DIR})
elseif(DSPERATE_PGO STREQUAL "use")
  if(NOT EXISTS "${DSPERATE_PGO_DIR}/MANIFEST")
    message(FATAL_ERROR "DSPERATE_PGO=use but ${DSPERATE_PGO_DIR}/MANIFEST does not exist; run tools/pgo_refresh.sh")
  endif()
  file(STRINGS "${DSPERATE_PGO_DIR}/MANIFEST" _ds_pgo_manifest REGEX "^fingerprint ")
  string(REGEX REPLACE "^fingerprint " "" _ds_pgo_manifest "${_ds_pgo_manifest}")
  if(NOT _ds_pgo_manifest STREQUAL _ds_pgo_fingerprint)
    message(FATAL_ERROR "PGO profile in ${DSPERATE_PGO_DIR} was made for a different compiler or flags (fingerprint ${_ds_pgo_manifest}, this build ${_ds_pgo_fingerprint}); run tools/pgo_refresh.sh, or build with -DDSPERATE_PGO=OFF")
  endif()
  add_compile_options(-fprofile-use=${DSPERATE_PGO_DIR} -fprofile-prefix-path=${CMAKE_BINARY_DIR} -fprofile-correction)
  if(DSPERATE_PGO_STRICT)
    add_compile_options(-Wno-error=coverage-mismatch)
  else()
    add_compile_options(-Wno-missing-profile -Wno-coverage-mismatch)
  endif()
endif()

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

add_subdirectory(src/core)
add_subdirectory(src/frontend/video)
if(DSPERATE_CHEEVOS)
  add_subdirectory(src/cheevos)
endif()
if(DSPERATE_NET)
  add_subdirectory(src/net)
endif()
if(DSPERATE_HEADLESS)
  add_subdirectory(src/frontend/headless)
endif()
if(DSPERATE_SDL)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 QUIET sdl2)
  endif()
  if(NOT SDL2_FOUND)
    find_package(SDL2 QUIET)
    if(SDL2_FOUND AND TARGET SDL2::SDL2)
      set(SDL2_LIBRARIES SDL2::SDL2)
    endif()
  endif()
  if(SDL2_FOUND)
    # SDL_SysWMinfo.info.wl only exists in SDL2 builds with Wayland (handheld
    # fbdev/KMSDRM builds lack it). libwayland is dlopened, never linked.
    if(DSPERATE_WAYLAND)
      include(CheckCXXSourceCompiles)
      set(CMAKE_REQUIRED_INCLUDES "${SDL2_INCLUDE_DIRS}")
      if(TARGET SDL2::SDL2)
        get_target_property(_sdl2_inc SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
        if(_sdl2_inc)
          list(APPEND CMAKE_REQUIRED_INCLUDES ${_sdl2_inc})
        endif()
      endif()
      check_cxx_source_compiles("#include <SDL2/SDL_syswm.h>
        int main() { SDL_SysWMinfo i; return i.info.wl.display ? 1 : 0; }" DSPERATE_SDL2_HAS_WAYLAND)
      unset(CMAKE_REQUIRED_INCLUDES)
      if(NOT DSPERATE_SDL2_HAS_WAYLAND)
        message(STATUS "DSperate: SDL2 built without Wayland; the dmabuf scanout tier is not built")
        set(DSPERATE_WAYLAND OFF)
      endif()
    endif()
    add_subdirectory(src/frontend/sdl)
  else()
    message(STATUS "DSperate: SDL2 not found; the SDL frontend is not built")
  endif()
endif()
if(DSPERATE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
