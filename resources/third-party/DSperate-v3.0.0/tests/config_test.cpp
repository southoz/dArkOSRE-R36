// SPDX-License-Identifier: GPL-3.0-or-later
// The settings file: parsing, and the single-key rewrite the pause menu and
// the layout hotkeys use to remember a change.
#include "frontend/sdl/config.h"
#include "check.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

const char* kPath = "config_test.ini";

void write(const std::string& text) {
  std::ofstream f(kPath);
  f << text;
}

std::string read() {
  std::ifstream f(kPath);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool has_line(const std::string& text, const std::string& want) {
  std::istringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) if (line == want) return true;
  return false;
}

void test_parsing() {
  write("[emu]\n"
        "frameskip = 2\n"
        "# a comment\n"
        "; another\n"
        "autosave = true    # trailing comments are cut\n"
        "empty =\n"
        "[video]\n"
        "  lcd_grid = 0.5  \n"          // surrounding space is trimmed
        "chunky=mean\n"                 // no spaces around the =
        "no equals sign here\n");
  ds::sdl::Config c;
  CHECK(c.load(kPath));
  CHECK(c.num("emu.frameskip", 9) == 2);
  CHECK(c.flag("emu.autosave", false));
  CHECK(c.real("video.lcd_grid", 0.0) == 0.5);
  CHECK(c.str("video.chunky") == "mean");
  // A key with no value falls back for the typed readers, and the section is
  // part of the name: the same key in two sections is two keys.
  CHECK(c.num("emu.empty", 7) == 7);
  CHECK(!c.has("emu.lcd_grid"));
  CHECK(!c.has("emu.no equals sign here"));
  // A missing file is a false, not a crash: the per-game files usually are.
  ds::sdl::Config missing;
  CHECK(!missing.load("config_test_does_not_exist.ini"));
}

void test_flag_spellings() {
  write("[emu]\n"
        "a = 1\nb = yes\nc = on\nd = TRUE\ne = 0\nf = no\ng = off\nh = wibble\n");
  ds::sdl::Config c;
  CHECK(c.load(kPath));
  CHECK(c.flag("emu.a", false) && c.flag("emu.b", false) && c.flag("emu.c", false));
  CHECK(!c.flag("emu.e", true) && !c.flag("emu.f", true) && !c.flag("emu.g", true));
  // Anything unrecognised keeps the default rather than guessing.
  CHECK(c.flag("emu.h", true));
  CHECK(!c.flag("emu.h", false));
  // "TRUE" is not one of the spellings, so it keeps the default too: the file
  // documents lower case, and silently accepting more would hide a typo.
  CHECK(!c.flag("emu.d", false));
}

// The menu writes one key at a time. What is around it has to survive: the
// file is mostly the commented defaults, and losing them would leave the
// player with no documentation.
void test_store_keeps_the_file() {
  write("# leading comment\n"
        "\n"
        "[emu]\n"
        "# frameskip = 0   the default\n"
        "autosave = false\n"
        "\n"
        "[video]\n"
        "lcd_grid = 0\n");
  CHECK(ds::sdl::Config::store(kPath, "emu.autosave", "true"));
  const std::string after = read();
  CHECK(has_line(after, "# leading comment"));
  CHECK(has_line(after, "# frameskip = 0   the default"));
  CHECK(has_line(after, "autosave = true"));
  CHECK(!has_line(after, "autosave = false"));
  CHECK(has_line(after, "lcd_grid = 0"));
  // It goes back in as the value that was written.
  ds::sdl::Config c;
  CHECK(c.load(kPath));
  CHECK(c.flag("emu.autosave", false));
  CHECK(c.real("video.lcd_grid", 1.0) == 0.0);
}

// A key the file does not have yet joins its section; a section it does not
// have is appended. Both happen the first time a setting is changed.
void test_store_adds_keys_and_sections() {
  write("[emu]\nautosave = false\n\n[video]\nlcd_grid = 0\n");
  CHECK(ds::sdl::Config::store(kPath, "emu.frameskip", "2"));
  CHECK(ds::sdl::Config::store(kPath, "user.nickname", "Ada"));
  const std::string after = read();
  CHECK(has_line(after, "frameskip = 2"));
  CHECK(has_line(after, "[user]"));
  CHECK(has_line(after, "nickname = Ada"));
  ds::sdl::Config c;
  CHECK(c.load(kPath));
  CHECK(c.num("emu.frameskip", 0) == 2);
  CHECK(c.str("user.nickname") == "Ada");
  // The new key landed in [emu], not in [video]: a section boundary matters.
  CHECK(!c.has("video.frameskip"));
  // Writing to a file that does not exist creates it.
  std::remove(kPath);
  CHECK(ds::sdl::Config::store(kPath, "video.chunky", "mean"));
  ds::sdl::Config fresh;
  CHECK(fresh.load(kPath));
  CHECK(fresh.str("video.chunky") == "mean");
}

// Every kind of value the menu writes has to come back as itself, or a row
// would show something other than what the emulator is running with.
void test_round_trip() {
  write("[emu]\n[video]\n[user]\n");
  const struct { const char* key; const char* value; } cases[] = {
    {"emu.frameskip", "3"},
    {"emu.ff_speed", "0"},                  // the UNLIMITED sentinel
    {"emu.autosave", "true"},
    {"video.chunky_cell", "auto"},          // the AUTO sentinel
    {"video.dominant_ratio", "auto"},
    {"video.lcd_grid", "0.5"},              // a percent row's double
    {"video.pip_alpha", "1"},
    {"video.pip_scale", "0.33"},
    {"video.seam", "blend_linear"},
    {"user.nickname", "Ada"},
    {"user.message", "Hello there"},        // spaces survive
    {"keys.a", "Right Shift"},
    {"padhotkeys.quit", "mod+start+back"},  // and so do + and mod
  };
  for (const auto& c : cases) CHECK(ds::sdl::Config::store(kPath, c.key, c.value));
  ds::sdl::Config cfg;
  CHECK(cfg.load(kPath));
  for (const auto& c : cases) CHECK(cfg.str(c.key) == c.value);
  // Written twice, the second value wins and the file does not grow a
  // duplicate: the menu rewrites the same key on every press.
  CHECK(ds::sdl::Config::store(kPath, "emu.frameskip", "1"));
  CHECK(ds::sdl::Config::store(kPath, "emu.frameskip", "2"));
  ds::sdl::Config again;
  CHECK(again.load(kPath));
  CHECK(again.num("emu.frameskip", 0) == 2);
  int seen = 0;
  std::istringstream ss(read());
  std::string line;
  while (std::getline(ss, line)) if (line.rfind("frameskip", 0) == 0) ++seen;
  CHECK(seen == 1);
}

// Later loads win, which is how the per-game file overrides the global one
// and the command line overrides both.
void test_layering() {
  write("[emu]\nframeskip = 1\nautosave = false\n");
  ds::sdl::Config c;
  CHECK(c.load(kPath));
  write("[emu]\nframeskip = 3\n");
  CHECK(c.load(kPath));
  CHECK(c.num("emu.frameskip", 0) == 3);   // overridden
  CHECK(!c.flag("emu.autosave", true));    // and what the second file omits is kept
  c.set("emu.frameskip", "0");
  CHECK(c.num("emu.frameskip", 9) == 0);
}

// Keys a release no longer reads (the retired CPU tuning, fast load, timing
// OC and geometry worker) still load: an old file must not stop the emulator.
void test_retired_keys_load() {
  write("[emu]\ncpu_oc = true\ncpu_tuning = underclock\nfast_load = true\ntiming_oc = true\ngx_worker = true\nframeskip = 2\n");
  ds::sdl::Config c;
  CHECK(c.load(kPath));
  CHECK(c.num("emu.frameskip", 0) == 2);
}

} // namespace

int main() {
  test_parsing();
  test_flag_spellings();
  test_store_keeps_the_file();
  test_store_adds_keys_and_sections();
  test_round_trip();
  test_layering();
  test_retired_keys_load();
  std::remove(kPath);
  std::printf("config: ok\n");
  return 0;
}
