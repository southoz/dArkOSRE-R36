// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <string>
#include <vector>

namespace ds::sdl {

// Pause menu settings, as data tables: the menu knows nothing about what any
// row means. Deliberately a subset of the config file -- one-time, debug-only,
// or invisible-effect keys stay ini-only.

// An ini value and what the player reads ("mean" -> DEFAULT, "linear" -> BILINEAR).
struct Choice { const char* value; const char* label; };

enum Flag : u8 {
  FlagLive    = 0,        // takes effect as soon as it is set
  FlagDeferred = 1u << 0, // applied when the menu closes, not per-step (layout changes)
  FlagRestart = 1u << 1,  // only read at startup; the row says so
};

// Why a row might be switched off; the menu asks the host rather than
// resolving these itself since some depend on live state, not just another key.
enum class Dep : u8 {
  None,
  FrameskipMode,      // emu.frameskip > 0: adaptive-or-fixed means nothing at 0
  PanelEffects,       // the tier has panel pixels at all (not the display engine)
  GridSeam,           // PanelEffects, and video.linear off, which overrides them
  Chunky,             // video.linear off (chunky survives at DS resolution)
  ChunkyCell,         // Chunky, and video.chunky not off
  Windowed,           // the tier does not own the panel outright
  OneWindow,          // not dual-window: two panels show one screen each, so there is no layout
  Pip,                // the layout is pip
  PipTouchHold,       // Pip, and the inset is not fully opaque (it is a fade timer)
  Dominant,           // the layout is dominant_v or dominant_h
  DominantThreshold,  // Dominant, and dominant_ratio is auto
  Net,                // the build has the Wi-Fi transports (DSPERATE_NET)
  NetInternet,        // net.mode is internet: the DNS choice means nothing otherwise
  // No network session is up (not just none requested): frameskip, fast
  // forward, speed and limiter are meaningless once pacing is controlled by
  // a peer or server instead of the emulator.
  NetSession,
  ShortcutsPath,      // a folder for NAND title shortcuts: paths.dsi_games, else paths.games
};

struct Setting {
  const char* key;                 // "video.linear"
  const char* label;               // "BILINEAR"
  enum class Type : u8 { Bool, Pick, Int, Percent, Text } type;   // Text: `lo` is max chars kept
  const Choice* choices; u8 nchoices;
  // Int/Percent: menu bounds, not the file's -- a value outside this range is
  // shown as-is and left alone until the row is moved.
  int lo, hi, step;
  const char* sentinel_value;      // value below `lo` meaning something else, e.g. "auto"/"0"
  const char* sentinel_label;      // shown for the sentinel, e.g. "AUTO"/"UNLIMITED"
  const char* suffix;              // appended when shown, e.g. "x" on FAST FORWARD SPEED
  const char* def;                 // used when the key is absent; must match what the frontend actually runs with
  u8 flags;
  Dep depends;
  const char* note;                // one line, shown under the list
};

// What the menu needs from the frontend. Implemented in main.cpp, keeping
// menu.cpp free of the NDS/Display/config-file types.
struct SettingsHost {
  virtual ~SettingsHost() = default;
  virtual std::string get(const char* key) const = 0;   // "" if unset; caller falls back to table default
  virtual void set(const char* key, const std::string& value) = 0;
  virtual bool enabled(const Setting& s) const = 0;
  virtual const char* disabled_reason(const Setting& s) const = 0;   // "" when enabled
  // Whether a choice is offered at all, e.g. the display-engine tier can only
  // do the mean of chunky cells, so the others aren't shown rather than shown and ignored.
  virtual bool value_allowed(const Setting& s, const char* value) const = 0;
  // Apply everything deferred; called once when the menu closes, not per-step,
  // to avoid flickering the display on each intermediate value.
  virtual void commit() = 0;
  virtual bool save_per_game() const = 0;
  virtual void set_save_per_game(bool on) = 0;
  virtual bool has_game() const = 0;   // false: no per-game file to save to

  // The Controls page. Bindings have no range/choices; the value comes from
  // a key press rather than stepping through possibilities. `pad` picks the
  // column, defaulting to it when a controller is plugged in.
  struct Binding { std::string key, label, value; };
  virtual int binding_count(bool pad) const = 0;
  virtual Binding binding(bool pad, int i) const = 0;
  virtual bool has_pad() const = 0;
  virtual void begin_capture(bool pad) = 0;   // frontend swallows the next input rather than acting on it
  virtual void cancel_capture() = 0;
  virtual bool capturing() const = 0;
  virtual std::string take_capture() = 0;   // non-empty once something was pressed; ends the capture
  virtual void bind(const std::string& key, const std::string& value) = 0;
  virtual void reset_bindings(bool pad) = 0;
  virtual std::vector<std::string> collisions() const = 0;   // shadowing bindings, one line each

  // Where DS Options page changes are saved: [user] in the config for
  // generated firmware, or the dump's sidecar for a real one. Null if nothing to say.
  virtual const char* user_settings_note() const = 0;
};

// Layout page hotkey-cycle rows are one checkbox per Display::Mode, keyed
// "video.layout_cycle.<mode>", backed by the ini's single `video.layout_cycle`
// list. The host refuses to remove the last mode.
constexpr const char* kLayoutCyclePrefix = "video.layout_cycle.";

// The pages. Each is terminated by a row with a null key.
extern const Setting kEmuSettings[];
extern const Setting kVideoSettings[];
extern const Setting kLayoutSettings[];
extern const Setting kUserSettings[];
int settings_count(const Setting* table);

std::string display_value(const Setting& s, const std::string& value);
// Value one step in `dir`: clamped at the ends for numbers, wraps for choices.
// Skips choices the host disallows.
std::string step_value(const Setting& s, const std::string& value, int dir, const SettingsHost& host);
std::string default_value(const Setting& s);

} // namespace ds::sdl
