// SPDX-License-Identifier: GPL-3.0-or-later
#include "lid.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <ctime>
#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#endif

namespace ds::sdl {

namespace {
s64 clock_ns(clockid_t id) {
  timespec ts{};
  clock_gettime(id, &ts);
  return static_cast<s64>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
#ifdef __linux__
bool switch_state(int fd, bool& closed) {
  unsigned long bits[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
  if (ioctl(fd, EVIOCGSW(sizeof bits), bits) < 0) return false;
  closed = (bits[SW_LID / (8 * sizeof(long))] >> (SW_LID % (8 * sizeof(long)))) & 1;
  return true;
}
#endif
}

void Lid::open() {
  skew_ns_ = clock_ns(CLOCK_BOOTTIME) - clock_ns(CLOCK_MONOTONIC);
#ifdef __linux__
  DIR* d = opendir("/dev/input");
  if (!d) return;
  while (dirent* e = readdir(d)) {
    if (std::strncmp(e->d_name, "event", 5) != 0) continue;
    const std::string path = std::string("/dev/input/") + e->d_name;
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    unsigned long sw[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
    if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof sw), sw) >= 0 && ((sw[SW_LID / (8 * sizeof(long))] >> (SW_LID % (8 * sizeof(long)))) & 1)) {
      char name[64] = "?";
      ioctl(fd, EVIOCGNAME(sizeof name), name);
      fd_ = fd;
      switch_state(fd_, closed_);
      std::fprintf(stderr, "lid: %s (%s), %s\n", path.c_str(), name, closed_ ? "closed" : "open");
      break;
    }
    ::close(fd);
  }
  closedir(d);
#endif
}

void Lid::close() {
#ifdef __linux__
  if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
}

bool Lid::poll(bool& closed) {
  const bool was = closed_;
#ifdef __linux__
  if (fd_ >= 0) {
    input_event ev;
    bool any = false;
    while (read(fd_, &ev, sizeof ev) == static_cast<ssize_t>(sizeof ev)) any = true;
    if (any) switch_state(fd_, closed_);
  }
#endif
  // No switch (or host froze before the event): clocks drifted >1s apart.
  const s64 skew = clock_ns(CLOCK_BOOTTIME) - clock_ns(CLOCK_MONOTONIC);
  if (skew - skew_ns_ > 1000000000 && fd_ < 0) { pulse_ = 6; std::fprintf(stderr, "lid: host resumed from suspend; pulsing the lid\n"); }
  skew_ns_ = skew;
  if (pulse_ > 0) { --pulse_; closed = pulse_ > 0; return true; }
  closed = closed_;
  return closed_ != was;
}

} // namespace ds::sdl
