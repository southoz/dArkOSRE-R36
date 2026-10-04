Vendored from https://github.com/lsalzman/enet -- ENet v1.3.18, commit
2662c0de09e36f2a2030ccc2c528a3e4c9e8138a.

The reliable-UDP library melonDS's LAN multiplayer speaks (its net/LAN.cpp
links the system's); DSperate's ../lan_mp.cpp speaks the same wire format
over this copy so a DSperate can be a peer of a melonDS. Vendored rather
than found because the two static handheld tiers (Miyoo A30, RG35XX SP)
cannot dlopen and their sysroots have no ENet.

Only the Unix sources are here (unix.c; win32.c is not vendored, nor the
docs, autotools and premake files). The feature probes ENet's own CMake
runs (HAS_FCNTL, HAS_POLL, HAS_GETADDRINFO, HAS_GETNAMEINFO,
HAS_GETHOSTBYNAME_R, HAS_GETHOSTBYADDR_R, HAS_INET_PTON, HAS_INET_NTOP,
HAS_MSGHDR_FLAGS, HAS_SOCKLEN_T) are run by ../CMakeLists.txt.

MIT licensed (see LICENSE), compatible with this project's GPL-3.0-or-later.
Do not hand-edit: re-copy from a newer upstream tag and update the commit
recorded above.
