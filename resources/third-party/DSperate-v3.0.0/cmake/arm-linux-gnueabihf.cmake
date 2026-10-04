# SPDX-License-Identifier: GPL-3.0-or-later
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)
set(CMAKE_C_COMPILER   arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# The A32 JIT and the NEON subset both need an ARMv7-A + NEON baseline; the
# Debian default (armv7-a without NEON) would silently drop the kernels.
set(CMAKE_C_FLAGS_INIT   "-march=armv7-a -mfpu=neon -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-march=armv7-a -mfpu=neon -mfloat-abi=hard")
# Lets `ctest` run cross-built binaries on an x86 host.
find_program(QEMU_ARM qemu-arm-static)
if(QEMU_ARM)
  set(CMAKE_CROSSCOMPILING_EMULATOR "${QEMU_ARM};-L;/usr/arm-linux-gnueabihf")
endif()
# Multiarch armhf packages (libsdl2-dev:armhf) install at the real paths, not
# under the sysroot, so point pkg-config at them; without this pkg_check_modules
# would answer with the host's amd64 sdl2 and only fail at link time.
if(IS_DIRECTORY /usr/lib/arm-linux-gnueabihf/pkgconfig)
  set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig")
  list(APPEND CMAKE_FIND_ROOT_PATH /usr/lib/arm-linux-gnueabihf /usr/include/arm-linux-gnueabihf)
endif()
