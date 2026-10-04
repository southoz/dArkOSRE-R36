// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DRM UAPI structs/ioctl numbers needed here, so the build needs neither
// libdrm nor kernel DRM headers (handhelds ship libdrm.so but no
// <drm/drm.h>). Kernel UAPI layout is fixed forever, so copying fields is
// safe. Taken verbatim from linux/include/uapi/drm/drm.h and drm_mode.h.
#pragma once

#include <cstdint>
#include <sys/ioctl.h>

namespace ds::sdl::drmu {

struct mode_modeinfo {
  uint32_t clock;
  uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
  uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
  uint32_t vrefresh;
  uint32_t flags;
  uint32_t type;
  char name[32];
};

struct mode_card_res {
  uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
  uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
  uint32_t min_width, max_width, min_height, max_height;
};

struct mode_crtc {
  uint64_t set_connectors_ptr;
  uint32_t count_connectors;
  uint32_t crtc_id, fb_id;
  uint32_t x, y;
  uint32_t gamma_size, mode_valid;
  mode_modeinfo mode;
};

struct mode_get_encoder {
  uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
};

struct mode_get_connector {
  uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
  uint32_t count_modes, count_props, count_encoders;
  uint32_t encoder_id, connector_id, connector_type, connector_type_id;
  uint32_t connection;          // 1 = connected
  uint32_t mm_width, mm_height, subpixel;
  uint32_t pad;
};

struct mode_fb_cmd2 {
  uint32_t fb_id, width, height, pixel_format, flags;
  uint32_t handles[4], pitches[4], offsets[4];
  uint64_t modifier[4];
};

struct mode_crtc_page_flip {
  uint32_t crtc_id, fb_id, flags, reserved;
  uint64_t user_data;
};

struct prime_handle {
  uint32_t handle, flags;
  int32_t fd;
};

struct gem_close { uint32_t handle, pad; };

struct event { uint32_t type, length; };
struct event_vblank {
  event base;
  uint64_t user_data;
  uint32_t tv_sec, tv_usec, sequence, crtc_id;
};

constexpr uint32_t EVENT_FLIP_COMPLETE = 0x02;
constexpr uint32_t PAGE_FLIP_EVENT = 0x01;
constexpr uint32_t CONNECTOR_CONNECTED = 1;

#define DS_DRM_IOWR(nr, type) _IOWR('d', (nr), type)

constexpr unsigned long IOCTL_SET_MASTER        = _IO('d', 0x1e);
constexpr unsigned long IOCTL_DROP_MASTER       = _IO('d', 0x1f);
constexpr unsigned long IOCTL_GEM_CLOSE         = _IOW('d', 0x09, gem_close);
constexpr unsigned long IOCTL_PRIME_FD_TO_HANDLE = DS_DRM_IOWR(0x2e, prime_handle);
constexpr unsigned long IOCTL_MODE_GETRESOURCES = DS_DRM_IOWR(0xA0, mode_card_res);
constexpr unsigned long IOCTL_MODE_GETCRTC      = DS_DRM_IOWR(0xA1, mode_crtc);
constexpr unsigned long IOCTL_MODE_SETCRTC      = DS_DRM_IOWR(0xA2, mode_crtc);
constexpr unsigned long IOCTL_MODE_GETENCODER   = DS_DRM_IOWR(0xA6, mode_get_encoder);
constexpr unsigned long IOCTL_MODE_GETCONNECTOR = DS_DRM_IOWR(0xA7, mode_get_connector);
constexpr unsigned long IOCTL_MODE_RMFB         = DS_DRM_IOWR(0xAF, unsigned int);
constexpr unsigned long IOCTL_MODE_PAGE_FLIP    = DS_DRM_IOWR(0xB0, mode_crtc_page_flip);
constexpr unsigned long IOCTL_MODE_ADDFB2       = DS_DRM_IOWR(0xB8, mode_fb_cmd2);

#undef DS_DRM_IOWR

} // namespace ds::sdl::drmu
