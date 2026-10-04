# SPDX-License-Identifier: GPL-3.0-or-later
#
# Networking: transports behind the Wi-Fi block's MpTransport/NetDriver.
# Own library so the core stays socket-free; pulled in only with DSPERATE_NET.
#
# enet/ is vendored; probe for what its unix.c needs, per upstream's CMake.
include(CheckFunctionExists)
include(CheckStructHasMember)
include(CheckTypeSize)
check_function_exists("fcntl" ENET_HAS_FCNTL)
check_function_exists("poll" ENET_HAS_POLL)
check_function_exists("getaddrinfo" ENET_HAS_GETADDRINFO)
check_function_exists("getnameinfo" ENET_HAS_GETNAMEINFO)
check_function_exists("gethostbyname_r" ENET_HAS_GETHOSTBYNAME_R)
check_function_exists("gethostbyaddr_r" ENET_HAS_GETHOSTBYADDR_R)
check_function_exists("inet_pton" ENET_HAS_INET_PTON)
check_function_exists("inet_ntop" ENET_HAS_INET_NTOP)
check_struct_has_member("struct msghdr" "msg_flags" "sys/types.h;sys/socket.h" ENET_HAS_MSGHDR_FLAGS)
set(CMAKE_EXTRA_INCLUDE_FILES "sys/types.h" "sys/socket.h")
check_type_size("socklen_t" ENET_HAS_SOCKLEN_T BUILTIN_TYPES_ONLY)
unset(CMAKE_EXTRA_INCLUDE_FILES)

set(ENET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/enet)
add_library(dsperate_enet STATIC
  ${ENET_DIR}/callbacks.c ${ENET_DIR}/compress.c ${ENET_DIR}/host.c ${ENET_DIR}/list.c
  ${ENET_DIR}/packet.c ${ENET_DIR}/peer.c ${ENET_DIR}/protocol.c ${ENET_DIR}/unix.c)
target_include_directories(dsperate_enet PUBLIC ${ENET_DIR}/include)
foreach(probe FCNTL POLL GETADDRINFO GETNAMEINFO GETHOSTBYNAME_R GETHOSTBYADDR_R INET_PTON INET_NTOP MSGHDR_FLAGS SOCKLEN_T)
  if(ENET_HAS_${probe})
    target_compile_definitions(dsperate_enet PRIVATE HAS_${probe}=1)
  endif()
endforeach()
# Third-party C: not ours to fix warnings in.
target_compile_options(dsperate_enet PRIVATE -w)

# slirp/ is vendored, with a glib shim in slirp/glib (libslirp sources
# #include <glib.h> and find it there). Source list matches upstream's meson build.
set(SLIRP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/slirp)
add_library(dsperate_slirp STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/arp_table.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/bootp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/cksum.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/dhcpv6.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/dnssearch.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/if.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/ip6_icmp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/ip6_input.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/ip6_output.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/ip_icmp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/ip_input.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/ip_output.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/mbuf.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/misc.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/ncsi.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/ndp_table.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/sbuf.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/slirp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/socket.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/state.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/stream.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/tcp_input.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/tcp_output.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/tcp_subr.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/tcp_timer.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/tftp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/udp6.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/udp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/util.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/version.c
  ${CMAKE_CURRENT_SOURCE_DIR}/slirp/vmstate.c
  ${SLIRP_DIR}/glib/glib.c)
target_include_directories(dsperate_slirp PUBLIC ${SLIRP_DIR} PRIVATE ${SLIRP_DIR}/glib)
set_target_properties(dsperate_slirp PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
# Third-party C: not ours to fix warnings in. -w doesn't hide a missing
# shim symbol -- that still fails the link.
target_compile_options(dsperate_slirp PRIVATE -w)

add_library(dsperate_net STATIC
  lan_mp.cpp
  slirp_driver.cpp)
target_include_directories(dsperate_net PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(dsperate_net PUBLIC dsperate_core dsperate_enet dsperate_slirp)
