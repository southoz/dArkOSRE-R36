# SPDX-License-Identifier: GPL-3.0-or-later
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
# DS_CROSS_GCC pins a GCC major version (e.g. 12), so a second PGO profile can
# be built with the compiler a downstream distro uses; Debian's cross packages
# install versioned binaries alongside each other. Empty = the distro default.
if(NOT DEFINED DS_CROSS_GCC AND DEFINED ENV{DS_CROSS_GCC})
  set(DS_CROSS_GCC "$ENV{DS_CROSS_GCC}")
endif()
set(_ds_gcc_suffix "")
if(DS_CROSS_GCC)
  set(_ds_gcc_suffix "-${DS_CROSS_GCC}")
endif()
set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc${_ds_gcc_suffix})
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++${_ds_gcc_suffix})
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Lets `ctest` run cross-built binaries on an x86 host.
find_program(QEMU_AARCH64 qemu-aarch64-static)
if(QEMU_AARCH64)
  set(CMAKE_CROSSCOMPILING_EMULATOR "${QEMU_AARCH64};-L;/usr/aarch64-linux-gnu")
endif()
