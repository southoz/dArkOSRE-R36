// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <map>
#include <string>

namespace ds::sdl {

// INI-style settings ("section.key" -> value, strings typed at point of use).
// Later loads win: global file, then per-game override, then command line.
class Config {
public:
  static std::string dir();                        // config directory, created on demand
  static std::string global_path() { return dir() + "/dsperate.ini"; }
  // Per-game files, in load order (filename wins): games/<GAMECODE>.ini then
  // games/<rom basename>.ini.
  static std::string game_path_code(const char code[4]);
  static std::string game_path_rom(const std::string& rom);

  bool load(const std::string& path);              // merge a file; false if it does not exist
  void set(const std::string& key, const std::string& value) { kv_[key] = value; }
  bool has(const std::string& key) const { return kv_.count(key) != 0; }

  std::string str(const std::string& key, const std::string& def = "") const;
  int    num(const std::string& key, int def) const;
  double real(const std::string& key, double def) const;
  bool   flag(const std::string& key, bool def) const;

  // Rewrites one `key = value` in `path`, adding the section/file if needed.
  static bool store(const std::string& path, const std::string& key, const std::string& value);
  static void write_default(const std::string& path, bool force = false);

private:
  std::map<std::string, std::string> kv_;
};

} // namespace ds::sdl
