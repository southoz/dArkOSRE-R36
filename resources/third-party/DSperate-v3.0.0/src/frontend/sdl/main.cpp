// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// SDL2 frontend: direct boot, both screens stacked, sound, and input from a
// keyboard, a game controller or a touchscreen. Settings come from an INI
// file (config.h) with the command line on top; hotkeys cover what a
// handheld needs (volume, layout, screenshots, save states), and the pause
// key opens a blitted menu over the held frame (menu.h).
#include "core/cpu/timing_mode.h"
#include "core/nds.h"
#include "core/cart/rom_source.h"
#include "core/handoff_stats.h"
#include "core/host_cores.h"
#include "core/cart/zip.h"
#include "core/cart/zip_cache.h"
#include "core/profile.h"
#include "core/frame_report.h"
#include "core/input/input_log.h"
#include "core/state/state.h"
#include "core/io/dsi_nand_persist.h"
#include "core/io/dsi_title_install.h"
#include "core/io/dsi_nand_launch.h"
#include "core/cheat/database.h"
#include "core/cart/miniz/miniz_tdef.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif
#if DSPERATE_CHEEVOS
#include "cheevos/cheevos_client.h"
#include "cheevos/cheevos_hash.h"
#include "cheevos/cheevos_http.h"
#endif
#if DSPERATE_NET
#include "net/lan_mp.h"
#include "net/slirp_driver.h"
#include <arpa/inet.h>
#endif
#include "audio.h"
#include "config.h"
#include "display.h"
#include "input.h"
#include "lid.h"
#include "loader_cart.h"
#include "menu.h"
#include "cpu_gov.h"
#include "pacer.h"
#include "present_thread.h"

#include <dirent.h>
#include <algorithm>
#include "mic_alsa.h"

#include <SDL2/SDL.h>
#include <sched.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <deque>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <thread>
#include <atomic>
#include <ctime>
#include <string>
#include <random>
#include <vector>

// Nearest tag and short commit hash.
extern const char* const kDsperateVersion;
extern const char* const kDsperateCommit;

namespace {

// Gates startup chatter and hotkey echoes; errors, refusals and disk-touching
// confirmations are never gated. DS_VERBOSE=1 enables.
bool verbose() { static const bool v = std::getenv("DS_VERBOSE") != nullptr; return v; }
#define VLOG(...) do { if (verbose()) std::fprintf(stderr, __VA_ARGS__); } while (0)


using namespace ds;

const char* kUsage =
    "usage: dsperate [rom.nds|rom.zip] [--bios9 F --bios7 F --firmware F] [options]\n"
    "  With no ROM (or a file named BootMenu.nds) the console boots its own\n"
    "  firmware: the DS menu, with the clock set from this machine and PictoChat.\n"
    "  A file named BootMenuDSi.nds is --dsi-mode: the DSi Menu off the NAND.\n"
    "  Neither file has to exist -- the name is the whole instruction.\n"
    "  Without dumps a built-in replacement BIOS and a generated firmware run games\n"
    "  (direct boot only; the DS menu and exact timing need the real files).\n"
    "  --config F      settings file (default ~/.config/dsperate/dsperate.ini; every\n"
    "                  option below has a key there; games/<rom name>.ini and games/<CODE>.ini\n"
    "                  override it per game, the filename one winning)\n"
    "  --version       print the release tag and the commit this was built from, and exit\n"
    "  --write-config F  write the default settings file (all keys commented) to F and exit\n"
    "  --dsi-mode      boot a DSi from its NAND (boot2, then the DSi Launcher) instead of the DS menu; no\n"
    "                  ROM. Needs --bios9i F --bios7i F (the DSi BIOS pair) and --dsi-nand F (a nand.bin\n"
    "                  with its nocash footer; paths.dsi_nand), with --bios9/--bios7 as usual and the DSi's\n"
    "                  firmware (--firmware, or paths.dsi_firmware for every DSi session).\n"
    "                  EXPERIMENTAL. The NAND is opened\n"
    "                  read-only; what a session changes is kept as files instead: title saves as\n"
    "                  <GAMECODE>.pub/.prv/.bnr (paths.saves, else beside the dump), system settings in\n"
    "                  <nand>.ovr and photos under <nand>.photos/, put back in at the next boot.\n"
    "                  A DSiWare .nds/.cia given with it is installed into the session (not the dump) unless\n"
    "                  the NAND has it; it needs its signed DSi TMD: embedded in the CIA, cached beside the\n"
    "                  saves, downloaded from Nintendo's update CDN, or --dsi-tmd F (default <game>.tmd).\n"
    "                  --dsi-offline never downloads; --dsi-hide-installed (emu.dsi_hide_installed) hides\n"
    "                  the dump's own DSiWare.\n"
    "                  The title starts straight away; --dsi-menu boots to the DSi Menu with it instead\n"
    "                  With no title, the loader card is in the slot there as on the DS menu: launching\n"
    "                  it raises the game list, and a DS game picked from it runs on a DS\n"
    "                  Without --dsi-nand, a DSiWare .nds/.cia given with it runs with only the DSi BIOS\n"
    "                  pair: the launcher's hand-off is emulated, the NAND and the DSi settings are made\n"
    "                  up from [user], and saves go to paths.saves (else beside the game). The DSi system\n"
    "                  font is DSperate's own (Noto Sans, WenQuanYi Micro Hei); --dsi-font F\n"
    "                  (paths.dsi_font) uses a console's /sys/TWLFontTable.dat instead.\n"
    "                  --dsi-sd DIR (paths.dsi_sd) puts a host folder in the DSi's SD card slot; what the\n"
    "                  DSi changes on the card is written back to the folder (new, changed and removed files)\n"
    "                  A DSiWare .nds/.dsi/.cia named without --dsi-mode runs this way too, and so does one\n"
    "                  picked from the loader's game list (marked [DSi]); the DSi BIOS pair can be set as\n"
    "                  paths.bios9i/paths.bios7i. When the title leaves (a soft reset), the session ends.\n"
    "  --scale N       window scale (default 2)\n"
    "  --fullscreen    start fullscreen\n"
    "  --layout L      vertical (default) | horizontal | single | pip | dominant_v | dominant_h\n"
    "  --screen S      top (default) or bottom: the screen shown alone, large or dominant\n"
    "  --screen-gap N  pixels between the two screens in the stacked, side-by-side and dominant layouts\n"
    "                  (video.screen_gap; any whole number, a negative one overlaps them; auto pushes\n"
    "                  the screens to the edges when the panel leaves room along the pair)\n"
    "  --pip-alpha X   opacity of the PiP inset at rest, 0..1 (default 1; it comes up to opaque\n"
    "                  while the bottom screen is touched)\n"
    "  --dominant-ratio R  the dominant layouts' secondary, relative to the dominant screen: auto\n"
    "                  (default: the dominant screen takes the largest whole scale that leaves the\n"
    "                  secondary at least --dominant-threshold of it, and the secondary the room left)\n"
    "                  or a ratio such as 0.5\n"
    "  --dominant-threshold T  the smallest secondary auto accepts, 0.1..0.99 (default 0.25)\n"
    "  --integer-scale [M]  whole panel pixels per DS pixel: under (default when bare; the largest\n"
    "                  that fits, letterboxed) | over (the smallest that covers, cropped, keeping the\n"
    "                  edge between the screens) | off\n"
    "  --dual-window   one window per video display, one DS screen each (dual-panel\n"
    "                  handhelds; also what direct scanout needs on them)\n"
    "  --linear        bilinear scaling instead of nearest (takes precedence over the grid, seams\n"
    "                  and chunky)\n"
    "  --lcd-grid S    LCD pixel grid strength, 0 (off, default) .. 1 (software scaling, or the\n"
    "                  display engine's overlay layer on the A30)\n"
    "  --seam S        dark (default): the LCD grid, dimmed by --lcd-grid | blend: box-filter seams\n"
    "                  (sharp-shimmerless): the one panel pixel/row that straddles two DS pixels is their\n"
    "                  area-weighted blend, all others crisp | blend_linear: the same in linear light\n"
    "  --chunky [M]    draw each 2x2 block of DS pixels as one cell (with the grid, one seam per\n"
    "                  block); lower resolution, for panels at fractional scales. M: mean (default;\n"
    "                  the area-weighted box) | extreme (the mean, unless the darkest or brightest\n"
    "                  pixel stands more than --chunky-threshold N (0..255, default 180) from it) |\n"
    "                  mode (dominant colour, mean when all differ) | tl (top-left pixel) | min | max\n"
    "  --chunky-cell C panel pixels per chunky cell: auto (default; the smallest of 4..16 that divides the\n"
    "                  screen, 4 on a 640x480 panel = 160x120 cells) | pair (2x2 DS pixels) | N (the\n"
    "                  nearest size at or below N that divides the screen)\n"
    "  --sink S        where frames go: auto (the default: the best this device has) | disp (the\n"
    "                  display engine's scaler layer, Miyoo A30 class) | fbdev (/dev/fb0, the H700\n"
    "                  handhelds' mali-fbdev SDL2) | kms (page flips) | dmabuf (Wayland zero-copy) |\n"
    "                  surface (SDL's window surface) | renderer (SDL_Renderer). One that won't open\n"
    "                  falls back to the renderer. video.sink. --disp / --fbdev are --sink disp / fbdev\n"
    "  --gpu-present / --no-gpu-present  scale and lay out the picture off the CPU, into the\n"
    "                  scanout buffer: the Rockchip RGA where the device has one, else a Vulkan\n"
    "                  compute pass (video.gpu_present = true | rga | vulkan | false; on by default)\n"
    "  --preload-rom / --no-preload-rom  read the whole ROM into memory at load, or page it in as\n"
    "                  the game reads it (cart.preload = auto | true | false; auto preloads a ROM on a\n"
    "                  network share that fits in memory, where a page read mid-frame is a round trip)\n"
    "  --no-audio      run without sound\n"
    "  --volume N      0..100\n"
    "  --audio-buffer X  how much sound is held ahead: auto (the default) or milliseconds;\n"
    "                  audio.buffer_size. Lower is less delay and less slack before a\n"
    "                  late frame is heard as a gap\n"
    "  --no-mic        do not open the microphone (M still fakes one)\n"
    "  --no-vsync      present without waiting for the display refresh\n"
    "  --interp        interpreter instead of the recompiler\n"
    "  --timing M      CPU cycle model: fast (default: constant memory and jump costs, much cheaper)\n"
    "                  | exact (melonDS's per-access model); emu.timing\n"
    "  --aa / --no-aa  3D anti-aliasing (video.aa, off by default): the hardware's edge blend on the\n"
    "                  CPU raster, 4x MSAA on the GPU raster\n"
    "  --gpu3d / --no-gpu3d  the 3D layer drawn on the GPU (video.gpu3d, off by default); the CPU\n"
    "                  raster where there is no Vulkan\n"
    "  --frameskip N   skip drawing up to N frames in N+1 (0 = off); emu.frameskip. Skipping runs\n"
    "                  in whole display periods, so on a game that drives its screens on\n"
    "                  alternate frames the limit counts pairs (DS_DEBUG_SKIP=1 shows the period)\n"
    "  --frameskip-mode M  adaptive (default; skip only while the emulator is behind, up to N)\n"
    "                  | fixed (always skip N of every N+1)\n"
    "  --no-frameskip-capture  do not skip frames that display-capture. Exact, but a game that\n"
    "                  captures every frame -- Pokemon B/W, Golden Sun -- then skips nothing,\n"
    "                  so frameskip does nothing at all on it. On by default (the captured\n"
    "                  VRAM holds the last drawn frame); emu.frameskip_capture\n"
    "  --limiter HZ    the rate the emulator is held to: auto (the console's own 59.8261 Hz, the\n"
    "                  default and the speed a game was written for), 30, 60 (what a player means\n"
    "                  by 60 fps: 0.29 % fast, and in step with a 60 Hz panel), 120, 144, 240, or\n"
    "                  off. emu.limiter\n"
    "  --speed N       run at N percent of that rate (25..400); emu.speed\n"
    "  --pacing M      how the wait for the next frame is spent: auto (default) | sleep | busy.\n"
    "                  A frequency governor that decides by polling how busy the last few\n"
    "                  milliseconds looked reads that sleep as an idle machine and clocks down\n"
    "                  under the emulator (ondemand on ROCKNIX: 4-6x the missed frames and ~20x\n"
    "                  the audio gaps). busy holds the core to the deadline instead, at the price\n"
    "                  of a core that never idles; auto does that only where such a governor is in\n"
    "                  charge. Nothing here changes a system setting. emu.pacing\n"
    "  --frames N      quit after N frames (for repeatable measurements)\n"
    "  --stats-from N  leave the first N frames out of the frame statistics (DS_FRAME_STATS)\n"
    "  --record F      write the played inputs to F (one record per frame)\n"
    "  --replay F      play the inputs in F instead of the controls; quits at its end\n"
    "  --rtc-host      run the clock from this machine even under --replay (INEXACT: a game\n"
    "                  that reads the date no longer replays the same, but the firmware's own\n"
    "                  menu needs a real clock to appear at all)\n"
    "  --netplay       local wireless with whoever is on the LAN: join a session heard within\n"
    "                  2.5 s, else host one (melonDS's LAN protocol; a melonDS can be the other end).\n"
    "                  The menu's NETWORK FEATURES row and net.mode do the same without a flag.\n"
    "  --lan-host NAME host a local-wireless session as NAME; --lan-join ADDR joins the one at ADDR;\n"
    "                  --lan-name NAME is our player name (default: the console's nickname)\n"
    "  --internet      the game reaches the real network through the emulated access point, over a\n"
    "                  user-mode TCP/IP stack (no privileges, works on wlan0). Not with local wireless\n"
    "  --dns WHERE     where the game looks up its servers: wiimmfi (the default: Nintendo's own\n"
    "                  servers were switched off in 2014), host (this machine's resolver), or an address\n"
    "  --load-state F  start from a save state instead of booting the game\n"
    "  --autosave-png F  with emu.autosave, write a PNG of both screens to F beside the auto state\n"
#if DSPERATE_CHEEVOS
    "  --cheevos         RetroAchievements for this run (Casual mode; needs a sign-in)\n"
    "  --no-cheevos      off for this run, whatever the config says\n"
    "  --cheevos-token F  sign in with the token in F instead of a stored sign-in (the format a\n"
    "                  CFW's PPSSPP helper writes: the token alone, with the account name coming\n"
    "                  from cheevos.username or --cheevos-user). Implies --cheevos\n"
    "  --cheevos-user U  the account that token signs in as, instead of cheevos.username\n"
#endif
    "  --autoload      start from the auto slot (emu.autosave's state) when there is one\n"
    "  --no-autoload   ignore it for this run, whatever emu.autoload says\n"
    "  --save F        battery save to start from, instead of <rom>.sav\n"
    "                  (a --replay never writes the save back, so a scene repeats)\n"
    "  --clear-cache   delete every unpacked zipped game (the .dsperate directories beside the\n"
    "                  games and paths.cache) except the one being launched, then run as usual\n";

std::string rom_dir_of(const std::string& rom) {
  const size_t slash = rom.find_last_of('/');
  return slash == std::string::npos ? "." : rom.substr(0, slash);
}

std::string rom_stem(const std::string& rom) {
  const size_t dot = rom.find_last_of('.');
  const size_t slash = rom.find_last_of('/');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return rom;
  return rom.substr(0, dot);
}
std::string base_name(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}
// Lower-cased extension, "" if none.
std::string rom_ext(const std::string& rom) {
  const std::string stem = rom_stem(rom);
  std::string ext = rom.substr(stem.size());
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

// A CIA, loose or inside a zip; read_dsiware unwraps it to the SRL the DSi slot takes.
bool is_cia_container(const std::string& rom) {
  const std::string ext = rom_ext(rom);
  if (ext == ".cia") return true;
  return ext == ".zip" && ds::io::zip_holds_cia(rom);
}

// Four-letter game code from a plain .nds header (offset 0x0C), read before
// cfg setup. Zips aren't peeked; they fall back to filename config, merged by code later.
bool peek_game_code(const std::string& rom, char out[4]) {
  if (rom_ext(rom) != ".nds") return false;
  FILE* f = std::fopen(rom.c_str(), "rb");
  if (!f) return false;
  char code[4] = {};
  const bool ok = std::fseek(f, 0x0C, SEEK_SET) == 0 && std::fread(code, 1, 4, f) == 4;
  std::fclose(f);
  // Must be four printable characters.
  for (int i = 0; ok && i < 4; ++i)
    if (code[i] < 0x20 || code[i] >= 0x7f) return false;
  if (!ok) return false;
  std::memcpy(out, code, 4);
  return true;
}

std::string save_path(const std::string& rom, const std::string& dir) {
  return dir.empty() ? rom_stem(rom) + ".sav" : dir + "/" + base_name(rom_stem(rom)) + ".sav";
}

// video.aa: true / false. The older mode words still read: accurate / enhanced / smooth as on, off as off.
bool aa_on(const std::string& v) {
  return v == "true" || v == "1" || v == "yes" || v == "on" || v == "accurate" || v == "enhanced" || v == "smooth";
}
void apply_aa(NDS& nds, bool on) { nds.gpu3d.renderer().set_aa(on); }

void load_save(NDS& nds, const std::string& path) {
  if (!nds.cart) return;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (!nds.cart->save_type_listed()) VLOG("save: game code not in the save list; the chip is detected on first use\n");
    return;
  }
  std::vector<u8> data;
  u8 buf[65536];
  for (size_t got; (got = std::fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + got);
  std::fclose(f);
  const ds::cart::Cart::SaveLoad r = nds.cart->load_save(data.data(), data.size());
  if (r.fitted) { VLOG("save: loaded %zu bytes from %s\n", data.size(), path.c_str()); return; }
  // First write resizes to the chip's size, which would cut off/pad the
  // player's original: keep it as .bak.
  const std::string bak = path + ".bak";
  if (r.chip_bytes) std::fprintf(stderr, "save: %s is %zu bytes but the game's save chip holds %u; the original is kept as %s\n", path.c_str(), data.size(), r.chip_bytes, bak.c_str());
  else std::fprintf(stderr, "save: %s is %zu bytes, a size no save chip has; ignored, the original is kept as %s\n", path.c_str(), data.size(), bak.c_str());
  if (FILE* e = std::fopen(bak.c_str(), "rb")) { std::fclose(e); return; }   // earlier original wins
  if (FILE* o = std::fopen(bak.c_str(), "wb")) {
    if (std::fwrite(data.data(), 1, data.size(), o) != data.size()) std::fprintf(stderr, "save: cannot write %s\n", bak.c_str());
    std::fclose(o);
  }
}

void write_save(NDS& nds, const std::string& path) {
  if (!nds.cart || nds.cart->sram().empty()) return;
  // Write-then-rename: a power cut mid-write leaves the previous file.
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) { std::fprintf(stderr, "save: cannot write %s\n", tmp.c_str()); return; }
  const bool ok = std::fwrite(nds.cart->sram().data(), 1, nds.cart->sram().size(), f) == nds.cart->sram().size();
  std::fclose(f);
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) { std::fprintf(stderr, "save: cannot write %s\n", path.c_str()); return; }
  nds.cart->clear_sram_dirty();
}

// Both screens into one PNG at the layout's natural size. Reads the
// framebuffers directly, so grid/chunky/seams/scaler layer are excluded.
bool write_png(NDS& nds, const std::string& path, const ds::sdl::Display::Layout& shown) {
  using Disp = ds::sdl::Display;
  int w = 0, h = 0;
  Disp::Layout layout = shown;
  layout.gap = 0; layout.gap_auto = false;   // screen_gap is a panel distance, not part of the game's picture
  Disp::natural_size(layout, 1.0, w, h);
  Disp::View views[Disp::SCREENS];
  Disp::place(layout, w, h, views);
  std::vector<u8> rgba(static_cast<size_t>(w) * h * 4, 0);
  for (int i = 0; i < Disp::SCREENS; ++i) {
    const Disp::View& v = views[i];
    if (!v.shown || v.rect.w <= 0 || v.rect.h <= 0) continue;
    const u32* fb = nds.gpu.framebuffer(v.screen);
    // Inset at its resting opacity (the touch ramp is live-only).
    const u32 alpha = v.direct ? 255 : static_cast<u32>(std::clamp(layout.pip_alpha, 0.0, 1.0) * 255.0 + 0.5);
    for (int y = 0; y < v.rect.h; ++y) {
      const int dy = v.rect.y + y;
      if (dy < 0 || dy >= h) continue;
      const int sy = static_cast<int>(static_cast<long>(y) * ds::SCREEN_H / v.rect.h);
      u8* dst = rgba.data() + (static_cast<size_t>(dy) * w + v.rect.x) * 4;
      for (int x = 0; x < v.rect.w; ++x) {
        const int dx = v.rect.x + x;
        if (dx < 0 || dx >= w) continue;
        const int sx = static_cast<int>(static_cast<long>(x) * ds::SCREEN_W / v.rect.w);
        u32 c = fb[sy * ds::SCREEN_W + sx];    // ARGB8888
        if (alpha < 255) {
          u32 under = (static_cast<u32>(dst[x * 4 + 0]) << 16) | (static_cast<u32>(dst[x * 4 + 1]) << 8) | dst[x * 4 + 2];
          Disp::blend_row(&under, &c, 1, alpha);
          c = under;
        }
        dst[x * 4 + 0] = (c >> 16) & 0xFF;
        dst[x * 4 + 1] = (c >> 8) & 0xFF;
        dst[x * 4 + 2] = c & 0xFF;
        dst[x * 4 + 3] = 0xFF;
      }
    }
  }
  size_t len = 0;
  void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), w, h, 4, &len, 6 /* zlib default level */, MZ_FALSE);
  if (!png) { std::fprintf(stderr, "png: cannot encode\n"); return false; }
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  const bool ok = f && std::fwrite(png, 1, len, f) == len;
  if (f) std::fclose(f);
  std::free(png);   // miniz allocates with plain malloc (MZ_MALLOC is not overridden)
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) { std::fprintf(stderr, "png: cannot write %s\n", path.c_str()); std::remove(tmp.c_str()); return false; }
  return true;
}

// <GAMECODE>-<timestamp>.png in paths.screenshots (states dir unless set).
void screenshot(NDS& nds, const std::string& dir, const ds::sdl::Display::Layout& layout) {
  ::mkdir(dir.c_str(), 0755);
  char stamp[32];
  const std::time_t now = std::time(nullptr);
  std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
  std::string code(nds.cart ? nds.cart->header().game_code : "NONE", 4);
  const std::string path = dir + "/" + code + "-" + stamp + ".png";
  if (write_png(nds, path, layout)) std::fprintf(stderr, "screenshot: %s\n", path.c_str());
}

std::string state_path(NDS& nds, const std::string& dir, int slot) {
  const std::string code(nds.cart ? nds.cart->header().game_code : "NONE", 4);
  return dir + "/" + code + "." + std::to_string(slot) + ".dss";
}

// ".auto" rather than a number so it can't collide with a player slot and
// refresh_slots() (walks 0..9) leaves it off the menu; reached only via --load-state.
std::string auto_state_path(NDS& nds, const std::string& dir) {
  const std::string code(nds.cart ? nds.cart->header().game_code : "NONE", 4);
  return dir + "/" + code + ".auto.dss";
}

// Auto slot as the starting point when emu.autoload is set and it exists.
// Left in place: a SIGKILL can't overwrite it, a clean exit does anyway.
std::string autoload_path(NDS& nds, const std::string& dir, bool enabled) {
  // Never for cart-less boot: auto_state_path() keys on "NONE".
  if (!enabled || !nds.cart) return {};
  const std::string path = auto_state_path(nds, dir);
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return {};
  std::fclose(f);
  return path;
}

// Screen layout rides after the machine's chunks. Frontend-only chunk:
// headless never writes it, an older file just ends before it.
void write_layout_chunk(ds::state::Writer& w, const ds::sdl::Display::Layout& l) {
  w.begin("VIEW");
  w.put(static_cast<u8>(l.mode)); w.put(static_cast<u8>(l.primary)); w.put(static_cast<u8>(l.corner));
  w.put(l.pip); w.put(l.dominant);
  w.end();
}

bool read_layout_chunk(ds::state::Reader& r, ds::sdl::Display::Layout& l) {
  using Disp = ds::sdl::Display;
  if (r.at_end() || !r.begin("VIEW")) return false;
  u8 mode = 0, primary = 0, corner = 0; double pip = l.pip, dominant = l.dominant;
  r.fields(mode, primary, corner);
  if (r.more()) r.fields(pip, dominant);
  r.end();
  if (!r.ok() || mode >= static_cast<u8>(Disp::Mode::Count) || primary > 1 || corner >= static_cast<u8>(Disp::Corner::Count)) return false;
  l.mode = static_cast<Disp::Mode>(mode); l.primary = primary; l.corner = static_cast<Disp::Corner>(corner);
  l.pip = std::clamp(pip, 0.1, 0.9); l.dominant = std::clamp(dominant, 0.1, 0.99);
  return true;
}

// Achievement progress, riding after the layout. Needed since Casual mode
// allows save states, so cheevos runtime state must travel with them.
// File-scope hook (not a parameter) so no save/load call site can skip it.
struct CheevosStateHook {
  virtual ~CheevosStateHook() = default;
  // False: nothing to carry, chunk not written.
  virtual bool save(u32& game_id, std::vector<u8>& blob) = 0;
};
CheevosStateHook* g_cheevos_state = nullptr;

// A state loaded before cheevos sign-in/fetch completes is held here and
// applied once the set arrives; also covers an in-session load mid-fetch.
struct PendingCheevosState {
  bool waiting = false;      // state loaded, not yet accounted for
  u32 game_id = 0;
  std::vector<u8> blob;      // empty: state carried no progress, so reset
} g_cheevos_pending;

void write_cheevos_chunk(ds::state::Writer& w) {
  if (!g_cheevos_state) return;
  u32 game_id = 0;
  std::vector<u8> blob;
  if (!g_cheevos_state->save(game_id, blob) || blob.empty()) return;
  w.begin("CHVO");
  w.put(game_id);
  w.vec(blob);
  w.end();
}

void read_cheevos_chunk(ds::state::Reader& r) {
  // No hook check: the session doesn't exist yet at startup read time.
  // No chunk found: still reset the runtime, or it keeps counting stale progress.
  if (r.at_end() || !r.begin("CHVO")) { g_cheevos_pending = {true, 0, {}}; return; }
  u32 game_id = 0;
  std::vector<u8> blob;
  r.fields(game_id);
  r.vec(blob);
  r.end();
  if (!r.ok() || blob.empty()) { g_cheevos_pending = {true, 0, {}}; return; }
  g_cheevos_pending = {true, game_id, std::move(blob)};
}


bool save_state_file(NDS& nds, const std::string& path, const ds::sdl::Display::Layout& layout) {
  ds::state::Writer w; std::string err;
  if (!nds.save_state(w, err)) { std::fprintf(stderr, "state: cannot save: %s\n", err.c_str()); return false; }
  write_layout_chunk(w, layout);
  write_cheevos_chunk(w);
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) { std::fprintf(stderr, "state: cannot write %s\n", tmp.c_str()); return false; }
  const bool ok = std::fwrite(w.data().data(), 1, w.data().size(), f) == w.data().size();
  std::fclose(f);
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) { std::fprintf(stderr, "state: cannot write %s\n", path.c_str()); return false; }
  std::fprintf(stderr, "state: saved %s (%zu KB)\n", path.c_str(), w.data().size() >> 10);
  return true;
}

// Set when a real state was refused (BIOS mismatch, another ROM, older
// format); not for an empty/unreadable slot. Shown on the slot row.
std::string g_state_refused;

// False if unusable, machine untouched; caller must reset if this fails
// after the load began. `layout` is set only if the state carries one.
bool load_state_file(NDS& nds, const std::string& path, ds::sdl::Display::Layout& layout, bool& layout_loaded) {
  layout_loaded = false;
  std::vector<u8> bytes;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    if (n > 0) { bytes.resize(static_cast<size_t>(n)); if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear(); }
    std::fclose(f);
  }
  if (bytes.empty()) { std::fprintf(stderr, "state: no state in slot (%s)\n", path.c_str()); return false; }
  g_state_refused.clear();
  ds::state::Reader r(bytes.data(), bytes.size());
  std::string err;
  if (!nds.load_state(r, err)) {
    std::fprintf(stderr, "state: cannot load %s: %s\n", path.c_str(), err.c_str());
    g_state_refused = "REJECTED";
    return false;
  }
  layout_loaded = read_layout_chunk(r, layout);
  read_cheevos_chunk(r);
  std::fprintf(stderr, "state: loaded %s (frame %llu%s)\n", path.c_str(), static_cast<unsigned long long>(nds.frame_count),
               layout_loaded ? (std::string(", layout ") + ds::sdl::Display::mode_name(layout.mode)).c_str() : "");
  return true;
}

// Game library for the loader cart's picker. Row title is the filename
// without extension (the ROM header's 12-byte title is often cryptic).
// paths.dsi_games; paths.dsi_shortcuts is its old name.
std::string dsi_games_dir(const ds::sdl::Config& cfg) {
  const std::string d = cfg.str("paths.dsi_games");
  return d.empty() ? cfg.str("paths.dsi_shortcuts") : d;
}

// NAND title shortcuts: paths.dsi_games, or the games folder if unset.
std::string shortcuts_dir(const ds::sdl::Config& cfg) {
  const std::string d = dsi_games_dir(cfg);
  return d.empty() ? cfg.str("paths.games") : d;
}

std::vector<ds::sdl::Menu::GameEntry> enumerate_games(const std::string& dir) {
  std::vector<ds::sdl::Menu::GameEntry> games;
  if (dir.empty()) return games;
  DIR* d = opendir(dir.c_str());
  if (!d) { std::fprintf(stderr, "games: cannot read %s\n", dir.c_str()); return games; }
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name.empty() || name[0] == '.') continue;
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) continue;
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != "nds" && ext != "zip" && ext != "dsi" && ext != "cia") continue;
    const std::string path = dir + "/" + name;
    // NAND title shortcut: installed on paths.dsi_nand, started without the DSi Menu.
    if (ds::io::is_shortcut_name(name)) {
      games.push_back({"[DSi] " + name.substr(0, name.size() - std::strlen(ds::io::kShortcutSuffix)), path});
      continue;
    }
    const bool dsi = ds::io::file_is_dsiware(path);
    if (!dsi && ext != "nds" && ext != "zip") continue;
    games.push_back({dsi ? "[DSi] " + rom_stem(name) : rom_stem(name), path});
  }
  closedir(d);
  // Sort by name, [DSi] label aside, so DSiWare titles sit among the rest.
  auto key = [](const std::string& t) { return t.compare(0, 6, "[DSi] ") == 0 ? t.substr(6) : t; };
  std::sort(games.begin(), games.end(), [&](const ds::sdl::Menu::GameEntry& a, const ds::sdl::Menu::GameEntry& b) {
    return key(a.title) < key(b.title);
  });
  return games;
}

// Games folder plus the shortcut folder's titles when that's elsewhere.
std::vector<ds::sdl::Menu::GameEntry> enumerate_library(const ds::sdl::Config& cfg) {
  std::vector<ds::sdl::Menu::GameEntry> games = enumerate_games(cfg.str("paths.games"));
  const std::string sc = dsi_games_dir(cfg);
  if (sc.empty() || sc == cfg.str("paths.games")) return games;
  for (ds::sdl::Menu::GameEntry& g : enumerate_games(sc))
    if (ds::io::is_shortcut_name(g.path)) games.push_back(std::move(g));
  auto key = [](const std::string& t) { return t.compare(0, 6, "[DSi] ") == 0 ? t.substr(6) : t; };
  std::stable_sort(games.begin(), games.end(), [&](const ds::sdl::Menu::GameEntry& a, const ds::sdl::Menu::GameEntry& b) {
    return key(a.title) < key(b.title);
  });
  return games;
}


// Everything keyed to the ROM in the slot: save/states/screenshots/cheats
// paths and the per-game .ini a hotkey writes to. Boot fills this once; the
// loader cart resets the machine and re-opens it without redoing config
// layering (a second per-game .ini merge would accumulate, not replace) --
// only game_ini is re-keyed, so a post-launch hotkey targets the right game.
struct Session {
  std::string rom_path;         // what is in the slot
  std::string rom_dir;          // its directory: the default home for everything below
  std::string game_ini;         // per-game settings; empty when there is no cart
  std::string states_dir;       // save states (and the autosave's PNG)
  std::string shots_dir;        // manual screenshots (F9); paths.screenshots, else states_dir
  std::string sav;              // battery save
  std::string cheats_on_path;   // which cheats are on, one name per line
  ds::cheat::GameCheats cheats; // the database entry for this ROM; the menu points at its groups

  // save_arg is --save: pins the battery save regardless of ROM.
  void open(NDS& nds, const ds::sdl::Config& cfg, const std::string& rom, const char* save_arg);
  void load_enabled(NDS& nds) const;
  void save_enabled(NDS& nds) const;
};

void Session::open(NDS& nds, const ds::sdl::Config& cfg, const std::string& rom, const char* save_arg) {
  rom_path = rom;
  const size_t slash = rom_path.find_last_of('/');
  rom_dir = slash == std::string::npos ? "." : rom_path.substr(0, slash);
  states_dir = cfg.str("paths.states", rom_dir);
  shots_dir = cfg.str("paths.screenshots", states_dir);
  sav = save_arg ? std::string(save_arg) : save_path(rom_path, cfg.str("paths.saves"));
  game_ini.clear();
  if (nds.cart) {
    game_ini = ds::sdl::Config::game_path_rom(rom_path);
    if (game_ini.empty()) game_ini = ds::sdl::Config::game_path_code(nds.cart->header().game_code);
  }

  // usrcheat.dat from paths.cheats, beside the ROM, or the config dir.
  cheats = ds::cheat::GameCheats{};
  nds.cheats.codes.clear();
  cheats_on_path.clear();
  std::string db = cfg.str("paths.cheats");
  // Path may name the file itself or its directory; avoid a double usrcheat.dat.
  if (!db.empty()) {
    auto ends_with_db = [](const std::string& s) {
      static const std::string tail = "usrcheat.dat";
      return s.size() >= tail.size() &&
             s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
    };
    while (db.size() > 1 && (db.back() == '/' || db.back() == '\\')) db.pop_back();
    if (!ends_with_db(db)) {
      std::string joined = db + "/usrcheat.dat";
      if (FILE* f = std::fopen(joined.c_str(), "rb")) { std::fclose(f); db = joined; }
    }
  }
  if (db.empty()) {
    for (const std::string& candidate : {rom_dir + "/usrcheat.dat", ds::sdl::Config::dir() + "/usrcheat.dat"}) {
      if (FILE* f = std::fopen(candidate.c_str(), "rb")) { std::fclose(f); db = candidate; break; }
    }
  }
  if (!db.empty()) {
    std::string err;
    // Header from the loaded cart, not the file at rom_path: a zipped game's
    // file starts with the archive header, so every lookup would miss.
    u8 header[512] = {};
    if (nds.cart) nds.cart->rom_read(0, header, sizeof header);
    if (nds.cart && ds::cheat::load_for_header(db, header, cheats, err)) {
      VLOG("cheats: %s -- %zu codes in %zu groups\n", cheats.name.c_str(), cheats.codes.size(), cheats.groups.size());
      nds.cheats.codes = cheats.codes;
    } else if (!err.empty()) {
      std::fprintf(stderr, "cheats: %s\n", err.c_str());
    }
  }
  // Names, not indices: a database update renumbers; a stale name is dropped.
  if (!cheats.codes.empty())
    cheats_on_path = states_dir + "/" + std::string(nds.cart ? nds.cart->header().game_code : "NONE", 4) + ".cheats";
}

void Session::load_enabled(NDS& nds) const {
  if (cheats_on_path.empty()) return;
  FILE* f = std::fopen(cheats_on_path.c_str(), "rb");
  if (!f) return;
  char line[512];
  size_t on = 0;
  while (std::fgets(line, sizeof line, f)) {
    std::string want(line);
    while (!want.empty() && (want.back() == '\n' || want.back() == '\r')) want.pop_back();
    if (want.empty()) continue;
    for (ds::cheat::Code& c : nds.cheats.codes)
      if (!c.is_note() && c.name == want) { c.enabled = true; ++on; }
  }
  std::fclose(f);
  if (on) VLOG("cheats: %zu enabled from %s\n", on, cheats_on_path.c_str());
}

void Session::save_enabled(NDS& nds) const {
  if (cheats_on_path.empty()) return;
  FILE* f = std::fopen(cheats_on_path.c_str(), "wb");
  if (!f) { std::fprintf(stderr, "cheats: cannot write %s\n", cheats_on_path.c_str()); return; }
  for (const ds::cheat::Code& c : nds.cheats.codes)
    if (c.enabled && !c.is_note()) std::fprintf(f, "%s\n", c.name.c_str());
  std::fclose(f);
}

// The stick-driven pen: an outlined crosshair with a red centre, drawn over
// the bottom screen in DS pixel space and mapped onto the destination.
using CursorDst = ds::sdl::Blit;   // px/pitch/h and the DS-column map; xrun null: 1:1

// DS-sized scratch buffer, the fallback for tiers with no panel-resolution
// surface the CPU may write. ui_scale() is 2 at 256x192.
ds::sdl::Canvas ds_canvas(u32* px) {
  return ds::sdl::Canvas{px, ds::SCREEN_W, static_cast<int>(ds::SCREEN_W), static_cast<int>(ds::SCREEN_H)};
}
void draw_cursor(const CursorDst& d, int cx, int cy, int size) {
  auto fill = [&](int x, int y, u32 colour) {
    if (x < 0 || x > 255 || y < 0 || y > 191) return;
    const ds::sdl::BlitRect r = ds::sdl::blit_rect(d, x, y);
    for (u32 yy = r.y0; yy < r.y1; ++yy) for (u32 xx = r.x0; xx < r.x1; ++xx) d.px[yy * d.pitch + xx] = colour;
  };
  // `size` scales the shape: arms `size` wide, 3*size long, size x size centre.
  auto box = [&](int x0, int y0, int w, int h, u32 colour) { for (int y = y0; y < y0 + h; ++y) for (int x = x0; x < x0 + w; ++x) fill(x, y, colour); };
  const u32 outline = 0xFF000000, line = 0xFFFFFFFF, centre = 0xFFFF2020;
  if (size < 1) size = 1;
  const int arm = 3 * size, half = size / 2;          // the centre box spans [cx-half, cx-half+size)
  const int c0 = -half, c1 = -half + size;            // centre extent relative to cx/cy
  box(cx + c0 - arm - 1, cy + c0 - 1, 2 * arm + size + 2, size + 2, outline);   // horizontal bar outline
  box(cx + c0 - 1, cy + c0 - arm - 1, size + 2, 2 * arm + size + 2, outline);   // vertical bar outline
  box(cx + c0 - arm, cy + c0, 2 * arm + size, size, line);
  box(cx + c0, cy + c0 - arm, size, 2 * arm + size, line);
  box(cx + c0, cy + c0, c1 - c0, c1 - c0, centre);
}

// A small white label (3x5 font: digits, capitals, else space) on a black
// box, anchored to a canvas corner. `right` picks right edge vs left, which
// is how the slot field (top-left) and FPS counter (top-right) stay apart.
ds::sdl::Rect draw_label(const ds::sdl::Canvas& d, const char* text, bool right) {
  static const u8 digits[10][5] = {
    {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
    {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7}};
  static const u8 letters[26][5] = {
    {2,5,7,5,5}, {6,5,6,5,6}, {3,4,4,4,3}, {6,5,5,5,6}, {7,4,6,4,7}, {7,4,6,4,4}, {3,4,5,5,3},
    {5,5,7,5,5}, {7,2,2,2,7}, {1,1,1,5,2}, {5,5,6,5,5}, {4,4,4,4,7}, {5,7,7,5,5}, {6,5,5,5,5},
    {2,5,5,5,2}, {6,5,6,4,4}, {2,5,5,6,3}, {6,5,6,5,5}, {3,4,2,1,6}, {7,2,2,2,2}, {5,5,5,5,7},
    {5,5,5,5,2}, {5,5,7,7,5}, {5,5,2,5,5}, {5,5,2,2,2}, {7,1,2,4,7}};
  static const u8 blank[5] = {0,0,0,0,0};
  auto glyph = [&](char c) -> const u8* {
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= 'a' && c <= 'z') return letters[c - 'a'];
    return blank;
  };
  auto fill = [&](int x, int y, u32 colour) {
    if (x < 0 || x >= d.w || y < 0 || y >= d.h) return;
    d.px[static_cast<size_t>(y) * d.pitch + static_cast<size_t>(x)] = colour;
  };
  int n = 0; while (text[n]) ++n;
  if (n == 0) return {};
  // Same measure the menu uses.
  const int S = ds::sdl::ui_scale(d);
  const int pad = S, margin = 2 * S;
  const int w = n * 3 * S + (n - 1) * S + 2 * pad;   // glyphs, one S-wide gap between each, a border
  const int h = 5 * S + 2 * pad;
  const int X = right ? d.w - margin - w : margin;
  const int Y = margin;
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) fill(X + x, Y + y, 0xFF000000);
  for (int g = 0; g < n; ++g) {
    const u8* f = glyph(text[g]);
    for (int r = 0; r < 5; ++r) for (int c = 0; c < 3; ++c)
      if ((f[r] >> (2 - c)) & 1)
        for (int y = 0; y < S; ++y) for (int x = 0; x < S; ++x)
          fill(X + pad + g * 4 * S + c * S + x, Y + pad + r * S + y, 0xFFFFFFFF);
  }
  return ds::sdl::Rect{X, Y, w, h};
}

// Slot digit or status ("STATE 3 SAVED") shown after a state hotkey.
constexpr int SLOT_OSD_FRAMES = 90;

// White blended over the whole screen at `alpha`, fading over FLASH_FRAMES.
// Drawn into the presented buffer, not the GPU framebuffers, so it's excluded from screenshots.
constexpr int FLASH_FRAMES = 12;
void draw_flash(const ds::sdl::Canvas& d, u32 alpha) {
  if (d.w <= 0 || d.h <= 0) return;
  std::vector<u32> white(static_cast<size_t>(d.w), 0xFFFFFFFFu);
  for (int y = 0; y < d.h; ++y)
    ds::sdl::Display::blend_row(d.px + static_cast<size_t>(y) * d.pitch, white.data(), static_cast<size_t>(d.w), alpha);
}

// A launcher's SIGTERM (or Ctrl-C) must still flush the battery save.
volatile std::sig_atomic_t g_signalled = 0;
void on_signal(int) { g_signalled = 1; }


// [video] settings, re-read/re-applied from the pause menu, not just at boot.
// parse_video() is pure (no SDL); open_displays() does the set_* calls that
// must precede Display::open. The sink plan and dual-window order are
// boot-only: they can force SDL's video driver, fixed before SDL_Init.
struct VideoSetup {
  int    scale = 2;
  bool   fullscreen = false, linear = false, vsync = true, dual_window = false;
  ds::sdl::Display::IntScale int_scale = ds::sdl::Display::IntScale::Off;
  double grid_s = 0.0;      // 0..1 as configured
  u32    grid = 256;        // brightness kept on a seam, 0..256 (256 = off)
  u8     seam_blend = 0;    // 0 dark, 1 blend, 2 blend in linear light
  u8     chunky = 0;
  int    chunky_cell = -1;
  u32    chunky_thresh = 180 * 256;
  ds::sdl::Display::Layout layout;
  std::vector<ds::sdl::Display::Mode> layout_cycle;
  // Boot-only, see above.
  ds::frontend::Sink panel_sink = ds::frontend::Sink::Count;   // Disp/Fbdev when one owns the panel (frontend/video/select.h)
  ds::frontend::Sink want_sink = ds::frontend::Sink::Count;    // explicit video.sink among the window sinks
  bool   gpu_present = false;
  ds::sdl::Display::GpuPresentMode gpu_mode = ds::sdl::Display::GpuPresentMode::Auto;   // video.gpu_present: true (auto: RGA, else Vulkan) | rga | vulkan
  int    upper_display = 0, lower_display = 1;   // dual-window: SDL display per physical panel
};

constexpr const char* kDefaultLayoutCycle = "vertical,horizontal,single,pip,dominant_v,dominant_h";

// "vertical, pip" -> modes in file order, duplicates kept. Unknown name
// fails the parse. `out` may come back empty; caller decides what that means.
bool parse_layout_cycle(const std::string& cyc, std::vector<ds::sdl::Display::Mode>& out) {
  using Disp = ds::sdl::Display;
  out.clear();
  for (size_t at = 0; at <= cyc.size();) {
    size_t end = cyc.find(',', at); if (end == std::string::npos) end = cyc.size();
    std::string name = cyc.substr(at, end - at);
    name.erase(0, name.find_first_not_of(' ')); name.erase(name.find_last_not_of(' ') + 1);
    Disp::Mode m;
    if (!name.empty() && !Disp::parse_mode(name, m)) { std::fprintf(stderr, "unknown layout %s in layout_cycle\n", name.c_str()); return false; }
    if (!name.empty()) out.push_back(m);
    at = end + 1;
  }
  return true;
}

std::string layout_cycle_string(const std::vector<ds::sdl::Display::Mode>& cycle) {
  std::string s;
  for (ds::sdl::Display::Mode m : cycle) { if (!s.empty()) s += ','; s += ds::sdl::Display::mode_name(m); }
  return s;
}

bool parse_video(const ds::sdl::Config& cfg, VideoSetup& vs) {
  using Disp = ds::sdl::Display;
  vs.scale = cfg.num("video.scale", 2);
  if (vs.scale < 1) vs.scale = 1;
  vs.fullscreen = cfg.flag("video.fullscreen", false);
  vs.linear = cfg.flag("video.linear", false);
  vs.vsync = cfg.flag("video.vsync", true);
  vs.dual_window = cfg.flag("video.dual_window", false);
  if (!Disp::parse_int_scale(cfg.str("video.integer_scale", "off"), vs.int_scale)) { std::fprintf(stderr, "integer_scale must be off, under or over\n"); return false; }
  vs.grid_s = std::min(1.0, std::max(0.0, cfg.real("video.lcd_grid", 0.0)));
  vs.grid = static_cast<u32>(std::lround((1.0 - vs.grid_s) * 256.0));
  vs.seam_blend = 0;
  {
    const std::string sm = cfg.str("video.seam", "dark");
    if (sm == "blend") vs.seam_blend = 1; else if (sm == "blend_linear") vs.seam_blend = 2;
    else if (sm != "dark") { std::fprintf(stderr, "unknown seam %s (dark | blend | blend_linear)\n", sm.c_str()); return false; }
    if (vs.linear && (vs.grid_s > 0.0 || vs.seam_blend || cfg.str("video.chunky", "false") != "false"))
      std::fprintf(stderr, "video.linear takes precedence over lcd_grid, seam and chunky\n");
  }
  {
    const std::string c = cfg.str("video.chunky_cell", "auto");
    if (c == "auto") vs.chunky_cell = -1; else if (c == "pair" || c == "2x") vs.chunky_cell = 0;
    else { vs.chunky_cell = std::atoi(c.c_str()); if (vs.chunky_cell < 2 || vs.chunky_cell > 64) { std::fprintf(stderr, "chunky_cell %s: auto | pair | 2..64\n", c.c_str()); return false; } }
  }
  vs.chunky_thresh = static_cast<u32>(std::min(255, std::max(0, cfg.num("video.chunky_threshold", 180)))) * 256;
  vs.chunky = 0;
  {
    const std::string c = cfg.str("video.chunky", "false");
    if (c == "tl") vs.chunky = 1; else if (c == "mean" || c == "true" || c == "1" || c == "yes" || c == "on") vs.chunky = 2;
    else if (c == "min") vs.chunky = 4; else if (c == "max") vs.chunky = 5; else if (c == "mode") vs.chunky = 3;
    else if (c == "extreme") vs.chunky = 6;
    else if (!(c == "false" || c == "0" || c == "no" || c == "off" || c.empty())) { std::fprintf(stderr, "unknown chunky %s (mean | extreme | mode | tl | min | max | false)\n", c.c_str()); return false; }
  }
  vs.layout = Disp::Layout{};
  vs.layout_cycle.clear();
  {
    const std::string l = cfg.str("video.layout", "vertical"), sc = cfg.str("video.screen", "top"), co = cfg.str("video.pip_corner", "br");
    if (!Disp::parse_mode(l, vs.layout.mode)) { std::fprintf(stderr, "unknown layout %s\n", l.c_str()); return false; }
    if (sc == "top") vs.layout.primary = 0; else if (sc == "bottom") vs.layout.primary = 1;
    else { std::fprintf(stderr, "unknown screen %s (top | bottom)\n", sc.c_str()); return false; }
    if (!Disp::parse_corner(co, vs.layout.corner)) { std::fprintf(stderr, "unknown pip_corner %s (tl | tr | bl | br)\n", co.c_str()); return false; }
    if (!parse_layout_cycle(cfg.str("video.layout_cycle", kDefaultLayoutCycle), vs.layout_cycle)) return false;
    if (vs.layout_cycle.empty()) vs.layout_cycle.push_back(vs.layout.mode);
    vs.layout.pip = std::clamp(cfg.real("video.pip_scale", 1.0 / 3.0), 0.1, 0.9);
    vs.layout.pip_alpha = std::clamp(cfg.real("video.pip_alpha", 1.0), 0.0, 1.0);
    vs.layout.gap_auto = cfg.str("video.screen_gap", "0") == "auto";   // the pair pushed to the edges
    vs.layout.gap = vs.layout.gap_auto ? 0 : cfg.num("video.screen_gap", 0);   // menu bounds it; file/CLI take anything, overlap included
    const std::string dr = cfg.str("video.dominant_ratio", "auto");
    vs.layout.dominant_auto = dr == "auto";
    if (!vs.layout.dominant_auto) {
      char* end = nullptr; const double v = std::strtod(dr.c_str(), &end);
      if (end == dr.c_str() || *end) { std::fprintf(stderr, "dominant_ratio must be a number or auto\n"); return false; }
      vs.layout.dominant = std::clamp(v, 0.1, 0.99);
    }
    vs.layout.dominant_min = std::clamp(cfg.real("video.dominant_threshold", 0.25), 0.1, 0.99);
  }
  return true;
}

// Dual-window: both windows present the same way (the frame loop hands the
// scaled targets to both or draws both). One that could not get or keep GPU
// present, e.g. a dmabuf allocation failing on a CMA pool full of page cache,
// takes the other down to scanline scaling with it. False if still mixed.
bool sync_dual_modes(ds::sdl::Display& a, ds::sdl::Display& b) {
  if (a.scaling() == b.scaling()) return true;
  (a.scaling() ? b : a).drop_gpu_present("dropped to match the other window");
  return a.scaling() == b.scaling();
}

// Split from parse_video so a settings change can close the windows and
// come back through here with new values.
bool open_displays(const VideoSetup& vs, ds::sdl::Display& display, ds::sdl::Display& display2) {
  display.set_gpu_present(vs.gpu_present, vs.gpu_mode); display2.set_gpu_present(vs.gpu_present, vs.gpu_mode);
  if (vs.dual_window) {
    display.set_sinks(ds::frontend::Sink::Count, vs.want_sink); display2.set_sinks(ds::frontend::Sink::Count, vs.want_sink);
    display.set_chunky(vs.chunky != 0, vs.chunky_cell); display2.set_chunky(vs.chunky != 0, vs.chunky_cell);
    display.set_grid_strength(vs.linear ? 0.0 : vs.grid_s); display2.set_grid_strength(vs.linear ? 0.0 : vs.grid_s);
    display.set_integer_scale(vs.int_scale); display2.set_integer_scale(vs.int_scale);
    if (!display.open("DSperate", vs.scale, vs.fullscreen, vs.linear, vs.vsync, vs.layout, 0, vs.upper_display) ||
        !display2.open("DSperate (Bottom)", vs.scale, vs.fullscreen, vs.linear, vs.vsync, vs.layout, 1, vs.lower_display)) return false;
    if (!sync_dual_modes(display, display2)) { std::fprintf(stderr, "dual-window: mixed display modes\n"); return false; }
    // MAIN SCREEN picks the DS screen on the upper panel.
    display.set_only_screen(vs.layout.primary);
    display2.set_only_screen(1 - vs.layout.primary);
    return true;
  }
  display.set_chunky(vs.chunky != 0, vs.chunky_cell);
  display.set_sinks(vs.panel_sink, vs.want_sink);
  display.set_integer_scale(vs.int_scale);
  display.set_grid_strength(vs.linear ? 0.0 : vs.grid_s);
  if (!vs.linear) display.set_disp_grid(static_cast<u8>(((256 - vs.grid) * 255) / 256));
  return display.open("DSperate", vs.scale, vs.fullscreen, vs.linear, vs.vsync, vs.layout);
}

// Controls page rows that are neither a DS button nor a hotkey.
struct Extra { const char* key_keys; const char* key_pad; const char* label; };
constexpr Extra kExtras[] = {
  {"hotkeys.modifier", "padhotkeys.modifier", "MODIFIER"},
  {nullptr,            "pad.stylus_button",   "STYLUS TAP"},
  {nullptr,            "pad.stylus_button.alt", "STYLUS TAP (2)"},
  {nullptr,            "pad.stylus_axis",     "STYLUS STICK"},
  {nullptr,            "pad.stylus_dpad",     "STYLUS DPAD"},
  {nullptr,            "pad.stick_dpad",      "STICK DPAD"},
  {nullptr,            "pad.stick_face",      "STICK ABXY"},
  // Axis remap: push the control the positive direction reads (stick
  // right/down, trigger pressed); stores physical axis + inverted flag.
  {nullptr,            "pad.axis_leftx",      "L STICK RIGHT"},
  {nullptr,            "pad.axis_lefty",      "L STICK DOWN"},
  {nullptr,            "pad.axis_rightx",     "R STICK RIGHT"},
  {nullptr,            "pad.axis_righty",     "R STICK DOWN"},
  {nullptr,            "pad.axis_lefttrigger", "L2 AXIS"},
  {nullptr,            "pad.axis_righttrigger", "R2 AXIS"},
};
// Rows holding a stick rather than a single control.
bool extra_is_stick(const char* key) { return std::strcmp(key, "pad.stylus_axis") == 0 || std::strcmp(key, "pad.stick_dpad") == 0 || std::strcmp(key, "pad.stick_face") == 0; }
bool extra_is_axis_remap(const char* key) { return std::strncmp(key, "pad.axis_", 9) == 0; }
// e.g. "-righty" -> physical control + whether inverted.
std::string axis_remap_label(const std::string& v0) {
  if (v0 == "none") return "NONE";
  const bool inv = !v0.empty() && v0[0] == '-';
  const std::string v = !v0.empty() && (v0[0] == '-' || v0[0] == '+') ? v0.substr(1) : v0;
  std::string n = v == "leftx" ? "L STICK X" : v == "lefty" ? "L STICK Y" : v == "rightx" ? "R STICK X" : v == "righty" ? "R STICK Y"
                : v == "lefttrigger" ? "L2" : v == "righttrigger" ? "R2" : v;
  for (char& ch : n) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return inv ? n + " INV" : n;
}
bool extra_in_column(const Extra& e, bool pad) { return pad || e.key_keys; }
int extra_count(bool pad) {
  int n = 0;
  for (const Extra& e : kExtras) n += extra_in_column(e, pad) ? 1 : 0;
  return n;
}
const Extra& extra_at(bool pad, int j) {
  for (const Extra& e : kExtras)
    if (extra_in_column(e, pad) && j-- == 0) return e;
  return kExtras[0];   // unreachable: callers bound j by extra_count()
}
const char* extra_key(const Extra& e, bool pad) { return pad ? e.key_pad : e.key_keys; }

// Same answer Input::configure() would give, one lookup for both display and reset.
const char* extra_default(const char* key, bool pad, const ds::sdl::Config& cfg) {
  if (std::strcmp(key, "pad.stylus_button") == 0) return ds::sdl::Input::stylus_button_default();
  if (std::strcmp(key, "pad.stylus_button.alt") == 0) return "none";   // second binding has no default
  if (std::strcmp(key, "pad.stylus_axis") == 0)   return ds::sdl::Input::stylus_axis_default(cfg);
  if (std::strcmp(key, "pad.stylus_dpad") == 0)   return ds::sdl::Input::stylus_dpad_default();
  if (std::strcmp(key, "pad.stick_face") == 0)    return ds::sdl::Input::stick_face_default();
  if (std::strcmp(key, "pad.stick_dpad") == 0)    return ds::sdl::Input::stick_dpad_default();
  if (extra_is_axis_remap(key))
    for (int i = 0; i < ds::sdl::Input::axis_remap_count(); ++i)
      if (ds::sdl::Input::axis_remap_key(i) == key) {
        static std::string defaults[SDL_CONTROLLER_AXIS_MAX];   // stable storage for returned pointer
        defaults[i] = ds::sdl::Input::axis_remap_default(i);
        return defaults[i].c_str();
      }
  return ds::sdl::Input::mod_default(pad);
}

} // namespace

namespace {
// RESET on a game started from the game list: main() starts the process over.
bool g_restart = false;
}

static int run(int argc, char** argv) {
  const char* rom = nullptr;
  const char* config_arg = nullptr;
  long frame_limit = 0;
  const char *record = nullptr, *replay = nullptr, *save_arg = nullptr, *load_state = nullptr;
  bool clear_cache = false;
  bool rtc_host = false;              // --rtc-host: real clock even under replay
  const char* lan_host = nullptr; const char* lan_join = nullptr; const char* lan_name = "DSperate"; bool netplay = false;
  bool lan_guest = false;             // net.mode = guest: join only, never host
  bool lan_name_set = false;          // --lan-name given, so nickname must not override it
  bool internet = false;              // --internet / net.mode = internet: emulated AP reaches the real network
  const char* dns_arg = nullptr;      // --dns: host, wiimmfi, or an address
  long stats_from = 0;   // frames run but left out of timing statistics
  bool dsi_mode = false;
  const char* bios9i = nullptr; const char* bios7i = nullptr; const char* dsi_nand = nullptr;
  const char* dsi_title = nullptr; const char* dsi_tmd = nullptr; bool dsi_offline = false, dsi_hide_installed = false, dsi_menu = false;
  bool dsi_hle = false;          // --dsi-mode with title and no NAND: launcher hand-off, BIOS pair only
  const char* dsi_font = nullptr;
  const char* dsi_sd = nullptr;
  u32 dsi_title_lo = 0;   // title --dsi-mode was given, once on the NAND: what autoload starts
  std::string dsi_title_cheevos_hash;   // RetroAchievements identity; runs from NAND, no cart to hash later

  // Found before options are read, so an optional-value flag like --chunky
  // [M] doesn't swallow `--chunky game.nds` as its mode.
  const char* rom_word = nullptr;
  auto looks_like_rom = [](const char* w) {
    if (w[0] == '-') return false;
    const std::string ext = rom_ext(w);
    if (ext == ".nds" || ext == ".zip") return true;
    struct stat st;
    return ::stat(w, &st) == 0 && S_ISREG(st.st_mode);
  };
  for (int i = 1; i < argc && !rom_word; ++i) if (looks_like_rom(argv[i])) rom_word = argv[i];
  // Command line is one more settings layer, applied after the files.
  ds::sdl::Config cli;
  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
    auto flag = [&](const char* name) { return !std::strcmp(argv[i], name); };
    // Next word unless it's another option or the game.
    auto optional = [&](const char* fallback) { return i + 1 < argc && argv[i + 1][0] != '-' && argv[i + 1] != rom_word ? argv[++i] : fallback; };
    if (arg("--bios9")) cli.set("paths.bios9", argv[++i]);
    else if (arg("--bios7")) cli.set("paths.bios7", argv[++i]);
    else if (arg("--firmware")) cli.set("paths.firmware", argv[++i]);
    else if (flag("--dsi-mode")) dsi_mode = true;
    else if (arg("--bios9i")) bios9i = argv[++i];
    else if (arg("--bios7i")) bios7i = argv[++i];
    else if (arg("--dsi-nand")) dsi_nand = argv[++i];
    else if (arg("--dsi-tmd")) dsi_tmd = argv[++i];
    else if (arg("--dsi-font")) dsi_font = argv[++i];
    else if (arg("--dsi-sd")) dsi_sd = argv[++i];
    else if (flag("--dsi-offline")) dsi_offline = true;
    else if (flag("--dsi-menu")) dsi_menu = true;
    else if (flag("--dsi-hide-installed")) dsi_hide_installed = true;
    else if (arg("--config")) config_arg = argv[++i];
    else if (arg("--write-config")) { ds::sdl::Config::write_default(argv[++i], true); return 0; }
    else if (arg("--scale")) cli.set("video.scale", argv[++i]);
    else if (flag("--dual-window")) cli.set("video.dual_window", "true");
    else if (arg("--pacing")) cli.set("emu.pacing", argv[++i]);
    else if (arg("--layout")) cli.set("video.layout", argv[++i]);
    else if (arg("--screen")) cli.set("video.screen", argv[++i]);
    else if (arg("--pip-alpha")) cli.set("video.pip_alpha", argv[++i]);
    else if (arg("--screen-gap")) cli.set("video.screen_gap", argv[++i]);
    else if (arg("--dominant-ratio")) cli.set("video.dominant_ratio", argv[++i]);
    else if (arg("--dominant-threshold")) cli.set("video.dominant_threshold", argv[++i]);
    else if (flag("--integer-scale")) cli.set("video.integer_scale", optional("under"));
    else if (arg("--frames")) frame_limit = std::atol(argv[++i]);
    else if (flag("--rtc-host")) rtc_host = true;
    else if (arg("--lan-host")) lan_host = argv[++i];
    else if (arg("--lan-join")) lan_join = argv[++i];
    else if (arg("--lan-name")) { lan_name = argv[++i]; lan_name_set = true; }
    else if (flag("--netplay")) netplay = true;   // join a session heard on the LAN, else host one
    else if (flag("--internet")) internet = true;   // exclusive with local wireless
    else if (arg("--dns")) dns_arg = argv[++i];
    else if (flag("--clear-cache")) clear_cache = true;
    else if (arg("--record")) record = argv[++i];
    else if (arg("--replay")) replay = argv[++i];
    else if (arg("--save")) save_arg = argv[++i];
    else if (arg("--load-state")) load_state = argv[++i];
    else if (arg("--autosave-png")) cli.set("emu.autosave_png", argv[++i]);
#if DSPERATE_CHEEVOS
    else if (flag("--cheevos")) cli.set("cheevos.enabled", "true");
    else if (flag("--no-cheevos")) cli.set("cheevos.enabled", "false");
    // Implies enabled, but a later --no-cheevos still wins (applied in order).
    else if (arg("--cheevos-token")) { cli.set("cheevos.token_file", argv[++i]); cli.set("cheevos.enabled", "true"); }
    else if (arg("--cheevos-user")) cli.set("cheevos.username", argv[++i]);
#endif
    else if (flag("--autoload")) cli.set("emu.autoload", "true");
    else if (flag("--no-autoload")) cli.set("emu.autoload", "false");
    else if (arg("--stats-from")) stats_from = std::atol(argv[++i]);
    else if (flag("--fullscreen")) cli.set("video.fullscreen", "true");
    else if (flag("--linear")) cli.set("video.linear", "true");
    else if (arg("--lcd-grid")) cli.set("video.lcd_grid", argv[++i]);
    else if (flag("--chunky")) cli.set("video.chunky", optional("mean"));
    else if (arg("--chunky-threshold")) cli.set("video.chunky_threshold", argv[++i]);
    else if (arg("--chunky-cell")) cli.set("video.chunky_cell", argv[++i]);
    else if (arg("--seam")) cli.set("video.seam", argv[++i]);
    else if (arg("--sink")) cli.set("video.sink", argv[++i]);
    else if (flag("--disp")) cli.set("video.sink", "disp");
    else if (flag("--no-disp")) cli.set("video.disp", "false");
    else if (flag("--fbdev")) cli.set("video.sink", "fbdev");
    else if (flag("--no-fbdev")) cli.set("video.fbdev", "false");
    else if (flag("--gpu-present")) cli.set("video.gpu_present", "true");
    else if (flag("--no-gpu-present")) cli.set("video.gpu_present", "false");
    else if (flag("--no-audio")) cli.set("audio.enabled", "false");
    else if (arg("--volume")) cli.set("audio.volume", argv[++i]);
    else if (arg("--audio-buffer")) cli.set("audio.buffer_size", argv[++i]);
    else if (arg("--speed")) cli.set("emu.speed", argv[++i]);
    else if (arg("--limiter")) cli.set("emu.limiter", argv[++i]);
    else if (flag("--no-mic")) cli.set("audio.mic", "false");
    else if (flag("--no-vsync")) cli.set("video.vsync", "false");
    else if (flag("--interp")) cli.set("emu.jit", "false");
    else if (arg("--timing")) cli.set("emu.timing", argv[++i]);
    else if (arg("--frameskip")) cli.set("emu.frameskip", argv[++i]);
    else if (arg("--frameskip-mode")) cli.set("emu.frameskip_mode", argv[++i]);
    else if (flag("--frameskip-capture")) cli.set("emu.frameskip_capture", "true");
    else if (flag("--no-frameskip-capture")) cli.set("emu.frameskip_capture", "false");
    else if (flag("--aa")) cli.set("video.aa", "true");
    else if (flag("--no-aa")) cli.set("video.aa", "false");
    else if (flag("--gpu3d")) cli.set("video.gpu3d", "true");
    else if (flag("--preload-rom")) cli.set("cart.preload", "true");
    else if (flag("--no-preload-rom")) cli.set("cart.preload", "false");
    else if (flag("--no-gpu3d")) cli.set("video.gpu3d", "false");
    else if (flag("--version")) { std::printf("DSperate %s (%s)\n", kDsperateVersion, kDsperateCommit); return 0; }
    else if (flag("--help")) { std::fputs(kUsage, stderr); return 0; }
    else if (argv[i][0] == '-' && argv[i][1] == '-') { std::fprintf(stderr, "unknown option %s\n", argv[i]); std::fputs(kUsage, stderr); return 2; }
    else rom = argv[i];
  }
  // BootMenuDSi.nds is --dsi-mode, the way BootMenu.nds is a firmware boot;
  // neither file has to exist. ROM is dropped, becoming the cartless DSi boot.
  const char* dsi_mode_by = "--dsi-mode";
  if (rom && rom_stem(base_name(rom)) == "BootMenuDSi") {
    dsi_mode = true;
    dsi_mode_by = "BootMenuDSi.nds";
    rom = nullptr;
  }
  // DSiWare named without --dsi-mode runs the DSi machine too: as a DS it
  // would not start at all.
  const bool dsi_mode_asked = dsi_mode;   // --dsi-mode itself, not implied by a DSiWare title
  const bool rom_shortcut = rom && ds::io::is_shortcut_name(rom);
  if (!dsi_mode && rom && !dsi_nand && (rom_shortcut || ds::io::file_is_dsiware(rom))) dsi_mode = true;
  ds::sdl::Config cfg;
  const std::string global_ini = config_arg ? std::string(config_arg) : ds::sdl::Config::global_path();
  if (!config_arg) ds::sdl::Config::write_default(global_ini);
  if (!cfg.load(global_ini) && config_arg) { std::fprintf(stderr, "cannot read %s\n", config_arg); return 2; }
  // paths.dsi_nand stands in for --dsi-nand only under --dsi-mode.
  const std::string dsi_nand_cfg = dsi_mode_asked && !dsi_nand ? cfg.str("paths.dsi_nand") : std::string();
  if (!dsi_nand_cfg.empty()) dsi_nand = dsi_nand_cfg.c_str();
  if (!dsi_hide_installed) dsi_hide_installed = cfg.flag("emu.dsi_hide_installed", false);
  const std::string shortcut_nand = dsi_nand ? std::string(dsi_nand) : cfg.str("paths.dsi_nand");
  if (dsi_mode) {
    // A game named with a NAND is DSiWare, installed into the session's NAND.
    dsi_hle = !dsi_nand && rom;
    if (!dsi_nand && !rom) { std::fprintf(stderr, "%s needs --dsi-nand (or paths.dsi_nand in %s) or a DSiWare title\n", dsi_mode_by, global_ini.c_str()); return 2; }
    if (dsi_hle && dsi_menu) { std::fprintf(stderr, "--dsi-menu needs --dsi-nand: without one there is no DSi Menu to boot\n"); return 2; }
    // Without a NAND the title goes in the slot (NDS::prepare_dsi_hle).
    if (!dsi_hle) { dsi_title = rom; rom = nullptr; }
  }
  auto apply_cli = [&] { for (const char* k : {"paths.bios9", "paths.bios7", "paths.firmware", "video.scale", "video.dual_window", "video.layout", "video.screen", "video.pip_alpha", "video.screen_gap", "video.dominant_ratio", "video.dominant_threshold", "video.integer_scale",
                                              "video.fullscreen", "video.linear", "video.lcd_grid", "video.chunky", "video.chunky_threshold", "video.chunky_cell", "video.seam", "video.sink", "video.disp", "video.fbdev", "video.gpu_present", "video.vsync", "audio.enabled", "audio.volume",
                                              "audio.mic", "emu.jit", "emu.timing", "emu.speed", "emu.limiter", "emu.pacing", "audio.buffer_size", "audio.latency_frames", "emu.frameskip", "emu.frameskip_mode", "emu.frameskip_capture", "video.aa", "video.gpu3d", "cart.preload", "emu.autosave_png", "emu.autoload", "cheevos.enabled", "cheevos.token_file", "cheevos.username"}) if (cli.has(k)) cfg.set(k, cli.str(k)); };
  apply_cli();
  const std::string bios9 = cfg.str("paths.bios9"), bios7 = cfg.str("paths.bios7");
  const std::string dsi_fw = cfg.str("paths.dsi_firmware");
  const bool have_dsi_fw = !dsi_fw.empty() && std::filesystem::exists(dsi_fw);
  const std::string fw = dsi_mode && have_dsi_fw && !cli.has("paths.firmware") ? dsi_fw : cfg.str("paths.firmware");
  const std::string dsi_bios9i = bios9i ? std::string(bios9i) : cfg.str("paths.bios9i");
  const std::string dsi_bios7i = bios7i ? std::string(bios7i) : cfg.str("paths.bios7i");
  auto have_dsi_bios = [&] { return !dsi_bios9i.empty() && !dsi_bios7i.empty() && std::filesystem::exists(dsi_bios9i) && std::filesystem::exists(dsi_bios7i); };
  if (dsi_mode && !have_dsi_bios()) {
    std::fprintf(stderr, "%s needs the DSi BIOS pair: --bios9i/--bios7i, or paths.bios9i/paths.bios7i in %s\n",
                 dsi_hle ? (std::string(rom) + " is DSiWare and").c_str() : dsi_mode_by, global_ini.c_str());
    return 2;
  }

  // No ROM (or BootMenu.nds, needn't exist) boots the firmware's own menu.
  // Every per-game path below is derived from this string.
  const bool boot_firmware = !rom || rom_stem(base_name(rom)) == "BootMenu";
  const std::string rom_path = rom ? std::string(rom) : ds::sdl::Config::dir() + "/BootMenu.nds";
  if (boot_firmware) VLOG("no game: booting the firmware\n");

  // Per-game files on top of the global one, CLI on top of both: title ID
  // first, then filename-keyed, so the filename wins. Rerun after the cart
  // loads to cover a zip (game code unreadable until unpacked).
  auto merge_game_config = [&](const char* code) {
    std::vector<std::string> paths;
    if (code) paths.push_back(ds::sdl::Config::game_path_code(code));
    paths.push_back(ds::sdl::Config::game_path_rom(rom_path));
    for (const std::string& p : paths)
      if (!p.empty() && cfg.load(p)) VLOG("config: %s\n", p.c_str());
    apply_cli();
  };
  if (!boot_firmware) {
    char code[4];
    merge_game_config(peek_game_code(rom_path, code) ? code : nullptr);
  }

  // Set before any thread exists so every later thread inherits it. Needs
  // privilege (root, or an rtprio limit); refused quietly otherwise.
  // emu.realtime = off | rr | fifo, emu.rt_priority.
  {
    const std::string rt = cfg.str("emu.realtime", "rr");
    const int prio = cfg.num("emu.rt_priority", 5);
    if (rt == "rr" || rt == "fifo") {
      sched_param sp{}; sp.sched_priority = prio;
      if (sched_setscheduler(0, rt == "rr" ? SCHED_RR : SCHED_FIFO, &sp) != 0)
        VLOG("realtime scheduling (%s %d) not permitted: %s\n", rt.c_str(), prio, std::strerror(errno));
      else { VLOG("realtime scheduling: %s %d\n", rt.c_str(), prio); }
    }
    ds::latch_worker_sched();   // what the video workers run at, whichever thread starts them
  }
  // CPU timing model (core/cpu/timing_mode.h), fixed before anything runs or
  // the recompiler emits its stubs. DS_TIMING wins.
  if (cfg.has("emu.timing") && !std::getenv("DS_TIMING")) ds::g_fast_timing = cfg.str("emu.timing") != "exact";
  VLOG("cpu timing: %s\n", ds::g_fast_timing ? "fast" : "exact");
  if (cfg.num("emu.host_cores", 0) > 0) ds::set_host_cores(static_cast<ds::u32>(cfg.num("emu.host_cores", 0)));   // DS_HOST_CORES still wins
  VLOG("host: %u cores\n", ds::host_cores());
  // Thread layout (host_cores.h): before any worker starts, so each places itself.
  {
    const std::string tl = cfg.str("emu.thread_layout", "auto");
    const bool on = (tl == "on" || tl == "auto") && !std::getenv("DS_NO_THREAD_LAYOUT");
    ds::set_thread_layout(on);
    VLOG("thread layout: %s\n", on ? "pinned" : "free");
  }

  NDS nds;
  if (cfg.has("emu.idle_skip") && !std::getenv("DS_IDLE_SKIP")) nds.sched.set_idle_skip(cfg.str("emu.idle_skip").c_str());   // DS_IDLE_SKIP wins
  ds::bios::UserSettings user;
  user.nickname = cfg.str("user.nickname", user.nickname);
  user.message = cfg.str("user.message", user.message);
  user.birthday_month = static_cast<ds::u8>(cfg.num("user.birthday_month", user.birthday_month));
  user.birthday_day = static_cast<ds::u8>(cfg.num("user.birthday_day", user.birthday_day));
  user.favourite_colour = static_cast<ds::u8>(cfg.num("user.colour", user.favourite_colour));
  user.language = static_cast<ds::u8>(cfg.num("user.language", user.language));
  // net.mode: config only speaks when no --netplay/--lan-* flag already decided.
  std::string lan_name_str = lan_name;
  if (!lan_name_set && !user.nickname.empty()) lan_name_str = user.nickname;
  lan_name = lan_name_str.c_str();
  if (!lan_host && !lan_join && !netplay && !internet) {
    const std::string mode = cfg.str("net.mode", "off");
    if (mode == "auto") netplay = true;
    else if (mode == "host") lan_host = lan_name;
    else if (mode == "guest") lan_guest = true;
    else if (mode == "internet") internet = true;
    else if (mode != "off")
      std::fprintf(stderr, "net.mode: \"%s\" is not off, auto, host, guest or internet\n", mode.c_str());
  }
  if (internet && (lan_host || lan_join || netplay || lan_guest)) {
    std::fprintf(stderr, "net: --internet is not local wireless; pick one\n");
    return 1;
  }
  {
    std::string err;
    if (!nds.load_bios(bios9, bios7, fw, user, &err)) { std::fprintf(stderr, "bios: %s\n", err.c_str()); return 1; }
    if (!nds.firmware_synthetic) VLOG("firmware: %s\n", fw.c_str());
    // A fixed MAC (dump's own, or generated 00:09:BF:11:22:33) collides
    // across instances and breaks PictoChat. Low 3 bytes generated once,
    // kept in config (the only part a DS lets differ; sent as `macadr` identity).
    {
      u32 suffix = static_cast<u32>(cfg.num("net.mac_suffix", -1));
      if (suffix > 0xFFFFFF) {
        std::random_device rd;
        suffix = rd() & 0xFFFFFF;
        if (!ds::sdl::Config::store(global_ini, "net.mac_suffix", std::to_string(suffix)))
          std::fprintf(stderr, "net: could not remember this console's MAC in %s\n", global_ini.c_str());
        cfg.set("net.mac_suffix", std::to_string(suffix));
      }
      nds.set_wifi_mac_suffix(suffix);
    }
  }
  if (!nds.bios_native) std::fprintf(stderr, "bios: %s, using the built-in FreeBIOS (direct boot only; timing is not Nintendo's)\n", bios9.empty() ? "no dumps given" : "dumps not found");
  if (nds.firmware_synthetic) std::fprintf(stderr, "firmware: %s, using a generated one ([user] in %s)\n", fw.empty() ? "no dump given" : "dump not found", global_ini.c_str());
  if (boot_firmware && !nds.can_boot_firmware()) {
    std::fprintf(stderr, "the DS menu needs real dumps: %s%s%s (--bios9/--bios7/--firmware or [paths] in %s)\n",
                 nds.bios_native ? "" : "bios9 and bios7", (!nds.bios_native && nds.firmware_synthetic) ? " and " : "",
                 nds.firmware_synthetic ? "firmware" : "", global_ini.c_str());
    return 2;
  }
  // FreeBIOS lacks the Blowfish key table and SWI behaviour the launcher
  // hand-off needs, so a title on it wedges instead of refusing cleanly.
  if (dsi_mode && !nds.bios_native) {
    std::fprintf(stderr, "a DSi session needs the DS BIOS dumps as well as the DSi pair: %s (--bios9/--bios7 or [paths] in %s).\n"
                 "The built-in FreeBIOS cannot boot one.\n",
                 bios9.empty() ? "no dumps given" : "dumps not found", global_ini.c_str());
    return 2;
  }
  // Opened once; a later DSiWare pick from the game list keeps it.
  const std::string dsi_sd_dir = dsi_sd ? std::string(dsi_sd) : cfg.str("paths.dsi_sd");
  auto open_dsi_sd = [&]() -> bool {
    if (dsi_sd_dir.empty() || nds.dsi_sd.valid()) return true;
    ds::io::SdCard::Report r;
    std::string err;
    if (!nds.dsi_sd.open(dsi_sd_dir, &r, &err)) { std::fprintf(stderr, "dsi sd: %s\n", err.c_str()); return false; }
    VLOG("dsi sd: %s, %d files and %d folders on a %llu MB FAT%d card\n", dsi_sd_dir.c_str(), r.files, r.dirs,
         static_cast<unsigned long long>(nds.dsi_sd.length() >> 20), nds.dsi_sd.fat_bits());
    for (const std::string& n : r.notes) std::fprintf(stderr, "dsi sd: %s\n", n.c_str());
    return true;
  };
  if (dsi_hle) {
    std::string err;
    if (!nds.load_dsi_bios(dsi_bios9i, dsi_bios7i, &err)) { std::fprintf(stderr, "dsi bios: %s\n", err.c_str()); return 1; }
    if (!nds.bios_native_dsi) { std::fprintf(stderr, "dsi bios: %s or %s not found\n", dsi_bios9i.c_str(), dsi_bios7i.c_str()); return 1; }
    if (!open_dsi_sd()) return 1;
    if (rom_shortcut) {
      if (shortcut_nand.empty()) { std::fprintf(stderr, "%s is a NAND title shortcut and needs the NAND: --dsi-nand or paths.dsi_nand in %s\n", rom, global_ini.c_str()); return 1; }
      if (!nds.load_dsi_nand(shortcut_nand, &err)) { std::fprintf(stderr, "dsi nand: %s\n", err.c_str()); return 1; }
    }
    nds.set_dsi(true);
    nds.dsi_hle_launch = true;   // NAND and settings made once the title is in the slot
    std::fprintf(stderr, rom_shortcut ? "console: DSi, a title from its NAND without the DSi Menu (EXPERIMENTAL)\n" : "console: DSi without a NAND (EXPERIMENTAL)\n");
  } else if (dsi_mode) {
    std::string err;
    if (!nds.load_dsi_bios(dsi_bios9i, dsi_bios7i, &err)) { std::fprintf(stderr, "dsi bios: %s\n", err.c_str()); return 1; }
    if (!nds.bios_native_dsi) { std::fprintf(stderr, "dsi bios: %s or %s not found\n", dsi_bios9i.c_str(), dsi_bios7i.c_str()); return 1; }
    if (!open_dsi_sd()) return 1;
    // Opened read-only: writes held in memory until pulled out as files.
    if (!nds.load_dsi_nand(dsi_nand, &err)) { std::fprintf(stderr, "dsi nand: %s\n", err.c_str()); return 1; }
    VLOG("dsi nand: %s (read-only; writes held in memory)\n", dsi_nand);
    const ds::io::NandPersistPaths pp = ds::io::NandPersistPaths::beside(dsi_nand, cfg.str("paths.saves"));
    if (dsi_hide_installed) {
      const int n = ds::io::nand_hide_installed_dsiware(nds.dsi_nand, nds.bus.bios7i.get(), &err);
      if (n < 0) { std::fprintf(stderr, "dsi: hiding the installed titles: %s\n", err.c_str()); return 1; }
      std::fprintf(stderr, "dsi: %d installed titles hidden for this session\n", n);
    }
    // At most one, skipped if the NAND already has it.
    if (dsi_title) {
      std::vector<ds::u8> srl, embedded;
      if (!ds::io::read_dsiware(dsi_title, srl, &err, &embedded)) { std::fprintf(stderr, "dsi: %s\n", err.c_str()); return 2; }
#if DSPERATE_CHEEVOS
      // Only for autoload: with --dsi-menu the player picks what runs.
      if (cfg.flag("cheevos.enabled", false) && !dsi_menu) {
        const auto src = ds::cart::RomSource::from_memory(srl);
        std::string herr;
        if (!src || !ds::cheevos::rom_hash(*src, dsi_title, dsi_title_cheevos_hash, herr, true))
          std::fprintf(stderr, "cheevos: cannot identify %s: %s\n", dsi_title, herr.c_str());
      }
#endif
      const u32 lo = static_cast<u32>(srl[0x230] | (srl[0x231] << 8) | (srl[0x232] << 16) | (srl[0x233] << 24));
      if (ds::io::nand_has_title(nds.dsi_nand, nds.bus.bios7i.get(), lo)) {
        std::fprintf(stderr, "dsi: %.4s: already installed on the NAND\n", reinterpret_cast<const char*>(&srl[0x0C]));
        dsi_title_lo = lo;
      } else {
        std::string beside = dsi_tmd ? std::string(dsi_tmd) : rom_stem(std::string(dsi_title)) + ".tmd";
        const std::string cache = pp.saves_dir + "/" + std::string(reinterpret_cast<const char*>(&srl[0x0C]), 4) + ".tmd";
        ds::io::TmdFetch fetch;
#if DSPERATE_CHEEVOS
        std::string http_err;
        std::shared_ptr<ds::cheevos::Backend> http = dsi_offline ? nullptr : std::shared_ptr<ds::cheevos::Backend>(ds::cheevos::make_curl_backend(http_err));
        if (http) fetch = [http](const std::string& url, std::vector<ds::u8>& body) {
          const ds::cheevos::Response resp = http->perform({url, {}, {}}, "DSperate");
          if (resp.status != 200) return false;
          body.assign(resp.body.begin(), resp.body.end());
          return true;
        };
#endif
        std::vector<std::string> tmd_log;
        const std::vector<ds::u8> tmd = ds::io::find_signed_tmd(srl, embedded, cache, beside, fetch, tmd_log);
        for (const std::string& l : tmd_log) VLOG("dsi: %s\n", l.c_str());
        if (tmd.empty()) {
          for (const std::string& l : tmd_log) std::fprintf(stderr, "dsi: %s\n", l.c_str());
          std::fprintf(stderr, "dsi: no signed DSi TMD for %.4s; the launcher cannot start it without one (put it at %s%s)\n",
                       reinterpret_cast<const char*>(&srl[0x0C]), beside.c_str(), fetch ? "" : ", or allow the download");
          return 2;
        }
        const ds::io::TitleInstall r = ds::io::nand_install_title(nds.dsi_nand, nds.bus.bios7i.get(), srl, tmd);
        std::fprintf(stderr, "dsi: %.4s: %s\n", reinterpret_cast<const char*>(&srl[0x0C]), r.message.c_str());
        if (r.result == ds::io::TitleInstall::Result::Failed) return 1;
        dsi_title_lo = r.title_lo;
      }
    }
    // Earlier sessions' saves/system sidecar/photos go back in before boot
    // reads the NAND. Not under replay, which must start identically each time.
    // mark_state_base lets a save state carry the NAND's sectors from here.
    nds.dsi_nand.mark_state_base();
    if (!replay) {
      const ds::io::NandPersistReport r = ds::io::nand_import(nds.dsi_nand, nds.bus.bios7i.get(), pp);
      if (r.saves || r.system_files || r.photos) std::fprintf(stderr, "dsi: restored %d title saves, %d system files, %d photos\n", r.saves, r.system_files, r.photos);
      for (const std::string& n : r.notes) std::fprintf(stderr, "dsi: %s\n", n.c_str());
    }
    nds.set_dsi(true);
    nds.dsi_nand_boot = true;   // reset() builds the DSi machine, setup_direct_boot() boots the NAND
    std::fprintf(stderr, "console: DSi (EXPERIMENTAL)\n");
  }
  nds.reset();
  // Sidecar beside the dump, not written into it. None in DSi mode.
  std::string fw_override = dsi_mode ? std::string() : cfg.str("paths.firmware_override", fw + ".ovr");
  if (!nds.firmware_synthetic && !fw_override.empty()) {
    std::string err;
    if (!nds.load_firmware_override(fw_override, err)) {
      if (err != "cannot open") std::fprintf(stderr, "firmware settings: %s: %s\n", fw_override.c_str(), err.c_str());
    } else if (err.empty()) {
      VLOG("firmware settings: %s\n", fw_override.c_str());   // loaded cleanly
    } else {
      std::fprintf(stderr, "firmware settings: %s -- warning: %s\n", fw_override.c_str(), err.c_str());
    }
  }
  VideoSetup vs;
  if (!parse_video(cfg, vs)) return 2;
  const u32& grid = vs.grid;
  const u8& seam_blend = vs.seam_blend;
  u8& chunky = vs.chunky;
  const u32& chunky_thresh = vs.chunky_thresh;
  bool audio_on = cfg.flag("audio.enabled", true), mic_on = cfg.flag("audio.mic", true);
  const bool jit = cfg.flag("emu.jit", true);
  const bool& dual_window = vs.dual_window;
  using Disp = ds::sdl::Display;
  using Menu = ds::sdl::Menu;
  Disp::Layout& layout = vs.layout;
  std::vector<Disp::Mode>& layout_cycle = vs.layout_cycle;
  ds::prof::set_level(std::getenv("DS_PROFILE"));
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  // Display-engine tier owns the panel: dummy/offscreen video driver keeps
  // window/events/controllers but draws nothing. Decided before SDL_Init.
  auto go_headless = [](const char* tier) {
    if (std::getenv("SDL_VIDEODRIVER")) return;
    const char* pick = nullptr;
    for (const char* want : {"dummy", "offscreen"}) {
      for (int i = 0; i < SDL_GetNumVideoDrivers() && !pick; ++i) if (!std::strcmp(SDL_GetVideoDriver(i), want)) pick = want;
      if (pick) break;
    }
    if (pick) {
      setenv("SDL_VIDEODRIVER", pick, 1);
      // A headless window never has focus, and SDL drops joystick events
      // without focus; let them through since the pad is the only input.
      SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    } else std::fprintf(stderr, "%s: this SDL2 has no headless video driver; its own driver will also open the panel\n", tier);
  };
  // Where frames go (frontend/video/select.h). The panel-owning sinks are
  // decided here, before SDL_Init, since they point SDL at a headless driver.
  {
    ds::frontend::BootProbe probe;
    probe.sink = cfg.str("video.sink");
    probe.disp = cfg.str("video.disp");
    probe.fbdev = cfg.str("video.fbdev");
    probe.dual_window = dual_window;
    if (const char* vd = std::getenv("SDL_VIDEODRIVER")) probe.sdl_videodriver = vd;
    for (int i = 0; i < SDL_GetNumVideoDrivers(); ++i) if (!std::strcmp(SDL_GetVideoDriver(i), "mali")) probe.sdl_has_mali = true;
    probe.display_env = std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY");
    probe.has_dri = ::access("/dev/dri", F_OK) == 0;
    probe.disp_ok = !dual_window && ds::sdl::DispOut::available();
    probe.fb0_ok = !dual_window && ds::sdl::FbdevOut::available();
    const ds::frontend::BootPlan plan = ds::frontend::plan_boot(probe);
    for (const std::string& n : plan.notes) std::fprintf(stderr, "%s\n", n.c_str());
    vs.panel_sink = plan.panel;
    vs.want_sink = plan.want;
    if (plan.unset_videodriver) unsetenv("SDL_VIDEODRIVER");
    if (plan.headless_driver) go_headless(plan.panel == ds::frontend::Sink::Disp ? "video.sink disp" : "video.sink fbdev");
  }
  if (vs.panel_sink == ds::frontend::Sink::Disp && chunky != 0 && chunky != 2) {
    std::fprintf(stderr, "chunky %s: the display-engine tier draws chunky cells in the scaler as their mean; using mean\n", cfg.str("video.chunky").c_str());
    chunky = 2;
  }

  // GPU present stage, default on. Display falls back to the scanline scaler
  // without dma-buf import, and GpuPresent::open() declines without Vulkan.
  {
    const std::string g = cfg.str("video.gpu_present", "true");
    vs.gpu_mode = g == "rga" ? ds::sdl::Display::GpuPresentMode::Rga : g == "vulkan" ? ds::sdl::Display::GpuPresentMode::Vulkan : ds::sdl::Display::GpuPresentMode::Auto;
    vs.gpu_present = g == "rga" || g == "vulkan" || g == "true" || g == "auto" || g == "on" || g == "1";
  }

  u32 init = SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER;
  if (audio_on || mic_on) init |= SDL_INIT_AUDIO;
  // Prefer SDL's native pipewire backend over Pulse (more daemon/wakeup cost).
  const std::string audio_driver = cfg.str("audio.driver", "pipewire");
  const bool driver_forced = std::getenv("SDL_AUDIODRIVER") != nullptr;
  if ((init & SDL_INIT_AUDIO) && !driver_forced && !audio_driver.empty()) setenv("SDL_AUDIODRIVER", audio_driver.c_str(), 1);
#ifdef SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS
  // Pad a/b/x/y are positions (south/east/west/north) everywhere: bindings
  // and the menu's pips assume it. SDL2 otherwise uses labels on Nintendo pads.
  SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
#endif
  if (SDL_Init(init) != 0) {
    if ((init & SDL_INIT_AUDIO) && !driver_forced && !audio_driver.empty()) {
      unsetenv("SDL_AUDIODRIVER");
      if (SDL_Init(init) == 0) goto sdl_ready;
    }
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    if (!(init & SDL_INIT_AUDIO) || SDL_Init(init & ~SDL_INIT_AUDIO) != 0) return 1;
    audio_on = mic_on = false;   // no audio subsystem: run silent
  }
sdl_ready:

  ds::sdl::Display display;
  ds::sdl::Display display2;   // dual-window: the lower panel's window
  std::vector<ds::sdl::Input::TouchRoute> touch_routes;   // DS_DUAL_SCREENS
  if (dual_window) {
    if (SDL_GetNumVideoDisplays() < 2) { std::fprintf(stderr, "--dual-window needs two video displays\n"); SDL_Quit(); return 1; }
    // Under KMSDRM display 0 is DSI-1, the lower panel; sway orders the other way.
    const char* vd = SDL_GetCurrentVideoDriver();
    const bool kms = vd && !std::strcmp(vd, "KMSDRM");
    vs.upper_display = kms ? 1 : 0;
    vs.lower_display = kms ? 0 : 1;
    if (const char* map = std::getenv("DS_DUAL_SCREENS"); map && *map) {
      std::string err;
      if (!ds::sdl::Input::parse_dual_screens(map, vs.upper_display, vs.lower_display, touch_routes, err)) {
        std::fprintf(stderr, "DS_DUAL_SCREENS: %s\n", err.c_str()); SDL_Quit(); return 1;
      }
      const int nd = SDL_GetNumVideoDisplays();
      if (vs.upper_display >= nd || vs.lower_display >= nd || vs.upper_display == vs.lower_display) {
        std::fprintf(stderr, "DS_DUAL_SCREENS: displays %d/%d, %d available\n", vs.upper_display, vs.lower_display, nd); SDL_Quit(); return 1;
      }
    }
  }
  if (!open_displays(vs, display, display2)) { SDL_Quit(); return 1; }
  // video.gpu3d (DS_GPU3D=1 overrides): the 3D layer on the GPU (Renderer3D::set_gpu).
  // After the displays: the Vulkan device opens the DRM node, and under direct
  // KMS the display must take master first.
  {
    const char* g = std::getenv("DS_GPU3D");
    const bool want = g ? std::atoi(g) != 0 : cfg.flag("video.gpu3d", false);
    nds.gpu3d.renderer().set_aa(aa_on(cfg.str("video.aa", "false")));   // before the raster starts: its MSAA is fixed at creation
    if (want) { std::string why; if (!nds.gpu3d.renderer().set_gpu(true, &why)) std::fprintf(stderr, "gpu3d: unavailable (%s), drawing on the CPU\n", why.c_str()); else std::fprintf(stderr, "gpu3d: on\n"); }
  }
  // Let fullscreen windows take their final size before the ROM loads (see
  // Display::settle_step); bounded, in case no configure comes.
  {
    static const std::vector<u32> blank(ds::SCREEN_W * ds::SCREEN_H, 0xFF000000u);
    const u32* const bfb[2] = {blank.data(), blank.data()};
    const Uint32 until = SDL_GetTicks() + 1000;
    for (;;) {
      bool done = display.settle_step(bfb);
      if (dual_window) { done = display2.settle_step(bfb) && done; sync_dual_modes(display, display2); }
      if (done || SDL_TICKS_PASSED(SDL_GetTicks(), until)) break;
      SDL_Delay(8);
    }
  }
  // Single-screen layout shows one screen (core skips the other's engine);
  // every other layout, and dual-window, shows both.
  auto apply_visibility = [&] {
    const Disp::Layout& l = display.current_layout();
    const bool single = !dual_window && l.mode == Disp::Mode::Single;
    for (int s = 0; s < 2; ++s) nds.gpu.set_screen_visible(s, !single || s == l.primary);
  };
  apply_visibility();

  ds::sdl::Audio audio;
  if (audio_on) audio.open(cfg.flag("audio.native_rate", true));
  audio.set_volume(cfg.num("audio.volume", 100));
  // buffer_size (ms) supersedes latency_frames; the old key is still read
  // when buffer_size is unset.
  auto apply_audio_buffer = [&audio](const std::string& v) {
    if (v.empty() || v == "auto") { audio.set_buffer_auto(); return; }
    audio.set_buffer_ms(std::atof(v.c_str()));
  };
  if (const std::string v = cfg.str("audio.buffer_size"); !v.empty()) apply_audio_buffer(v);
  else if (const int frames = cfg.num("audio.latency_frames", 0); frames > 0)
    audio.set_buffer_ms(frames * ds::sdl::Audio::FRAME_MS);
  else audio.set_buffer_auto();
  // Not during a replay: the log carries the mic.
  ds::sdl::MicAlsa mic_alsa;
  // Opened lazily on first AUX read: on the handhelds the capture PCM
  // shares a DAI with SDL's playback stream, and a failed setup can leave
  // playback stuck. `rejected`: don't poke the same codec twice through SDL.
  bool mic_tried = !(mic_on && !replay);
  auto open_mic = [&] {
    if (mic_tried) return;
    mic_tried = true;
    if (!mic_alsa.open(ds::spu::Spu::SAMPLE_RATE, cfg.str("audio.mic_dev").c_str()) && !mic_alsa.rejected())
      audio.open_capture();
  };

  ds::sdl::Input input;
  input.set_touch_routes(std::move(touch_routes));
  input.configure(cfg);
  input.open_controllers();
  std::vector<u32> menu_fb[2] = {std::vector<u32>(ds::SCREEN_W * ds::SCREEN_H), std::vector<u32>(ds::SCREEN_W * ds::SCREEN_H)};
  {
    const std::string pl = cfg.str("cart.preload", "auto");
    ds::cart::RomSource::set_preload(pl == "true" || pl == "on" || pl == "1" ? ds::cart::RomSource::Preload::On : pl == "false" || pl == "off" || pl == "0" ? ds::cart::RomSource::Preload::Off : ds::cart::RomSource::Preload::Auto);
  }
  nds.rom_cache_dir = cfg.str("paths.cache", "");
  nds.rom_cache_max_bytes = static_cast<u64>(std::max(0, cfg.num("cart.cache_mb", 2048))) << 20;
  const bool cache_session = cfg.str("cart.cache", "keep") == "session";
  // Every unpacked image goes except the one this launch is about to use.
  if (clear_cache) {
    std::string keep;
    { std::ifstream f(rom_path, std::ios::binary); u8 magic[4] = {}; f.read(reinterpret_cast<char*>(magic), 4);
      if (f.gcount() == 4 && ds::cart::is_zip(magic, 4)) keep = ds::cart::zip_cache_path(rom_path); }
    u64 freed = 0;
    std::vector<std::string> dirs = {ds::cart::zip_cache_dir(rom_dir_of(rom_path))};
    if (!nds.rom_cache_dir.empty()) dirs.push_back(nds.rom_cache_dir);
    if (!cfg.str("paths.games").empty()) dirs.push_back(ds::cart::zip_cache_dir(cfg.str("paths.games")));
    for (const std::string& d : dirs) freed += ds::cart::clear_cache(d, keep);
    std::fprintf(stderr, "cache: cleared %llu MB of unpacked games\n", static_cast<unsigned long long>(freed >> 20));
  }
  // `plain`: nearest scaling with no picture effects, for text pages (pause
  // menu, loader notice) where grid/chunky/seams would blur glyphs.
  auto set_scale_targets = [&](const ds::sdl::Display::Target target[2], bool scaled, bool plain = false) {
    const bool at_source = display.effects_at_source();
    for (int i = 0; i < 2; ++i) {
      if (!scaled) { nds.gpu.set_scale_target(i, ds::gpu::Gpu::ScaleTarget{}); continue; }
      ds::gpu::Gpu::ScaleTarget st = plain
          ? ds::gpu::Gpu::ScaleTarget{target[i].px, target[i].pitch, target[i].h, target[i].xrun_plain}
          : ds::gpu::Gpu::ScaleTarget{target[i].px, target[i].pitch, target[i].h, target[i].xrun, at_source || !target[i].grid ? 256u : grid, display.chunky_on(i) ? chunky : static_cast<u8>(0), chunky_thresh,
                                      at_source ? static_cast<u8>(0) : seam_blend, target[i].seam_w,
                                      static_cast<const ds::gpu::Gpu::CellMap*>((dual_window && display2.only_screen() == i ? display2 : display).cell_map(i)),
                                      vs.linear && !at_source, target[i].lin_sx, target[i].lin_wx};
      st.y_lo = target[i].y_lo; st.y_hi = target[i].y_hi;   // crop window (integer overscale)
      nds.gpu.set_scale_target(i, st);
    }
  };
  // Loads a ROM with an "unpacking" notice if it takes more than a moment.
  // Load runs on a worker; this thread pumps events (B/quit cancels) and
  // redraws the notice. Dots advance only on extraction progress.
  auto load_rom_notice = [&](const std::string& path) -> bool {
    std::atomic<bool> cancel{false}, done{false};
    std::atomic<u64> progress{0};
    bool ok = false;
    nds.rom_cancel = &cancel;
    nds.rom_progress = [](void* u, u64 bytes, u64) { static_cast<std::atomic<u64>*>(u)->store(bytes); };
    nds.rom_progress_user = &progress;
    std::thread worker([&] { ok = nds.load_rom(path); done = true; });
    const std::string title = rom_stem(base_name(path));
    const Uint32 start = SDL_GetTicks();
    Uint32 last_change = start, last_step = start;
    u64 last_progress = 0;
    int dots = 0;
    bool shown = false;
    while (!done) {
      SDL_Event e;
      while (SDL_PollEvent(&e)) input.handle(e, display, dual_window ? &display2 : nullptr);
      if (input.quit() || g_signalled || ((input.take_menu_presses() >> ds::io::Io::Button::BTN_B) & 1)) cancel = true;
      const Uint32 now = SDL_GetTicks();
      const u64 p = progress.load();
      if (p != last_progress) {
        last_progress = p; last_change = now;
        if (now - last_step >= 250) { dots = (dots + 1) % 4; last_step = now; }
      }
      if (!shown && now - start < 300) { SDL_Delay(10); continue; }   // a loose or cached game never shows it
      if (!shown) display.set_page(true);
      shown = true;
      const int menu_screen = dual_window ? 0 : display.current_layout().primary;
      const u32* fb[2];
      for (int i = 0; i < 2; ++i) {
        std::memcpy(menu_fb[i].data(), nds.gpu.framebuffer(i), menu_fb[i].size() * 4);
        ds::sdl::dim_framebuffer(menu_fb[i].data(), static_cast<u32>(menu_fb[i].size()));
        fb[i] = menu_fb[i].data();
      }
      char line2[32];
      if (cancel) std::snprintf(line2, sizeof line2, "STOPPING");
      else if (now - last_change > 5000) std::snprintf(line2, sizeof line2, "WAITING ON THE CARD");
      else std::snprintf(line2, sizeof line2, "UNPACKING%.*s", dots, "...");
      const char* const line3 = "FIRST LAUNCH ONLY    B CANCELS";
      // Drawn on the panel's own pixels where writable, at panel resolution.
      const bool on_canvas = display.canvas_capable();
      if (!on_canvas) ds::sdl::draw_notice(ds_canvas(menu_fb[menu_screen].data()), title.c_str(), line2, line3);
      ds::sdl::Display::Target target[2] = {};
      if (dual_window) sync_dual_modes(display, display2);
      bool scaled = display.begin_frame(target);
      if (dual_window) scaled = display2.begin_frame(target) && scaled;
      if (scaled) {
        set_scale_targets(target, true, true);
        for (int i = 0; i < 2; ++i) nds.gpu.scale_image(i, menu_fb[i].data());
        // Before the notice: a PiP inset must not land on top of it.
        display.finish_views();
        if (dual_window) display2.finish_views();
        ds::sdl::Display::CanvasView cv;
        if (on_canvas && display.canvas(cv)) {
          ds::sdl::draw_notice(ds::sdl::Canvas{cv.px, cv.pitch, cv.w, cv.h}, title.c_str(), line2, line3);
          // Whole canvas: nothing else redraws the letterbox around it.
          display.note_canvas_draw_all();
        }
        display.present();
        if (dual_window) display2.present();
        set_scale_targets(target, false);
      } else {
        display.draw(fb);
        if (dual_window) display2.draw(fb);
      }
      SDL_Delay(50);
    }
    worker.join();
    if (shown) display.set_page(false);
    nds.rom_cancel = nullptr; nds.rom_progress = nullptr; nds.rom_progress_user = nullptr;
    if (!ok && cancel) VLOG("unpacking cancelled\n");
    return ok;
  };
  auto discard_session_cache = [&] {
    if (cache_session && !nds.rom_cache_path.empty()) { ds::cart::remove_cached(nds.rom_cache_path); nds.rom_cache_path.clear(); }
  };
  // ROM goes in after the display is open, so a slow unpack shows the notice.
  const bool rom_is_cia = is_cia_container(rom_path);
  auto load_shortcut_title = [&](const std::string& path) -> bool {
    ds::io::NandShortcut sc;
    std::string err;
    if (!ds::io::read_shortcut(path, sc)) { std::fprintf(stderr, "dsi: %s is not a NAND title shortcut\n", path.c_str()); return false; }
    if (!sc.from(nds.dsi_nand)) { std::fprintf(stderr, "dsi: %s was made from another NAND than %s\n", path.c_str(), shortcut_nand.c_str()); return false; }
    if (!nds.load_dsi_nand_title(sc.title_lo, &err)) { std::fprintf(stderr, "dsi: %s: %s\n", path.c_str(), err.c_str()); return false; }
    return true;
  };
  if (dsi_hle && rom_shortcut) {
    if (!load_shortcut_title(rom_path)) { SDL_Quit(); return 1; }
  } else if (dsi_hle && rom_is_cia) {
    std::vector<ds::u8> srl;
    std::string err;
    if (!ds::io::read_dsiware(rom_path, srl, &err) || !nds.load_rom_image(std::move(srl))) { std::fprintf(stderr, "dsi: %s: %s\n", rom_path.c_str(), err.c_str()); SDL_Quit(); return 1; }
  } else if (!boot_firmware && !load_rom_notice(rom_path)) { std::fprintf(stderr, "could not read %s\n", rom_path.c_str()); SDL_Quit(); return 1; }
  auto prepare_dsiware = [&]() -> bool {
    std::string err;
    std::vector<std::string> made;
    nds.dsi_font_path = dsi_font ? std::string(dsi_font) : cfg.str("paths.dsi_font");
    if (!nds.dsi_font_path.empty() && !std::filesystem::exists(nds.dsi_font_path)) {
      std::fprintf(stderr, "dsi: font %s not found; using DSperate's own\n", nds.dsi_font_path.c_str());
      nds.dsi_font_path.clear();
    }
    if (!nds.prepare_dsi_hle(user, &err, &made)) { std::fprintf(stderr, "dsi: %s\n", err.c_str()); return false; }
    for (const std::string& m : made) std::fprintf(stderr, "dsi: %s\n", m.c_str());
    return true;
  };
  if (dsi_hle && !prepare_dsiware()) { SDL_Quit(); return 1; }
  // Firmware boot: BootMenu.nds beside the config wins if present, else a
  // built-in loader cart is assembled in memory. Not while autoloading DSiWare.
  if (boot_firmware && (!dsi_mode || (!dsi_hle && (!dsi_title || dsi_menu)))) {
    if (nds.load_rom(rom_path.c_str())) {
      VLOG("loader cart: %s\n", rom_path.c_str());
    } else if (cfg.flag("loader.card", true) &&
               nds.load_rom_image(ds::sdl::build_loader_cart(cfg.str("loader.title", "Game Menu"),
                                                             cfg.str("loader.subtitle", "Dariragan! Dagozuban!")))) {
      VLOG("loader cart: built in\n");
    } else {
      VLOG("loader cart: none; the slot stays empty\n");
    }
  }
  // Merge again now the game code is known (first time for a zip).
  if (nds.cart) {
    merge_game_config(nds.cart->header().game_code);
    VLOG("game: %.12s [%.4s]\n", nds.cart->header().game_title, nds.cart->header().game_code);
    if (!nds.rom_zip_entry.empty()) VLOG("zip: %s\n", nds.rom_zip_entry.c_str());
  }
  Session session;
  session.open(nds, cfg, rom_path, save_arg);

  cfg.set("video.aa", aa_on(cfg.str("video.aa", "false")) ? "true" : "false");
  apply_aa(nds, aa_on(cfg.str("video.aa", "false")));
  if (!boot_firmware || nds.dsi) nds.setup_direct_boot();   // on a DSi this is the NAND boot
  if (nds.dsi && dsi_title_lo && !dsi_menu) nds.dsi_autoload(dsi_title_lo);
  // Off in the core by default for reproducibility. Not under --replay: a
  // game reading the date would replay differently each time.
  if (!replay || rtc_host) nds.io.start_rtc_clock();
  else VLOG("rtc: frozen for the replay\n");
  if (replay && rtc_host) std::fprintf(stderr, "rtc: --rtc-host over a replay; this run is not reproducible\n");
#if DSPERATE_JIT
  if (jit && !ds::jit::attach(nds, true, true)) return 1;
  // The DS menu boots under per-instruction budget checks: block-granularity
  // overshoot can intermittently stop it booting. Dropped once a game
  // launches; not the DSi Menu, which runs fine without it.
  if (jit && boot_firmware && !nds.dsi) { ds::jit::set_strict(true); VLOG("jit: strict timing for the firmware\n"); }
#else
  (void)jit;
#endif
  // A replay must start from the same battery save every time, so it's loaded read-only.
  load_save(nds, session.sav);
  const bool save_readonly = replay != nullptr;
  if (save_readonly) VLOG("save: read-only for the replay\n");

  ds::input::Log log;
  if (record && replay) { std::fprintf(stderr, "--record and --replay are exclusive\n"); return 2; }
  if (record && !log.open_write(record)) { std::fprintf(stderr, "cannot write %s\n", record); return 1; }
  if (replay) {
    if (!log.open_read(replay)) { std::fprintf(stderr, "cannot read %s\n", replay); return 1; }
    VLOG("replay: %u frames from %s\n", log.frames(), replay);
  }
  // After the battery save (state's SRAM wins) and after the replay log
  // opens (can wind forward to the state's frame). Skipped under
  // replay/recording, since the log runs inputs from boot.
  const std::string autoload = load_state || log.reading() || log.writing()
                                   ? std::string()
                                   : autoload_path(nds, session.states_dir, cfg.flag("emu.autoload", false));
  if (load_state || !autoload.empty()) {
    const char* start_state = load_state ? load_state : autoload.c_str();
    bool got_layout = false;
    // Unreadable --load-state fails the run; an unreadable auto state just boots normally.
    if (!load_state_file(nds, start_state, layout, got_layout)) {
      if (load_state) return 1;
    } else {
      if (!load_state) std::fprintf(stderr, "state: autoloaded %s\n", start_state);
      // The state's view replaces the config's, dual-window aside.
      if (got_layout && !dual_window) { display.set_layout(layout); apply_visibility(); }
      if (log.reading()) { ds::input::Frame f; for (u64 k = 0; k < nds.frame_count && log.read(f); ++k) {} }
    }
  }
  ds::sdl::Lid lid;
  if (!replay) lid.open();
  std::vector<s16> mic, mic_raw, mic_queue;
  // ADC sits well off zero; one-pole DC blocker, ~25 Hz at 32768 Hz.
  // audio.mic_gain scales what remains (default 0.25).
  double dc = 0.0;
  // Noise gate: floor is the slowest-rising rms seen; a frame under
  // audio.mic_gate times it (default 5, 0 = off) is sent as silence.
  const double mic_gate = cfg.real("audio.mic_gate", 5.0);
  double mic_floor = 1e9;
  const double mic_gain = cfg.real("audio.mic_gain", 0.25);
  const size_t mic_per_frame = ds::spu::Spu::SAMPLE_RATE * ds::CYCLES_PER_FRAME / ds::ARM9_CLOCK_HZ;   // 547

  // Wall-clock pacing when there is no audio queue to pace against.
  const double frame_ns = 1e9 * ds::CYCLES_PER_FRAME / ds::ARM9_CLOCK_HZ;
  ds::sdl::Pacer pacer(frame_ns);
  // emu.pacing: a polling governor reads "sleep" as idle and clocks down
  // under us; "busy" holds the core to the deadline instead (idle power
  // cost, read-only on sysfs). "auto" (default) is "busy" exactly when a
  // polling governor is in charge.
  {
    const std::string pacing = cfg.str("emu.pacing", "auto");
    std::string gov;
    const bool polls = ds::sdl::host_governor_polls(&gov);
    const bool busy = pacing == "busy" || (pacing != "sleep" && pacing != "off" && polls);
    pacer.set_busy_wait(busy);
    if (busy) VLOG("pacing: busy-wait to the deadline (governor %s%s)\n",
                   polls ? gov.c_str() : ds::sdl::cpu_governor().c_str(),
                   pacing == "busy" ? ", asked for" : ": it decides by polling load");
    else VLOG("pacing: sleep to the deadline (governor %s)\n", ds::sdl::cpu_governor().c_str());
  }
  // emu.rt_relief (default on): stay under the kernel's RT bandwidth cap
  // (sched_rt_runtime_us/period_us). The kernel's lump reclaim would show as
  // a periodic hitch; this gives it back a frame at a time instead.
  if (cfg.flag("emu.rt_relief", true)) {
    const auto sysctl_num = [](const char* path) -> double {
      std::FILE* f = std::fopen(path, "r");
      if (!f) return 0.0;
      double v = 0.0;
      if (std::fscanf(f, "%lf", &v) != 1) v = 0.0;
      std::fclose(f);
      return v;
    };
    const double runtime_us = sysctl_num("/proc/sys/kernel/sched_rt_runtime_us");
    const double period_us = sysctl_num("/proc/sys/kernel/sched_rt_period_us");
    int policy = 0; sched_param sp{};
    const bool rt = pthread_getschedparam(pthread_self(), &policy, &sp) == 0 && (policy == SCHED_RR || policy == SCHED_FIFO);
    if (!rt || std::getenv("DS_EMU_OTHER")) VLOG("rt relief: not needed, this thread is not real-time\n");
    else if (runtime_us < 0 || period_us <= 0) VLOG("rt relief: not needed, the real-time class is uncapped\n");
    else if (runtime_us >= period_us) VLOG("rt relief: not needed, the cap is not a cap (%.0f of %.0f us)\n", runtime_us, period_us);
    else {
      pacer.set_rt_relief(runtime_us / period_us, period_us * 1000.0);
      VLOG("rt relief: the real-time class is capped at %.1f %% of every %.0f ms; giving the rest back a frame at a time\n",
           100.0 * runtime_us / period_us, period_us / 1000.0);
    }
  }
  // "auto" (default): the console's own 59.8261 Hz. 60 is what a player
  // means by "60 fps" (slightly fast; absorbed by rate control). "off" still
  // presents/plays sound, just doesn't pace. emu.speed multiplies the result.
  int speed_pct = cfg.num("emu.speed", 100);
  std::string limiter_mode = cfg.str("emu.limiter", "auto");
  bool limiter_off = false;
  // Limiter, speed percent and fast forward all write this one scale factor.
  double base_scale = 1.0;
  double frame_budget_ms = frame_ns / 1e6;   // one frame at the rate now in force; adaptive frameskip's budget
  auto apply_limiter = [&] {
    // Anything unrecognised falls back to the console's own rate.
    double hz = 0.0;
    if (limiter_mode == "off" || limiter_mode == "unlimited") { limiter_off = true; }
    else {
      limiter_off = false;
      const double parsed = std::atof(limiter_mode.c_str());
      hz = parsed >= 1.0 && parsed <= 1000.0 ? parsed : 0.0;
    }
    const double period_ns = hz > 0.0 ? 1e9 / hz : frame_ns;
    speed_pct = speed_pct > 0 ? std::clamp(speed_pct, 25, 400) : 100;
    const double scale = speed_pct / 100.0;
    base_scale = frame_ns / period_ns * scale;
    frame_budget_ms = period_ns / 1e6 / scale;
    // Unlimited is the exception: audio keeps the console's rate and the
    // drop path deals with the surplus, as for fast forward.
    audio.set_speed(limiter_off ? 1.0 : base_scale);
    pacer.reset();
  };
  apply_limiter();
  const double ticks_per_ns = static_cast<double>(SDL_GetPerformanceFrequency()) / 1e9;

  const bool show_fps = std::getenv("DS_FPS") != nullptr;
  // Same 60-frame measurement as the DS_FPS log line; holds the last value between measurements.
  bool fps_osd = cfg.flag("video.fps", false);
  int fps_value = 0;                        // last measured, 0..999
  Uint64 fps_mark = SDL_GetPerformanceCounter();
  Uint64 emu_ticks = 0, draw_ticks = 0, wait_ticks = 0, fps_pace_ticks = 0;   // wait: blocked in begin_frame for a free scanout buffer
  u64 frames = 0;
  // Only run_frame()'s slice goes in: present/vsync blocking would peg
  // every frame at the refresh interval and hide the clusters this finds.
  const double ticks_to_ms = 1e3 / static_cast<double>(SDL_GetPerformanceFrequency());
  std::vector<double> frame_ms, work_ms;   // emu slice; emu + present slice
  if (frame_limit > 0) { frame_ms.reserve(static_cast<size_t>(frame_limit)); work_ms.reserve(static_cast<size_t>(frame_limit)); }
  Uint64 pace_ticks = 0, draw_ticks_total = 0;
  bool paused = false;
  // Set once the transport has actually started (not from the flags). While
  // live, gates save states, fast forward, frameskip, speed knobs, and pause.
  bool net_live = false;
  int state_slot = 0;
  // Fast forward: ff_speed floors the limiter (0 = uncapped); ff_skip
  // presents one frame in ff_skip+1, every frame still emulated exactly.
  // Frameskip: emu.frameskip is the limit N. "fixed" always skips that
  // pattern; "adaptive" (default) skips only while behind real time (a debt
  // in ms, paid down by what a skipped frame saves).
  //
  // A skipped frame leaves out line rendering, 3D raster and scaling; the
  // decision reaches the core one frame early since 3D raster for a frame
  // runs during the frame before it (a display-capturing frame is never skipped).
  int fs_limit = cfg.num("emu.frameskip", 0);            // the menu can change these
  bool fs_adaptive = cfg.str("emu.frameskip_mode", "adaptive") != "fixed";
  const bool fs_capture = cfg.flag("emu.frameskip_capture", true);
  nds.gpu.set_frameskip_capture(fs_capture);
  u64 fs_refused = 0;         // skips the core would not take (capture / display FIFO)
  // Skipping runs in blocks of a whole display period: a game that renders
  // one screen per frame and swaps them needs every phase of a period
  // drawn, or the two screens look like they're swapping (stale vs fresh).
  int fs_left = 0;            // frames left in the current block
  bool fs_in_skip = false;    // that block is a skip block
  int fs_blocks = 0;          // skip blocks run back to back, against the limit
  bool fs_period_warned = false;
  int fs_drawn_run = 0;       // drawn frames since the last skipped one
  u64 fs_skipped = 0;         // reported with the frame statistics
  double fs_debt_ms = 0;      // adaptive: how far behind real time we are
  // One state to the unlisted ".auto" slot when the session ends, so a
  // kill/Ctrl-C can be resumed via --load-state. Nothing written while playing.
  bool autosave = cfg.flag("emu.autosave", false);
  // "true": <GAMECODE>.auto.png beside the state; any other value is the
  // file to write. Only taken when the state is written.
  const std::string autosave_png_cfg = cfg.str("emu.autosave_png", "false");
  const bool autosave_png = autosave_png_cfg != "false" && autosave_png_cfg != "0" && !autosave_png_cfg.empty();
  bool ff_toggle = cfg.flag("emu.fast_forward", false);
  int ff_speed = cfg.num("emu.ff_speed", 0), ff_skip = cfg.num("emu.ff_skip", 3);
  bool was_fast = false;
  std::vector<u32> cursor_fb(ds::SCREEN_W * ds::SCREEN_H);   // bottom screen with the pen crosshair
  std::vector<u32> osd_fb(ds::SCREEN_W * ds::SCREEN_H);      // primary screen with the slot digit / FPS counter
  std::vector<u32> flash_fb[2] = {std::vector<u32>(ds::SCREEN_W * ds::SCREEN_H), std::vector<u32>(ds::SCREEN_W * ds::SCREEN_H)};   // both screens under the screenshot flash
  int flash_left = 0;                                        // frames of screenshot flash still to show
  int slot_shown = 0;                                        // frames left to show the slot field
  std::string slot_text;                                     // ... and what it says
  auto show_slot = [&](const std::string& text) { slot_text = text; slot_shown = SLOT_OSD_FRAMES; };
  int pip_touch_hold = std::max(0, cfg.num("video.pip_touch_hold", 60));
  constexpr int PIP_FADE_STEP = 24;                          // ~10 frames rest to opaque
  int pip_alpha = static_cast<int>(layout.pip_alpha * 255.0 + 0.5), pip_hold = 0;
  // The pause menu (menu.h) and the two screen copies it is composited into.
  ds::sdl::Menu menu;
  menu.set_cheats(&nds.cheats.codes, &session.cheats.groups);
  session.load_enabled(nds);
  // DSI NAND LINKS: a .dspr.nds shortcut file per DSiWare title on
  // paths.dsi_nand while on, none while off. Refreshed at every start and
  // when the row is toggled. Reads its own NAND copy; session's untouched.
  auto sync_nand_shortcuts = [&](bool on) -> std::string {
    // A folder paths.dsi_games has since moved away from keeps its
    // shortcuts; only the currently-named folder is updated or cleared.
    const std::string dir = shortcuts_dir(cfg);
    if (dir.empty()) return "NO SHORTCUT FOLDER";
    ds::io::NandImage nand;
    std::vector<ds::u8> b7;
    if (on) {
      if (shortcut_nand.empty() || !std::filesystem::exists(shortcut_nand)) return "NO DSI NAND (PATHS.DSI_NAND)";
      if (!have_dsi_bios()) return "NO DSI BIOS";
      if (!nand.open(shortcut_nand)) return "THE DSI NAND CANNOT BE READ";
      std::ifstream f(dsi_bios7i, std::ios::binary);
      b7.assign(std::istreambuf_iterator<char>(f), {});
      if (b7.size() < 0x10000) return "THE DSI BIOS CANNOT BE READ";
    }
    const ds::io::ShortcutSync r = ds::io::sync_shortcuts(dir, on ? &nand : nullptr, on ? b7.data() : nullptr, on);
    if (r.written || r.removed) std::fprintf(stderr, "dsi shortcuts: %d written, %d removed in %s\n", r.written, r.removed, dir.c_str());
    for (const std::string& n : r.notes) std::fprintf(stderr, "dsi shortcuts: %s\n", n.c_str());
    return std::to_string(r.written) + " ADDED, " + std::to_string(r.removed) + " REMOVED";
  };
  if (cfg.flag("emu.dsi_nand_shortcuts", false)) sync_nand_shortcuts(true);
  std::string shortcuts_note;   // what the last change of the row did, for a toast
  std::vector<ds::sdl::Menu::GameEntry> games =
      boot_firmware && nds.cart ? enumerate_library(cfg) : std::vector<ds::sdl::Menu::GameEntry>{};
  menu.set_games(&games);
  // Armed until a game is launched: after that the cart in the slot is real.
  bool launcher = boot_firmware && nds.cart != nullptr;
  nds.dsi_loader_watch = launcher && nds.dsi;
  if (launcher) VLOG("launcher: %zu games in %s\n", games.size(), cfg.str("paths.games").c_str());
  bool launching = false;       // the card's launch fade is on screen; the list is going up
  bool launch_latched = false;  // ... and it has already been raised once for this fade
  // DS Download Play boots through the same fade-to-white a card launch
  // uses. Tracks whether this console has been in a local-wireless exchange
  // since booting; once associated, the next fade is Download Play's, not a
  // card launch. Cleared on the firmware reboot path below.
  bool mp_ever = false;
  bool menu_dirty = false;      // the menu screens need compositing and presenting again
  Uint32 menu_ms = 0;           // SDL_GetTicks at the menu's last tick
  // Pausing waits for one more presented, unscaled frame: scanline tiers
  // write lines straight to the window surface and never fill fb_.
  bool pause_pending = false;
  // Same deferral, opposite reason: a screenshot wants the unadorned
  // picture, taken from fb_ after one deliberately unscaled frame.
  bool shot_pending = false;
  // Slot kAutoSlot is the auto state, which the slot page lists for deletion.
  auto slot_path = [&](int i) { return i == Menu::kAutoSlot ? auto_state_path(nds, session.states_dir) : state_path(nds, session.states_dir, i); };
  auto refresh_slots = [&] { for (int i = 0; i <= Menu::kAutoSlot; ++i) {
    FILE* f = std::fopen(slot_path(i).c_str(), "rb");
    menu.set_slot_used(i, f != nullptr);
    if (f) std::fclose(f);
  }
  menu.set_slot_notice(g_state_refused.empty() ? nullptr : g_state_refused.c_str()); };
  u32 sram_writes_seen = nds.cart ? nds.cart->sram_writes() : 0;
  u64 sram_quiet_since = 0;
  // DSi equivalent: NAND writes go out as title saves/system sidecar/photos
  // once quiet for two seconds. Without a NAND, no system sidecar (DSi
  // settings remade from [user] each start).
  auto hle_paths = [&](const std::string& game) {
    ds::io::NandPersistPaths p;
    p.saves_dir = cfg.str("paths.saves");
    if (p.saves_dir.empty()) { const std::filesystem::path r(game); p.saves_dir = r.has_parent_path() ? r.parent_path().string() : "."; }
    p.photos_dir = p.saves_dir + "/dsi-photos";
    return p;
  };
  ds::io::NandPersistPaths dsi_paths = rom_shortcut ? ds::io::NandPersistPaths::beside(shortcut_nand, cfg.str("paths.saves"))
                                     : dsi_hle ? hle_paths(rom_path)
                                     : dsi_mode ? ds::io::NandPersistPaths::beside(dsi_nand, cfg.str("paths.saves")) : ds::io::NandPersistPaths{};
  // Earlier saves go back into the made-up NAND; that becomes the export baseline.
  auto import_dsiware_saves = [&] {
    if (replay) return;
    const ds::io::NandPersistReport r = ds::io::nand_import(nds.dsi_nand, nds.bus.bios7i.get(), dsi_paths);
    nds.dsi_nand.mark_baseline();
    if (r.saves || r.photos) std::fprintf(stderr, "dsi: restored %d title saves, %d photos\n", r.saves, r.photos);
    for (const std::string& n : r.notes) std::fprintf(stderr, "dsi: %s\n", n.c_str());
  };
  if (dsi_hle) import_dsiware_saves();
  u64 nand_writes_seen = nds.dsi_nand.writes, nand_quiet_since = 0, nand_exported = nds.dsi_nand.writes;
  auto flush_dsi = [&] {
    if (!dsi_mode || save_readonly || nds.dsi_nand.writes == nand_exported) return;
    nand_exported = nds.dsi_nand.writes;
    const ds::io::NandPersistReport r = ds::io::nand_export(nds.dsi_nand, nds.bus.bios7i.get(), dsi_paths);
    if (r.saves || r.system_files || r.photos) VLOG("dsi: saved %d title saves, %d system files, %d photos\n", r.saves, r.system_files, r.photos);
    for (const std::string& n : r.notes) std::fprintf(stderr, "dsi: %s\n", n.c_str());
  };
  // SD card changes go back to its folder once writes are quiet for two seconds.
  u64 sd_writes_seen = nds.dsi_sd.writes, sd_quiet_since = 0, sd_synced = nds.dsi_sd.writes;
  auto flush_sd = [&] {
    if (save_readonly || !nds.dsi_sd.valid() || nds.dsi_sd.writes == sd_synced) return;
    sd_synced = nds.dsi_sd.writes;
    const ds::io::SdCard::Report r = nds.dsi_sd.sync();
    if (r.files || r.dirs || r.removed) VLOG("dsi sd: %d files written, %d folders made, %d removed in %s\n", r.files, r.dirs, r.removed, nds.dsi_sd.folder().c_str());
    for (const std::string& n : r.notes) std::fprintf(stderr, "dsi sd: %s\n", n.c_str());
  };
  auto flush_save = [&] { if (!save_readonly && nds.cart && nds.cart->sram_dirty()) write_save(nds, session.sav); flush_dsi(); flush_sd(); };
  // Whether fb_ holds the last run frame's picture (true after an unscaled
  // frame; false after a scanline-tier frame written straight to the panel).
  bool fb_current = false;
  auto autosave_now = [&] {
    // net_live: quitting would otherwise leave a state resuming a dead conversation.
    if (!autosave || save_readonly || log.writing() || net_live) return;
    // Thumbnail comes from fb_; run one more unscaled frame if stale.
    if (autosave_png && !fb_current) {
      ds::sdl::Display::Target none[2] = {};
      set_scale_targets(none, false);
      nds.run_frame();
      fb_current = true;
    }
    if (!save_state_file(nds, auto_state_path(nds, session.states_dir), display.current_layout())) return;
    flush_save();   // the .sav and the state never diverge
    if (!autosave_png) return;
    const bool beside = autosave_png_cfg == "true" || autosave_png_cfg == "1";
    std::string png = autosave_png_cfg;
    if (beside) { png = auto_state_path(nds, session.states_dir); png.replace(png.size() - 3, 3, "png"); }
    if (write_png(nds, png, display.current_layout())) std::fprintf(stderr, "state: thumbnail %s\n", png.c_str());
  };
  Disp::Layout loaded_layout = layout; bool got_layout = false;   // from `layout`: a state carries no pip_alpha, the config's stays
  auto apply_loaded_layout = [&] {
    if (!got_layout || dual_window) return;
    loaded_layout.gap = display.current_layout().gap; loaded_layout.gap_auto = display.current_layout().gap_auto;   // not in a state either, and the menu may have moved it since
    display.set_layout(loaded_layout);
    apply_visibility();
    menu_dirty = true;
  };
  // Tier and dual-window panel order are boot decisions, carried over.
  // Layout is carried over too (hotkeys may have moved it). If new settings
  // won't open, old ones are restored; if those fail too, the session ends.
  auto reopen_display = [&]() -> bool {
    VideoSetup want;
    if (!parse_video(cfg, want)) return false;
    want.panel_sink = vs.panel_sink;
    want.want_sink = vs.want_sink;
    want.upper_display = vs.upper_display;
    want.lower_display = vs.lower_display;
    want.layout = display.current_layout();
    if (dual_window) want.layout.primary = vs.layout.primary;   // the windows' own layouts don't track it
    const VideoSetup before = vs;
    display.close();
    if (dual_window) display2.close();
    vs = want;
    if (!open_displays(vs, display, display2)) {
      std::fprintf(stderr, "video: those settings would not open; keeping the old ones\n");
      display.close();
      if (dual_window) display2.close();
      vs = before;
      if (!open_displays(vs, display, display2)) { std::fprintf(stderr, "video: and neither will the old ones\n"); input.request_quit(); }
      return false;
    }
    apply_visibility();
    // A new window forgets a text page is on it, else the menu's glyphs merge.
    display.set_page(paused);
    menu_dirty = true;
    return true;
  };

  // Pause menu's window onto the settings. Everything that knows what a key
  // means lives here; menu.cpp only knows how to draw a row and step a value.
  struct Host final : ds::sdl::SettingsHost {
    ds::sdl::Config& cfg;
    NDS& nds;
    Disp& display;
    VideoSetup& vs;
    const std::string& global_ini;
    const std::string& game_ini;
    std::function<void(const char*, const std::string&)> apply;
    std::function<void()> reopen;
    bool per_game = false;
    // Pointer, not a copy: transports come up after this host is built.
    const bool* net_live = nullptr;

    Host(ds::sdl::Config& c, NDS& n, Disp& d, VideoSetup& v, const std::string& gi, const std::string& pi, ds::sdl::Input& in)
        : cfg(c), nds(n), display(d), vs(v), global_ini(gi), game_ini(pi), input(in) {}

    // LAYOUT reads the live layout, not the file: hotkeys and a loaded
    // state move it without writing video.layout.
    std::string pending_mode;
    Disp::Mode effective_mode() const {
      Disp::Mode m;
      if (!pending_mode.empty() && Disp::parse_mode(pending_mode, m)) return m;
      return display.current_layout().mode;
    }
    // Which mode a cycle-checkbox row names, or false for any other key.
    static bool cycle_key(const char* key, Disp::Mode& m) {
      const size_t n = std::strlen(ds::sdl::kLayoutCyclePrefix);
      return std::strncmp(key, ds::sdl::kLayoutCyclePrefix, n) == 0 && Disp::parse_mode(key + n, m);
    }
    bool in_cycle(Disp::Mode m) const {
      return std::find(vs.layout_cycle.begin(), vs.layout_cycle.end(), m) != vs.layout_cycle.end();
    }

    std::string get(const char* key) const override {
      if (is_user_key(key) && user_in_firmware()) return user_get(key);
      if (std::strcmp(key, "video.layout") == 0) return Disp::mode_name(effective_mode());
      Disp::Mode m;
      if (cycle_key(key, m)) return in_cycle(m) ? "true" : "false";
      return cfg.str(key, "");
    }

    void set(const char* key, const std::string& value) override {
      if (is_user_key(key) && user_in_firmware()) { user_set(key, value); return; }
      // A checkbox edits the ring; ticking a mode reinserts it in Display::Mode order.
      Disp::Mode m;
      if (cycle_key(key, m)) {
        std::vector<Disp::Mode> ring = vs.layout_cycle;
        const bool on = value == "true";
        if (on && !in_cycle(m)) {
          auto at = ring.begin();
          while (at != ring.end() && *at < m) ++at;
          ring.insert(at, m);
        } else if (!on) {
          ring.erase(std::remove(ring.begin(), ring.end(), m), ring.end());
          if (ring.empty()) return;   // never nothing: value_allowed refuses this too
        }
        set("video.layout_cycle", layout_cycle_string(ring));
        return;
      }
      if (std::strcmp(key, "video.layout") == 0) pending_mode = value;
      cfg.set(key, value);
      apply(key, value);
      const std::string& path = per_game && !game_ini.empty() ? game_ini : global_ini;
      if (!ds::sdl::Config::store(path, key, value))
        std::fprintf(stderr, "settings: cannot write %s\n", path.c_str());
    }

    bool reopen_wanted = false, layout_wanted = false, fullscreen_wanted = false;
    std::function<void()> relayout;
    std::function<void()> refullscreen;
    void commit() override {
      // Layout first: reopen carries the live layout across.
      if (layout_wanted) { layout_wanted = false; relayout(); }
      if (fullscreen_wanted) { fullscreen_wanted = false; refullscreen(); }
      if (reopen_wanted) { reopen_wanted = false; reopen(); }
    }

    bool has_game() const override { return !game_ini.empty(); }

    // --- Controls ---------------------------------------------------------
    ds::sdl::Input& input;
    std::function<void()> reconfigure_input;

    // What a slot of a hotkey holds, defaults included; "none" for unset.
    std::string hot_value(bool pad, int a, int slot) const {
      const std::string key = (pad ? "padhotkeys." : "hotkeys.") + std::string(ds::sdl::action_name(static_cast<ds::sdl::Action>(a)))
                            + ds::sdl::Input::hot_suffix(slot);
      if (slot) return cfg.str(key, "none");   // second binding has no default
      return cfg.str(key, pad ? ds::sdl::Input::pad_hot_default(a) : ds::sdl::Input::key_hot_default(a));
    }
    static bool is_set(const std::string& v) { return !v.empty() && v != "none"; }

    // Second row shown once the player set one, or the first slot is bound
    // (so a second could join it).
    bool alt_row_shown(bool pad, int a) const {
      return is_set(hot_value(pad, a, 1)) || is_set(hot_value(pad, a, 0));
    }

    // DS buttons, then each hotkey (with its second row if shown), then
    // extras. Counting and indexing go through the same rules.
    int binding_count(bool pad) const override {
      int n = ds::sdl::Input::button_count() + extra_count(pad);
      for (int a = 0; a < ds::sdl::Input::action_count(); ++a) n += alt_row_shown(pad, a) ? 2 : 1;
      return n;
    }

    // What an extra row holds, and how it reads.
    std::string extra_value(const Extra& e, bool pad) const {
      const char* key = extra_key(e, pad);
      const std::string v = cfg.str(key, extra_default(key, pad, cfg));
      if (extra_is_axis_remap(key)) return axis_remap_label(v);
      if (extra_is_stick(key))   // true/false: stick_dpad's old spellings (Input::parse_stick)
        return v == "left" || v == "true" || v == "1" ? "LEFT STICK" : v == "right" ? "RIGHT STICK" : "NONE";
      return pad ? ds::sdl::Input::pad_label(v) : upper(v);
    }

    Binding binding(bool pad, int i) const override {
      const int nb = ds::sdl::Input::button_count();
      Binding out;
      if (i < 0) return out;
      if (i < nb) {
        const char* name = ds::sdl::Input::button_name(i);
        out.key = (pad ? "pad." : "keys.") + std::string(name);
        out.label = upper(name);
        out.value = cfg.str(out.key, pad ? ds::sdl::Input::pad_default(i) : ds::sdl::Input::key_default(i));
      } else {
        int k = i - nb, a = 0, slot = 0;
        for (; a < ds::sdl::Input::action_count(); ++a) {
          const int rows = alt_row_shown(pad, a) ? 2 : 1;
          if (k < rows) { slot = k; break; }
          k -= rows;
        }
        if (a >= ds::sdl::Input::action_count()) {
          if (k >= extra_count(pad)) return out;   // past the end: no row
          const Extra& e = extra_at(pad, k);
          out.key = extra_key(e, pad);
          out.label = e.label;
          out.value = extra_value(e, pad);
          return out;                              // already in the page's terms
        }
        const char* name = ds::sdl::action_name(static_cast<ds::sdl::Action>(a));
        out.key = (pad ? "padhotkeys." : "hotkeys.") + std::string(name) + ds::sdl::Input::hot_suffix(slot);
        // "FAST FORWARD TOGGLE" doesn't fit the row at the menu's size.
        out.label = upper(std::strcmp(name, "fast_forward_toggle") == 0 ? "ff_toggle" : name) + (slot ? " (2)" : "");
        out.value = hot_value(pad, a, slot);
      }
      // Display only: the file keeps SDL's lower-case names; pad values also
      // get position pips and L1/L2/SELECT spellings (pad_label).
      out.value = pad ? ds::sdl::Input::pad_label(out.value) : upper(out.value);
      return out;
    }

    bool has_pad() const override { return input.has_pad(); }
    void begin_capture(bool pad) override { input.begin_capture(pad); }
    void cancel_capture() override { input.cancel_capture(); }
    bool capturing() const override { return input.capturing(); }
    std::string take_capture() override { return input.take_capture(); }

    void bind(const std::string& key, const std::string& value0) override {
      std::string value = value0;
      // Stick rows store which stick, not the axis pushed; axis-remap rows
      // store the physical axis moved.
      if (extra_is_axis_remap(key.c_str()) && value != "none") {
        std::string raw = input.captured_raw_axis();
        if (raw.empty()) return;
        if (raw[0] == '+') raw = raw.substr(1);
        value = raw;
      }
      if (extra_is_stick(key.c_str()) && value != "none") {
        const char* stick = ds::sdl::Input::stylus_axis_of(value);
        if (!stick) return;
        value = stick;
      }
      // The modifier captured on its own for a hotkey row is "mod" alone: its
      // own name there would be a hotkey the modifier always swallows.
      {
        const bool pad_hot = key.compare(0, 11, "padhotkeys.") == 0, key_hot = key.compare(0, 8, "hotkeys.") == 0;
        if ((pad_hot || key_hot) && key != "padhotkeys.modifier" && key != "hotkeys.modifier" &&
            value == cfg.str(pad_hot ? "padhotkeys.modifier" : "hotkeys.modifier", ds::sdl::Input::mod_default(pad_hot)))
          value = "mod";
      }
      // A modifier can't be built on itself, nor can the pen's tap.
      if ((key == "hotkeys.modifier" || key == "padhotkeys.modifier" ||
           key == "pad.stylus_button" || key == "pad.stylus_button.alt" || key == "pad.stylus_dpad") &&
          value.compare(0, 4, "mod+") == 0)
        value = value.substr(4);
      cfg.set(key, value);
      const std::string& path = per_game && !game_ini.empty() ? game_ini : global_ini;
      if (!ds::sdl::Config::store(path, key, value))
        std::fprintf(stderr, "settings: cannot write %s\n", path.c_str());
      // Re-read the lot: configure() resolves the modifier, chords, collisions.
      reconfigure_input();
    }

    void reset_bindings(bool pad) override {
      const int nb = ds::sdl::Input::button_count();
      const std::string& path = per_game && !game_ini.empty() ? game_ini : global_ini;
      for (int i = 0; i < nb; ++i) {
        const std::string k = (pad ? "pad." : "keys.") + std::string(ds::sdl::Input::button_name(i));
        const char* v = pad ? ds::sdl::Input::pad_default(i) : ds::sdl::Input::key_default(i);
        cfg.set(k, v);
        ds::sdl::Config::store(path, k, v);
      }
      for (int a = 0; a < ds::sdl::Input::action_count(); ++a)
        for (int slot = 0; slot < ds::sdl::Input::HOT_SLOTS; ++slot) {
          const std::string k = (pad ? "padhotkeys." : "hotkeys.") + std::string(ds::sdl::action_name(static_cast<ds::sdl::Action>(a))) + ds::sdl::Input::hot_suffix(slot);
          const char* v = slot ? "none" : pad ? ds::sdl::Input::pad_hot_default(a) : ds::sdl::Input::key_hot_default(a);
          cfg.set(k, v);
          ds::sdl::Config::store(path, k, v);
        }
      for (int j = 0; j < extra_count(pad); ++j) {
        const char* k = extra_key(extra_at(pad, j), pad);
        const char* v = extra_default(k, pad, cfg);
        cfg.set(k, v);
        ds::sdl::Config::store(path, k, v);
      }
      reconfigure_input();
    }

    std::vector<std::string> collisions() const override { return input.collisions(); }

    // --- DS Options -------------------------------------------------------
    // Generated firmware: [user] in the config. Real dump: the dump's own
    // settings pages, read/written in memory, persisted to a sidecar.
    std::string fw_override;

    bool user_in_firmware() const { return !nds.firmware_synthetic; }

    const char* user_settings_note() const override {
      return user_in_firmware() ? "IN THE FIRMWARE" : nullptr;
    }

    static bool is_user_key(const char* key) { return std::strncmp(key, "user.", 5) == 0; }

    // Which field of the settings block a config key names.
    static bool user_field(const char* key, NDS::UserField& out) {
      const std::string k = key + 5;
      if (k == "nickname")       { out = NDS::UserField::Nickname; return true; }
      if (k == "message")        { out = NDS::UserField::Message; return true; }
      if (k == "colour")         { out = NDS::UserField::Colour; return true; }
      if (k == "birthday_month") { out = NDS::UserField::BirthdayMonth; return true; }
      if (k == "birthday_day")   { out = NDS::UserField::BirthdayDay; return true; }
      if (k == "language")       { out = NDS::UserField::Language; return true; }
      return false;
    }

    std::string user_get(const char* key) const {
      ds::bios::UserSettings u;
      if (!nds.read_user_settings(u)) return "";
      const std::string k = key + 5;
      if (k == "nickname")       return u.nickname;
      if (k == "message")        return u.message;
      if (k == "colour")         return std::to_string(u.favourite_colour);
      if (k == "birthday_month") return std::to_string(u.birthday_month);
      if (k == "birthday_day")   return std::to_string(u.birthday_day);
      if (k == "language")       return std::to_string(u.language);
      return "";
    }

    void user_set(const char* key, const std::string& v) {
      ds::bios::UserSettings u;
      if (!nds.read_user_settings(u)) return;
      NDS::UserField f;
      if (!user_field(key, f)) return;
      const std::string k = key + 5;
      if (k == "nickname")            u.nickname = v;
      else if (k == "message")        u.message = v;
      else if (k == "colour")         u.favourite_colour = static_cast<u8>(std::atoi(v.c_str()) & 15);
      else if (k == "birthday_month") u.birthday_month = static_cast<u8>(std::atoi(v.c_str()));
      else if (k == "birthday_day")   u.birthday_day = static_cast<u8>(std::atoi(v.c_str()));
      else if (k == "language")       u.language = static_cast<u8>(std::atoi(v.c_str()) & 7);
      if (!nds.write_user_settings(f, u)) return;
      // Written out now, not at exit: a kill would lose it otherwise.
      std::string err;
      if (!fw_override.empty() && !nds.save_firmware_override(fw_override, err))
        std::fprintf(stderr, "firmware settings: %s: %s\n", fw_override.c_str(), err.c_str());
    }

    static std::string upper(const std::string& s) {
      std::string out = s;
      for (char& c : out) {
        if (c == '_') c = ' ';
        else c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      }
      return out;
    }
    bool save_per_game() const override { return per_game && has_game(); }
    void set_save_per_game(bool on) override { per_game = on && has_game(); }

    // effects_at_source: DS-resolution draw for a hardware scaler, so
    // grid/seams/bilinear have no panel pixels; chunky still applies as the mean.
    bool panel_effects() const { return !display.effects_at_source(); }

    bool value_allowed(const ds::sdl::Setting& s, const char* value) const override {
      // Last ticked layout can't be unticked: an empty ring is nowhere for the hotkeys to go.
      Disp::Mode m;
      if (cycle_key(s.key, m))
        return std::strcmp(value, "false") != 0 || !in_cycle(m) || vs.layout_cycle.size() > 1;
      if (std::strcmp(s.key, "video.chunky") != 0 || panel_effects()) return true;
      return !std::strcmp(value, "false") || !std::strcmp(value, "mean");
    }

    bool enabled(const ds::sdl::Setting& s) const override { return !*disabled_reason(s); }

    const char* disabled_reason(const ds::sdl::Setting& s) const override {
      const auto flag = [&](const char* k, bool def) { return cfg.flag(k, def); };
      const Disp::Layout& l = display.current_layout();
      // Mode the page is working towards, so picking PIP lights its rows immediately.
      const Disp::Mode mode = effective_mode();
      const bool pip = mode == Disp::Mode::Pip;
      const bool dominant = mode == Disp::Mode::DominantV || mode == Disp::Mode::DominantH;
      switch (s.depends) {
      case ds::sdl::Dep::None: return "";
      case ds::sdl::Dep::FrameskipMode:
        // Frameskip is forced to 0 for a session, so its mode is moot too.
        if (net_live && *net_live) return "NOT DURING A NETWORK SESSION";
        return cfg.num("emu.frameskip", 0) > 0 ? "" : "ONLY WITH FRAMESKIP ON";
      case ds::sdl::Dep::PanelEffects:
        if (display.rga_present()) return "THE RGA ALWAYS SCALES BILINEAR";
        return panel_effects() ? "" : "THIS SCREEN SCALES IN HARDWARE";
      case ds::sdl::Dep::GridSeam:
        if (display.rga_present()) return std::strcmp(s.key, "video.seam") == 0 ? "THE RGA DRAWS THE DARK GRID ONLY" : "";
        if (!panel_effects()) return "THIS SCREEN SCALES IN HARDWARE";
        return flag("video.linear", false) ? "BILINEAR IS ON" : "";
      case ds::sdl::Dep::Chunky:
        if (display.rga_present()) return "NOT ON THE RGA PRESENT";
        return flag("video.linear", false) ? "BILINEAR IS ON" : "";
      case ds::sdl::Dep::ChunkyCell:
        if (display.rga_present()) return "NOT ON THE RGA PRESENT";
        if (flag("video.linear", false)) return "BILINEAR IS ON";
        return cfg.str("video.chunky", "false") == "false" ? "ONLY WITH CHUNKY ON" : "";
      case ds::sdl::Dep::Windowed:
        // A tier that owns the panel is already filling it.
        return display.scaling() && !display.window() ? "THIS SCREEN IS ALWAYS FULL" : "";
      case ds::sdl::Dep::OneWindow:
        return vs.dual_window ? "NOT WITH TWO WINDOWS" : "";
      case ds::sdl::Dep::Pip:
        return pip ? "" : "PIP LAYOUT ONLY";
      case ds::sdl::Dep::PipTouchHold:
        if (!pip) return "PIP LAYOUT ONLY";
        return l.pip_alpha < 1.0 ? "" : "ONLY WHEN THE PIP FADES";
      case ds::sdl::Dep::Dominant:
        return dominant ? "" : "DOMINANT LAYOUTS ONLY";
      case ds::sdl::Dep::DominantThreshold:
        if (!dominant) return "DOMINANT LAYOUTS ONLY";
        return cfg.str("video.dominant_ratio", "auto") == "auto" ? "" : "ONLY WHEN THE RATIO IS AUTO";
      case ds::sdl::Dep::Net:
#if DSPERATE_NET
        return "";
#else
        return "THIS BUILD HAS NO NETWORKING";
#endif
      case ds::sdl::Dep::NetInternet:
        // Reads the config, not the session: the row is restart-only.
        return cfg.str("net.mode", "off") == "internet" ? "" : "ONLY WITH NETWORK FEATURES ON INTERNET";
      case ds::sdl::Dep::NetSession:
        return (net_live && *net_live) ? "NOT DURING A NETWORK SESSION" : "";
      case ds::sdl::Dep::ShortcutsPath:
        return shortcuts_dir(cfg).empty() ? "NEEDS PATHS.DSI_GAMES OR PATHS.GAMES" : "";
      }
      return "";
    }
  };

  Host host(cfg, nds, display, vs, global_ini, session.game_ini, input);
  host.reconfigure_input = [&] { input.configure(cfg); };
  host.fw_override = fw_override;
  host.net_live = &net_live;
  host.reopen = [&] { reopen_display(); };
  // Whole layout re-read from the config in one go. Mode is the LAYOUT
  // row's pending choice if one was made, else the live one (a hotkey or
  // loaded state may have moved it since the file was read).
  host.relayout = [&] {
    if (dual_window) {   // two windows, one screen each: only which screen is on which panel
      host.pending_mode.clear();
      VideoSetup want;
      if (!parse_video(cfg, want) || want.layout.primary == vs.layout.primary) return;
      vs.layout.primary = want.layout.primary;
      display.set_only_screen(vs.layout.primary);
      display2.set_only_screen(1 - vs.layout.primary);
      menu_dirty = true;
      return;
    }
    VideoSetup want;
    if (!parse_video(cfg, want)) { host.pending_mode.clear(); return; }
    want.layout.mode = host.effective_mode();
    host.pending_mode.clear();
    display.set_layout(want.layout);
    vs.layout = want.layout;
    pip_alpha = static_cast<int>(want.layout.pip_alpha * 255.0 + 0.5);
    apply_visibility();
    menu_dirty = true;
  };
  host.refullscreen = [&] {
    if (display.fullscreen() == cfg.flag("video.fullscreen", false)) return;
    display.toggle_fullscreen();
    if (dual_window) display2.toggle_fullscreen();
    menu_dirty = true;
  };
  // Declared here since host.apply needs to call it (NETWORK FEATURES is live).
  std::function<void(const std::string&)> set_net_mode;
  // Push one changed key into the running machine. Anything not named here
  // either needs the display reopened (below) or is only read at startup.
  host.apply = [&](const char* key, const std::string& v) {
    const auto is = [&](const char* k) { return std::strcmp(key, k) == 0; };
    const bool on = v == "1" || v == "true" || v == "yes" || v == "on";
    if (is("emu.frameskip")) { if (!net_live) fs_limit = std::atoi(v.c_str()); return; }
    if (is("emu.frameskip_mode")) { fs_adaptive = v != "fixed"; return; }
    if (is("emu.dsi_nand_shortcuts")) {
      shortcuts_note = sync_nand_shortcuts(on);   // shown by the frame loop's toast
      if (boot_firmware && nds.cart) games = enumerate_library(cfg);
      return;
    }
    // NETWORK FEATURES: live, so a player can put the radio up and down without quitting.
    if (is("net.mode")) { if (set_net_mode) set_net_mode(v); return; }
    if (is("emu.speed")) { speed_pct = std::atoi(v.c_str()); apply_limiter(); return; }
    if (is("emu.limiter")) { limiter_mode = v; apply_limiter(); return; }
    if (is("audio.buffer_size")) { apply_audio_buffer(v); return; }
    if (is("audio.latency_frames")) { audio.set_buffer_ms(std::atoi(v.c_str()) * ds::sdl::Audio::FRAME_MS); return; }
    if (is("emu.ff_speed")) { ff_speed = std::atoi(v.c_str()); return; }
    if (is("emu.ff_skip")) { ff_skip = std::atoi(v.c_str()); return; }
    if (is("emu.autosave")) { autosave = on; return; }
    if (is("video.aa")) { apply_aa(nds, aa_on(v)); return; }
    if (is("video.gpu3d")) { std::string why; if (!nds.gpu3d.renderer().set_gpu(on, &why)) std::fprintf(stderr, "gpu3d: unavailable (%s), drawing on the CPU\n", why.c_str()); return; }
    if (is("video.gpu_present")) { host.reopen_wanted = true; return; }
    if (is("video.fps")) { fps_osd = on; if (on && !show_fps) { fps_mark = SDL_GetPerformanceCounter(); emu_ticks = draw_ticks = wait_ticks = 0; } return; }
    if (is("video.pip_touch_hold")) { pip_touch_hold = std::max(0, std::atoi(v.c_str())); return; }
    // Everything below moves the picture, so it's applied on menu close (Host::commit).
    if (is("video.fullscreen")) { host.fullscreen_wanted = true; return; }
    if (is("video.layout_cycle")) {
      std::vector<Disp::Mode> ring;
      if (parse_layout_cycle(v, ring) && !ring.empty()) layout_cycle = ring;
      return;
    }
    if (is("video.layout") || is("video.screen") || is("video.pip_corner") || is("video.pip_scale") ||
        is("video.pip_alpha") || is("video.screen_gap") || is("video.dominant_ratio") || is("video.dominant_threshold")) {
      host.layout_wanted = true;
      return;
    }
    // Baked into the scaler's tables at open, so applying means reopening.
    if (is("video.linear") || is("video.lcd_grid") || is("video.seam") ||
        is("video.chunky") || is("video.chunky_cell") || is("video.integer_scale"))
      host.reopen_wanted = true;
  };
  menu.set_settings_host(&host);
  auto set_paused = [&](bool p) {
    if (p == paused) return;
    paused = p;
    audio.pause(p);
    display.set_page(p);
    if (p) flush_save(); else { pacer.reset(); fs_debt_ms = 0; }
    VLOG("%s\n", p ? "paused" : "resumed");
  };
  // A framed, four-second notice drawn over the game. One at a time.
  struct Toast { const char* header = nullptr; std::string title, detail; u32 points = 0; int frames = 0; };
  std::deque<Toast> toast_queue;
  Toast toast_now;
  int toast_left = 0;
  ds::sdl::Rect toast_last{};          // what the canvas has to take back out
  constexpr int TOAST_FRAMES = 240;    // four seconds: long enough to read two lines
  constexpr int TOAST_INFO_FRAMES = 150;
  // DSi state loaded without an SD card. Twice a toast's time.
  auto sd_note_toast = [&] {
    if (nds.sd_card_note.empty()) return;
    Toast t;
    t.header = "SD CARD";
    t.title = "LEFT OUT OF THE STATE";
    t.detail = nds.sd_card_note.find("without one") != std::string::npos ? "THE STATE HAD NO CARD" : "ITS FOLDER HAS CHANGED";
    t.frames = TOAST_FRAMES * 2;
    toast_queue.push_back(std::move(t));
  };
  sd_note_toast();   // covers the state autoloaded at startup
  bool dsp_warned = false;   // the DSP warning below has been shown for this start

  auto toast_step = [&] {
    if (toast_left > 0) { --toast_left; return; }
    if (toast_queue.empty()) return;
    toast_now = std::move(toast_queue.front());
    toast_queue.pop_front();
    toast_left = toast_now.frames;
  };
  const auto toast_on = [&] { return toast_left > 0; };
  auto draw_toast_on = [&](const ds::sdl::Canvas& c) {
    ds::sdl::draw_toast(c, toast_now.header, toast_now.title.c_str(),
                        toast_now.detail.empty() ? nullptr : toast_now.detail.c_str(),
                        toast_now.points);
  };

#if DSPERATE_CHEEVOS
  // RetroAchievements, Casual mode. Off unless asked for; every failure is
  // reported once and then ignored.
  ds::cheevos::Client cheevos;
  const bool cheevos_on = cfg.flag("cheevos.enabled", false);
#endif
#if DSPERATE_NET
  std::unique_ptr<ds::net::LanMp> lan;
  std::unique_ptr<ds::net::SlirpDriver> slirp;
  // One function for starting/stopping a session, used at startup and the
  // menu's NETWORK FEATURES row. `mode` is a net.mode value plus "join" for
  // --lan-join. What a session takes away, it gives back on the way out.
  // Set when a guest heard nobody: see begin_guest_scan below.
  bool guest_retry_armed = false, radio_was_on = false, scanning = false;
  bool knobs_held = false;
  int held_speed = 100; std::string held_limiter = "auto";
  int  held_fs_limit = 0;
  bool held_ff_toggle = false;
  auto take_away_for_session = [&] {
    if (knobs_held) return;
    knobs_held = true;
    held_fs_limit = fs_limit;
    held_ff_toggle = ff_toggle;
    held_speed = speed_pct;
    held_limiter = limiter_mode;
    // A console in a session runs at the console's rate, full stop: the peer keeps time.
    if (speed_pct != 100 || limiter_mode != "auto") {
      if (speed_pct != 100 || !limiter_off)
        std::fprintf(stderr, "net: the game runs at the console's own rate for this session -- the network keeps time\n");
      speed_pct = 100;
      limiter_mode = "auto";
      apply_limiter();
    }
    if (fs_limit > 0) {
      std::fprintf(stderr, "net: frameskip is off for this session -- the emulator cannot set its own pace while the network keeps time\n");
      fs_limit = 0;
    }
    if (ff_toggle) {
      std::fprintf(stderr, "net: fast forward is off for this session\n");
      ff_toggle = false;
    }
  };
  auto give_back_after_session = [&] {
    if (!knobs_held) return;
    knobs_held = false;
    fs_limit = held_fs_limit;
    ff_toggle = held_ff_toggle;
    if (speed_pct != held_speed || limiter_mode != held_limiter) {
      speed_pct = held_speed;
      limiter_mode = held_limiter;
      apply_limiter();
      std::fprintf(stderr, "net: the frame limiter is back where it was -- the session is over\n");
    }
  };
  set_net_mode = [&](const std::string& mode) {
    // Down first: a guest says goodbye rather than going silent.
    if (lan) { lan->end_session(); nds.io.wifi.set_transport(nullptr); lan.reset(); }
    if (slirp) { nds.io.set_net_driver(nullptr); slirp->stop(); slirp.reset(); }
    const bool was_live = net_live;
    net_live = false;

    if (mode == "internet") {
      // Wiimmfi's resolver answers Nintendo's own hostnames with its own
      // servers. wifi.dns takes any address, since this has moved before.
      constexpr ds::u32 kWiimmfiDns = 0xB23E2BD4;   // 178.62.43.212
      auto dns = ds::net::SlirpDriver::Dns::Custom;
      ds::u32 dns_addr = kWiimmfiDns;
      const std::string where = dns_arg ? dns_arg : cfg.str("wifi.dns", "wiimmfi");
      if (where == "host") { dns = ds::net::SlirpDriver::Dns::Host; dns_addr = 0; }
      else if (where != "wiimmfi") {
        in_addr parsed{};
        if (inet_pton(AF_INET, where.c_str(), &parsed) == 1) {
          dns_addr = ntohl(parsed.s_addr);
        } else {
          std::fprintf(stderr, "wifi.dns: \"%s\" is not host, wiimmfi or an address; using wiimmfi\n", where.c_str());
        }
      }
      slirp = std::make_unique<ds::net::SlirpDriver>();
      if (!slirp->start(dns, dns_addr)) {
        std::fprintf(stderr, "internet: %s\n", slirp->error().c_str());
        slirp.reset();
      } else {
        nds.io.set_net_driver(slirp.get());
        net_live = true;
        VLOG("internet: up, DNS %s\n", where.c_str());
      }
    } else if (mode == "auto" || mode == "guest" || mode == "host" || mode == "join") {
      lan = std::make_unique<ds::net::LanMp>();
      bool up = lan->ok();
      if (up && (mode == "auto" || mode == "guest")) {
        // Same scan either way; auto may fall back to hosting, guest may not.
        const auto role = lan->start_auto(lan_name, 2500, 16, mode == "auto");
        up = role != ds::net::LanMp::Role::None;
        if (up) VLOG("netplay: %s\n", role == ds::net::LanMp::Role::Host ? "no session heard, hosting" : ("joined " + lan->peer_name()).c_str());
      } else if (up) {
        up = mode == "join" ? lan->start_client(lan_name, lan_join)
                            : lan->start_host(lan_host ? lan_host : lan_name, 16);
      }
      if (!up) {
        std::fprintf(stderr, "lan: %s\n", lan->error().c_str());
        lan.reset();
        // Guest heard nobody: try again when the game asks for its radio.
        if (mode == "guest") guest_retry_armed = true;
      } else {
        VLOG("lan: %s, player %d\n", lan->is_host() ? "hosting" : "joined", lan->my_id());
        nds.io.wifi.set_transport(lan.get());
        net_live = true;
      }
    }

    if (net_live) take_away_for_session();
    else give_back_after_session();
    menu.set_network_session(net_live);
    // Pause menu is a stop with no session, an overlay with one.
    if (menu.open() && net_live != was_live) {
      if (net_live) { set_paused(false); display.set_page(true); }
      else set_paused(true);
      menu_dirty = true;
    }
  };
  // A guest that heard nobody gets one more go when the game first puts a
  // frame on the air. Once only, guest-mode only (auto hosts when it hears
  // nothing). Signal is Wifi::tx_frames(), NOT radio power (fires too
  // early). Spread across frames, unlike start_auto's blocking scan.
  Uint32 scan_began_ms = 0;
  constexpr Uint32 kScanMs = 2500;   // as start_auto: a host beacons once a second
  // Keep each line to about 24 characters: toast_box truncates past two
  // thirds of the DS screen width.
  auto net_toast = [&](const char* title, const char* detail, int frames) {
    Toast t;
    t.header = "LOCAL WIRELESS";
    t.title = title;
    if (detail) t.detail = detail;
    t.frames = frames;
    toast_queue.push_back(std::move(t));
  };
  auto begin_guest_scan = [&] {
    guest_retry_armed = false;
    if (!lan) lan = std::make_unique<ds::net::LanMp>();
    if (!lan->ok() || !lan->start_discovery()) {
      std::fprintf(stderr, "lan: %s\n", lan->error().c_str());
      lan.reset();
      net_toast("SCAN FAILED", "THE NETWORK REFUSED IT", TOAST_FRAMES);
      return;
    }
    scanning = true;
    scan_began_ms = SDL_GetTicks();
    VLOG("lan: the game is on the air; looking for a session\n");
    net_toast("LOOKING FOR A SESSION", nullptr, static_cast<int>(kScanMs) * 60 / 1000);
  };
  auto finish_guest_scan = [&] {
    scanning = false;
    if (!lan) return;
    if (!lan->scan_join(lan_name)) {
      std::fprintf(stderr, "lan: %s\n", lan->error().c_str());
      lan.reset();
      net_toast("NO SESSION FOUND", "NOBODY IS HOSTING HERE", TOAST_FRAMES);
      return;
    }
    nds.io.wifi.set_transport(lan.get());
    net_live = true;
    take_away_for_session();
    menu.set_network_session(true);
    VLOG("lan: joined %s as player %d\n", lan->peer_name().c_str(), lan->my_id());
    net_toast("JOINED A SESSION", lan->peer_name().c_str(), TOAST_INFO_FRAMES);
  };
  {
    // What the flags and net.mode asked for, in the words set_net_mode takes.
    const char* mode = lan_join ? "join" : lan_host ? "host" : netplay ? "auto"
                     : lan_guest ? "guest" : internet ? "internet" : "off";
    if (std::strcmp(mode, "off") != 0) set_net_mode(mode);
  }
#else
  if (lan_host || lan_join || netplay || lan_guest) std::fprintf(stderr, "lan: built without DSPERATE_NET\n");
  if (internet) std::fprintf(stderr, "internet: built without DSPERATE_NET\n");
#endif
#if DSPERATE_CHEEVOS
  std::string cheevos_hash;      // the running game's identity; empty if it could not be hashed
  bool cheevos_set_asked = false;
  // The cart's hash, or for a DSiWare title run from NAND, the one taken
  // when read. The loader cart is not a game and gets none.
  auto cheevos_identify = [&](const std::string& name) {
    cheevos_hash.clear();
    cheevos_set_asked = false;
    std::string err;
    if (!nds.cart) { cheevos_hash = dsi_title_cheevos_hash; return; }
    if (!ds::cheevos::rom_hash(nds.cart->source(), name, cheevos_hash, err, nds.dsi))
      std::fprintf(stderr, "cheevos: cannot identify this ROM: %s\n", err.c_str());
  };

  auto cheevos_show = [&] {
    for (const ds::cheevos::Message& m : cheevos.take_messages()) {
      std::fprintf(stderr, "cheevos: %s%s%s\n", m.text.c_str(),
                   m.detail.empty() ? "" : " -- ", m.detail.c_str());
      // Through shot_pending (hotkey's path): captures the game, not the
      // toast, which is drawn onto the canvas afterwards.
      if (m.kind == ds::cheevos::Message::Kind::Unlock && cfg.flag("cheevos.auto_screenshot", false))
        shot_pending = true;
      if (!cfg.flag("cheevos.toasts", true)) continue;
      Toast t;
      t.header = m.kind == ds::cheevos::Message::Kind::Unlock ? "ACHIEVEMENT UNLOCKED"
               : m.kind == ds::cheevos::Message::Kind::Problem ? "RETROACHIEVEMENTS" : nullptr;
      t.title = m.text;
      t.detail = m.detail;
      t.points = m.points;
      t.frames = m.kind == ds::cheevos::Message::Kind::Unlock ? TOAST_FRAMES : TOAST_INFO_FRAMES;
      toast_queue.push_back(std::move(t));
    }
  };
  // Cached rather than rebuilt per frame: rc_client_create_achievement_list allocates.
  struct CheevosMenuHost final : ds::sdl::CheevosHost {
    ds::cheevos::Client* c = nullptr;
    std::vector<ds::cheevos::Client::Achievement> rows;
    ds::cheevos::Client::Summary sum{};
    void refresh() {
      if (!c) return;
      rows = c->achievements();
      sum = c->summary();
    }
    std::string status() const override {
      if (!c) return "NOT AVAILABLE IN THIS BUILD";
      switch (c->state()) {
      case ds::cheevos::State::Off:
        return c->unavailable_reason().empty() ? "TURNED OFF" : c->unavailable_reason();
      case ds::cheevos::State::SignedOut:   return "NOT SIGNED IN";
      case ds::cheevos::State::SigningIn:   return "SIGNING IN...";
      case ds::cheevos::State::SignedIn:    return "SIGNED IN AS " + c->username();
      case ds::cheevos::State::LoadingGame: return "LOADING ACHIEVEMENTS...";
      case ds::cheevos::State::Playing:     return "SIGNED IN AS " + c->username();
      case ds::cheevos::State::EmptySet:
        return "NO ACHIEVEMENTS PUBLISHED FOR THIS GAME YET";
      case ds::cheevos::State::NoSet:
        return "NO ACHIEVEMENTS FOR THIS ROM - HASH " + c->game_hash();
      }
      return "";
    }
    std::string progress() const override {
      if (!c || sum.total == 0) return {};
      char buf[128];
      // Encore explains why an already-earned achievement unlocks again.
      std::snprintf(buf, sizeof buf, "%u/%u EARNED  %u/%u POINTS%s",
                    sum.unlocked, sum.total, sum.points_earned, sum.points,
                    c->encore() ? "  (ENCORE)" : "");
      return buf;
    }
    bool signed_in() const override {
      if (!c) return false;
      const auto st = c->state();
      return st == ds::cheevos::State::SignedIn || st == ds::cheevos::State::Playing ||
             st == ds::cheevos::State::LoadingGame || st == ds::cheevos::State::NoSet ||
             st == ds::cheevos::State::EmptySet;
    }
    bool has_set() const override { return !rows.empty(); }
    int row_count() const override { return static_cast<int>(rows.size()); }
    Row row(int i) const override {
      Row r;
      if (i < 0 || i >= static_cast<int>(rows.size())) return r;
      const auto& a = rows[static_cast<size_t>(i)];
      r.title = a.title;
      r.detail = (!a.unlocked && !a.progress.empty()) ? a.progress + "   " + a.description
                                                     : a.description;
      r.points = a.points;
      r.unlocked = a.unlocked;
      r.unsupported = a.unsupported;
      return r;
    }
    void sign_in(const std::string& user, const std::string& password) override {
      if (c) c->sign_in(user, password);
    }

    ds::sdl::SettingsHost* settings = nullptr;
    static const char* key_of(Option o) {
      switch (o) {
      case Option::Toasts:     return "cheevos.toasts";
      case Option::Screenshot: return "cheevos.auto_screenshot";
      case Option::Encore:     return "cheevos.encore";
      }
      return "";
    }
    bool option(Option o) const override {
      if (!settings) return false;
      const std::string v = settings->get(key_of(o));
      if (v.empty()) return o == Option::Toasts;   // only toasts default on
      return v == "true" || v == "1" || v == "on";
    }
    void set_option(Option o, bool on) override {
      if (!settings) return;
      settings->set(key_of(o), on ? "true" : "false");
      // Encore is read at game load; tell the session now for next load.
      if (o == Option::Encore && c) c->set_encore(on);
    }
    void sign_out() override {
      if (c) c->sign_out();
      rows.clear();
      sum = {};
    }
  } cheevos_menu;
  // Achievement progress in and out of save states.
  struct CheevosState final : CheevosStateHook {
    ds::cheevos::Client* c = nullptr;
    bool save(u32& game_id, std::vector<u8>& blob) override {
      if (!c) return false;
      game_id = c->game_id();
      return c->serialize_progress(blob);
    }
  } cheevos_state;

  // Puts a state's achievement progress in once a set exists for it. Called
  // before rc_client_do_frame, so the runtime never evaluates stale progress.
  auto cheevos_apply_state = [&] {
    if (!g_cheevos_pending.waiting) return;
    if (cheevos.state() == ds::cheevos::State::LoadingGame ||
        cheevos.state() == ds::cheevos::State::SigningIn) return;   // not yet; keep waiting
    if (cheevos.state() != ds::cheevos::State::Playing) {
      g_cheevos_pending = {};
      return;
    }
    // Game id must match, or applying it is how a false unlock happens; a
    // mismatch resets rather than guesses.
    const bool ok = !g_cheevos_pending.blob.empty() && g_cheevos_pending.game_id != 0 &&
                    g_cheevos_pending.game_id == cheevos.game_id() &&
                    cheevos.deserialize_progress(g_cheevos_pending.blob.data(),
                                                 g_cheevos_pending.blob.size());
    if (!ok) cheevos.reset();
    VLOG("cheevos: state progress %s\n", ok ? "restored" : "reset (none carried, or it did not apply)");
    g_cheevos_pending = {};
  };

  if (cheevos_on) {
    cheevos_menu.c = &cheevos;
    cheevos_menu.settings = &host;
    menu.set_cheevos_host(&cheevos_menu);
    cheevos_state.c = &cheevos;
    g_cheevos_state = &cheevos_state;
  }

  // Watches for the moment sign-in (asynchronous) completes, rather than
  // asking for the set at boot and reporting a misleading "Login required".
  // Also what lets signing in from the menu load the set for a running game.
  // Without this the account page sits on "SIGNING IN..." until the player
  // presses something, since the menu only recomposites on a detected change.
  ds::cheevos::State cheevos_drawn_state = ds::cheevos::State::Off;
  auto cheevos_menu_follow = [&] {
    if (cheevos.state() == cheevos_drawn_state) return;
    cheevos_drawn_state = cheevos.state();
    cheevos_menu.refresh();   // the list and the counts move with it
    menu_dirty = true;
  };

  auto cheevos_catch_up = [&] {
    if (cheevos_set_asked || cheevos_hash.empty()) return;
    if (cheevos.state() != ds::cheevos::State::SignedIn) return;
    cheevos_set_asked = true;
    cheevos.load_game(nds, cheevos_hash);
  };
  if (cheevos_on) {
    std::string err;
    if (!cheevos.start(err)) {
      std::fprintf(stderr, "cheevos: %s\n", cheevos.unavailable_reason().c_str());
    } else {
      // Before anything loads: rcheevos evaluates encore at game load and
      // ignores it afterwards.
      cheevos.set_encore(cfg.flag("cheevos.encore", false));
      // A password is never kept; an expired token fails and the player
      // signs in again from the menu.
      ds::cheevos::Credentials creds;
      // A token file named on the command line overrides whatever is
      // stored; nothing is written back, the file isn't ours.
      const std::string token_file = cfg.str("cheevos.token_file");
      if (!token_file.empty()) {
        if (!ds::cheevos::read_token_file(token_file, creds, err)) {
          std::fprintf(stderr, "cheevos: %s\n", err.c_str());
        } else {
          if (creds.username.empty()) creds.username = cfg.str("cheevos.username");
          if (creds.username.empty()) {
            std::fprintf(stderr, "cheevos: %s has a token but no username; pass --cheevos-user or set cheevos.username\n",
                         token_file.c_str());
            creds = ds::cheevos::Credentials{};
          } else {
            std::fprintf(stderr, "cheevos: using the token from %s\n", token_file.c_str());
          }
        }
      }
      if (creds.empty() &&
          !ds::cheevos::load_credentials(ds::sdl::Config::dir(), creds, err) && !err.empty())
        std::fprintf(stderr, "cheevos: %s\n", err.c_str());
      // Failing that, the CFW front end's own sign-in: only the token is
      // read, never the password. Ours wins once we have one.
      if (creds.empty() && cfg.flag("cheevos.use_system_login", true)) {
        std::string from;
        if (ds::cheevos::import_cfw_credentials(creds, from))
          std::fprintf(stderr, "cheevos: using the sign-in from %s\n", from.c_str());
      }
      if (!creds.empty()) cheevos.sign_in_with_token(creds.username, creds.token);
      else std::fprintf(stderr, "cheevos: not signed in\n");

      // Safe to hash the cart's source here: read_unpatched reads past the
      // secure-area rewrite Cart has already done by this point.
      if (!launcher) cheevos_identify(session.rom_path);
    }
    cheevos_show();
  }
#endif
  // Starting a game from the loader cart's picker (and DS_LAUNCH_AT, which
  // does the same from a script).
  bool from_list = false;   // the game running was picked from the list: RESET goes back to the firmware
  auto launch_game = [&](const std::string& pick) {
    VLOG("launcher: %s\n", pick.c_str());
    // DSiWare runs with the launcher hand-off and a made-up NAND; no way
    // back to the DS menu, the session ends when the title does.
    const bool pick_shortcut = ds::io::is_shortcut_name(pick);
    const bool pick_dsi = pick_shortcut || ds::io::file_is_dsiware(pick);
    if (pick_shortcut && shortcut_nand.empty()) {
      std::fprintf(stderr, "launcher: %s needs the NAND (paths.dsi_nand in %s)\n", pick.c_str(), global_ini.c_str());
      return;
    }
    bool left_dsi = false;                      // a DS pick leaving the DSi machine
    if (pick_dsi && !have_dsi_bios()) {
      std::fprintf(stderr, "launcher: %s is DSiWare and needs the DSi BIOS pair (paths.bios9i/paths.bios7i in %s)\n", pick.c_str(), global_ini.c_str());
      return;
    }
    if (pick_dsi && !nds.bios_native) {
      std::fprintf(stderr, "launcher: %s is DSiWare and needs the DS BIOS dumps too (paths.bios9/paths.bios7 in %s); the built-in FreeBIOS cannot boot a DSi\n",
                   pick.c_str(), global_ini.c_str());
      return;
    }
    flush_save();
#if DSPERATE_JIT
    // Back off the firmware's strict timing: the game wants the speed.
    if (jit) { ds::jit::set_strict(std::getenv("DS_JIT_STRICT") != nullptr); ds::jit::flush_all(); }
#endif
    if (pick_dsi) {
      std::string err;
      if (!nds.load_dsi_bios(dsi_bios9i, dsi_bios7i, &err) || !nds.bios_native_dsi) {
        std::fprintf(stderr, "launcher: dsi bios: %s\n", err.empty() ? "not found" : err.c_str());
        return;
      }
      if (!open_dsi_sd()) return;
      // Replaces the DS firmware; its settings sidecar left alone from here on.
      if (have_dsi_fw && !cli.has("paths.firmware") && fw != dsi_fw) {
        if (!nds.load_bios(bios9, bios7, dsi_fw, user, &err)) {
          std::fprintf(stderr, "launcher: dsi firmware: %s\n", err.c_str());
          input.request_quit();   // the DS firmware is gone: nothing to go back to
          return;
        }
        nds.set_wifi_mac_suffix(static_cast<u32>(cfg.num("net.mac_suffix", 0)));
        fw_override.clear();
        host.fw_override.clear();
        VLOG("launcher: DSi firmware %s\n", dsi_fw.c_str());
      }
      nds.dsi_nand.close();
      if (pick_shortcut && !nds.load_dsi_nand(shortcut_nand, &err)) {
        std::fprintf(stderr, "launcher: dsi nand: %s\n", err.c_str());
        input.request_quit();
        return;
      }
      nds.dsi_nand_synthetic = false;
      nds.dsi_nand_boot = false;   // picked from the DSi launcher: made-up NAND is handed over, not booted
      nds.dsi_boot_blobs.clear();
      nds.dsi_loader_watch = false;
      nds.set_dsi(true);
      nds.dsi_hle_launch = true;
      dsi_mode = dsi_hle = true;
    } else if (nds.dsi) {
      // DS game picked on the DSi launcher runs on a DS; DSi firmware stays loaded.
      nds.dsi_nand.close();
      nds.dsi_sd.close();   // synced by the flush_save() above; a DS has no SD slot
      nds.dsi_nand_synthetic = false;
      nds.dsi_nand_boot = false;
      nds.dsi_boot_blobs.clear();
      nds.dsi_loader_watch = false;
      nds.dsi_font_hle = false;
      nds.set_dsi(false);
      dsi_mode = dsi_hle = false;
      left_dsi = true;
    }
    nds.reset();
    discard_session_cache();
    bool loaded;
    if (pick_shortcut) {
      loaded = load_shortcut_title(pick);
    } else if (pick_dsi && is_cia_container(pick)) {
      std::vector<ds::u8> srl;
      std::string err;
      loaded = ds::io::read_dsiware(pick, srl, &err) && nds.load_rom_image(std::move(srl));
      if (!loaded) std::fprintf(stderr, "launcher: %s\n", err.c_str());
    } else {
      loaded = load_rom_notice(pick);
    }
    if (loaded && pick_dsi && !prepare_dsiware()) { input.request_quit(); return; }   // the DS machine is gone: nothing to go back to
    if (!loaded && pick_dsi) { input.request_quit(); return; }
    if (!loaded) {
      // Stay on the list: a cancelled unpacking is a change of mind, not an error.
      std::fprintf(stderr, "launcher: could not read %s\n", pick.c_str());
      return;
    }
    nds.setup_direct_boot();
    if (left_dsi) VLOG("launcher: %s on a DS (%s)\n", pick.c_str(), jit ? "recompiler" : "interpreter");
    if (pick_dsi) {
      dsi_paths = pick_shortcut ? ds::io::NandPersistPaths::beside(shortcut_nand, cfg.str("paths.saves")) : hle_paths(pick);
      import_dsiware_saves();
      nand_writes_seen = nand_exported = nds.dsi_nand.writes;
      std::fprintf(stderr, "launcher: %s on the DSi (EXPERIMENTAL)\n", pick.c_str());
    }
    // --save deliberately not carried over: it pins one file for the command-line ROM.
    launcher = false;
    from_list = true;
    session.open(nds, cfg, pick, nullptr);
#if DSPERATE_CHEEVOS
    if (cheevos_on) { cheevos.unload_game(); cheevos_identify(pick); }
#endif
    menu.set_cheats(&nds.cheats.codes, &session.cheats.groups);
    session.load_enabled(nds);
    load_save(nds, session.sav);
    // After the battery save, so the state's SRAM wins; never during replay/recording.
    if (const std::string a = log.reading() || log.writing()
                                  ? std::string()
                                  : autoload_path(nds, session.states_dir, cfg.flag("emu.autoload", false));
        !a.empty() && load_state_file(nds, a, loaded_layout, got_layout)) {
      std::fprintf(stderr, "state: autoloaded %s\n", a.c_str());
      apply_loaded_layout();
      sd_note_toast();
    }
    sram_writes_seen = nds.cart ? nds.cart->sram_writes() : 0;
    state_slot = 0;
    refresh_slots();
    menu.set_open(false);
    set_paused(false);
  };
  auto pump_menu = [&] {
      if (!menu.open()) return;
      // Ticked every idle pass: held-direction repeat and name scrolling need elapsed time.
      const Uint32 now_ms = SDL_GetTicks();
      const u32 elapsed = static_cast<u32>(now_ms - menu_ms);
      menu_ms = now_ms;
      if (host.capturing()) menu_dirty = true;   // shows "PRESS ANY..." until collected
      menu.face_presses(input.take_menu_faces());
      switch (menu.update(input.take_menu_presses(), input.menu_held(), elapsed)) {
      case Menu::Result::None: break;
      case Menu::Result::Resume:
        state_slot = menu.slot();
        if (menu.cheats_dirty()) { session.save_enabled(nds); menu.clear_cheats_dirty(); }
        menu.set_open(false);
        display.set_page(false);
        set_paused(false);
        break;
      // Same guards as the save-state hotkeys.
      case Menu::Result::Save:
        if (net_live) std::fprintf(stderr, "state: not during a network session\n");
        else if (save_readonly) std::fprintf(stderr, "state: not during a replay\n");
        else if (save_state_file(nds, state_path(nds, session.states_dir, menu.slot()), display.current_layout())) {
          g_state_refused.clear();   // the slot now holds a state this build made
          flush_save();
        }
        refresh_slots();
        break;
      case Menu::Result::Load:
        if (net_live) { std::fprintf(stderr, "state: not during a network session\n"); break; }
        if (save_readonly) { std::fprintf(stderr, "state: not during a replay\n"); break; }
        if (log.writing()) { std::fprintf(stderr, "state: not while recording\n"); break; }
        // A load leaves the menu too: the player wants to see where they landed.
        if (load_state_file(nds, state_path(nds, session.states_dir, menu.slot()), loaded_layout, got_layout)) {
          apply_loaded_layout();
          sd_note_toast();
          state_slot = menu.slot();
          menu.set_open(false);
          set_paused(false);
          audio.clear();
          pacer.reset();
          fs_debt_ms = 0;
          flush_save();
        } else refresh_slots();
        break;
      case Menu::Result::Launch: launch_game(menu.chosen()); break;
      case Menu::Result::Quit: input.request_quit(); break;
      case Menu::Result::Delete: {
        const std::string path = slot_path(menu.doomed_slot());
        if (std::remove(path.c_str()) != 0) std::fprintf(stderr, "state: cannot delete %s: %s\n", path.c_str(), std::strerror(errno));
        else std::fprintf(stderr, "state: deleted %s\n", path.c_str());
        if (menu.doomed_slot() == Menu::kAutoSlot) { std::string png = path; png.replace(png.size() - 3, 3, "png"); std::remove(png.c_str()); }
        if (menu.doomed_slot() == menu.slot()) g_state_refused.clear();
        refresh_slots();
        break;
      }
      // Same guards as a load.
      case Menu::Result::Reset:
        if (net_live) { std::fprintf(stderr, "reset: not during a network session\n"); break; }
        if (save_readonly) { std::fprintf(stderr, "reset: not during a replay\n"); break; }
        if (log.writing()) { std::fprintf(stderr, "reset: not while recording\n"); break; }
        if (from_list) {
          std::fprintf(stderr, "reset: back to the firmware\n");
          g_restart = true;
          input.request_quit();
          break;
        }
        // Same as the power-off path below, but no autosave.
        std::fprintf(stderr, "reset\n");
        flush_save();
#if DSPERATE_JIT
        if (jit) ds::jit::flush_all();
#endif
        nds.reset();
        if (!boot_firmware || nds.dsi) nds.setup_direct_boot();
        if (nds.dsi && dsi_title_lo && !dsi_menu) nds.dsi_autoload(dsi_title_lo);
        mp_ever = false;
#if DSPERATE_CHEEVOS
        if (cheevos_on) cheevos.reset();
#endif
        sram_writes_seen = nds.cart ? nds.cart->sram_writes() : 0;
        menu.set_open(false);
        display.set_page(false);
        set_paused(false);
        audio.clear();
        pacer.reset();
        fs_debt_ms = 0;
        break;
      }
  };
  // The emulation thread takes its core; the audio, SDL and driver threads
  // started so far go to the aux core, and once more after the first frames
  // for the ones started lazily.
  ds::place_current_thread(ds::ThreadRole::Emu);
  ds::place_foreign_threads();
  while (!input.quit() && !g_signalled && (frame_limit == 0 || frames < static_cast<u64>(frame_limit))) {
    if (frames == 120) ds::place_foreign_threads();
    SDL_Event e;
    input.set_menu_open(menu.open());
    while (SDL_PollEvent(&e)) input.handle(e, display, dual_window ? &display2 : nullptr);
    for (ds::sdl::Action a : input.take_actions()) {
      using A = ds::sdl::Action;
      switch (a) {
      case A::Pause:
        // `paused` is false throughout a network session's menu visit, so
        // this tests "is the menu up", not "is the machine stopped".
        if (paused || menu.open()) {
          state_slot = menu.slot();
          if (menu.cheats_dirty()) { session.save_enabled(nds); menu.clear_cheats_dirty(); }
          menu.set_open(false);
          display.set_page(false);
          set_paused(false);
        }
        else pause_pending = true;
        break;
      case A::VolumeUp: audio.set_volume(audio.volume() + 10); audio.set_muted(false); VLOG("volume %d%%\n", audio.volume()); break;
      case A::VolumeDown: audio.set_volume(audio.volume() - 10); VLOG("volume %d%%\n", audio.volume()); break;
      case A::Mute: audio.set_muted(!audio.muted()); VLOG("%s\n", audio.muted() ? "muted" : "unmuted"); break;
      // Anything moving the screens while paused must recomposite the menu.
      case A::Fullscreen: display.toggle_fullscreen(); if (dual_window) display2.toggle_fullscreen(); menu_dirty = true; break;
      case A::LayoutNext: case A::LayoutPrev: {
        if (dual_window) break;
        Disp::Layout l = display.current_layout();
        const int n = static_cast<int>(layout_cycle.size());
        int at = 0;
        for (int i = 0; i < n; ++i) if (layout_cycle[static_cast<size_t>(i)] == l.mode) { at = a == A::LayoutNext ? (i + 1) % n : (i + n - 1) % n; break; }
        l.mode = layout_cycle[static_cast<size_t>(at)];
        display.set_layout(l);
        apply_visibility();
        menu_dirty = true;
        VLOG("layout: %s\n", Disp::mode_name(l.mode));
        if (!session.game_ini.empty()) ds::sdl::Config::store(session.game_ini, "video.layout", Disp::mode_name(l.mode));
        break;
      }
      case A::ScreenSwap: {
        if (dual_window) {
          vs.layout.primary = 1 - vs.layout.primary;
          display.set_only_screen(vs.layout.primary);
          display2.set_only_screen(1 - vs.layout.primary);
          menu_dirty = true;
          if (!session.game_ini.empty()) ds::sdl::Config::store(session.game_ini, "video.screen", vs.layout.primary ? "bottom" : "top");
          break;
        }
        Disp::Layout l = display.current_layout();
        l.primary = 1 - l.primary;
        display.set_layout(l);
        apply_visibility();
        menu_dirty = true;
        if (!session.game_ini.empty()) ds::sdl::Config::store(session.game_ini, "video.screen", l.primary ? "bottom" : "top");
        break;
      }
      case A::PipCornerNext: {
        if (dual_window) break;
        Disp::Layout l = display.current_layout();
        l.corner = static_cast<Disp::Corner>((static_cast<int>(l.corner) + 1) % static_cast<int>(Disp::Corner::Count));
        display.set_layout(l);
        menu_dirty = true;
        if (!session.game_ini.empty()) ds::sdl::Config::store(session.game_ini, "video.pip_corner", Disp::corner_name(l.corner));
        break;
      }
      case A::Screenshot: if (paused) screenshot(nds, session.shots_dir, display.current_layout()); else shot_pending = true; break;
      case A::Lid: input.set_lid(!input.lid()); VLOG("lid: %s\n", input.lid() ? "closed" : "open"); if (input.lid()) flush_save(); break;
      case A::SlotNext: state_slot = (state_slot + 1) % 10; show_slot(std::to_string(state_slot)); VLOG("state slot %d\n", state_slot); break;
      case A::SlotPrev: state_slot = (state_slot + 9) % 10; show_slot(std::to_string(state_slot)); VLOG("state slot %d\n", state_slot); break;
      case A::SaveState:
        // A state freezes the machine mid-conversation, but nothing restores a network session.
        if (net_live) { std::fprintf(stderr, "state: not during a network session\n"); break; }
        if (save_readonly) { std::fprintf(stderr, "state: not during a replay\n"); break; }
        if (save_state_file(nds, state_path(nds, session.states_dir, state_slot), display.current_layout())) {
          flush_save();   // the .sav and the state never diverge
          show_slot("STATE " + std::to_string(state_slot) + " SAVED");
        }
        break;
      case A::LoadState:
        if (net_live) { std::fprintf(stderr, "state: not during a network session\n"); break; }
        if (save_readonly) { std::fprintf(stderr, "state: not during a replay\n"); break; }
        if (log.writing()) { std::fprintf(stderr, "state: not while recording\n"); break; }
        if (load_state_file(nds, state_path(nds, session.states_dir, state_slot), loaded_layout, got_layout)) {
          apply_loaded_layout();
          audio.clear();
          pacer.reset();
          fs_debt_ms = 0;
          flush_save();
          show_slot("STATE " + std::to_string(state_slot) + " LOADED");
          sd_note_toast();
        }
        break;
      // Without DS_FPS, restart the window or the first number averages over the whole gap.
      case A::FpsToggle:
        fps_osd = !fps_osd;
        if (fps_osd && !show_fps) { fps_mark = SDL_GetPerformanceCounter(); emu_ticks = draw_ticks = wait_ticks = 0; }
        break;
      case A::FastForwardToggle:
        if (net_live) { std::fprintf(stderr, "fast forward: not during a network session\n"); break; }
        ff_toggle = !ff_toggle; VLOG("fast forward %s\n", ff_toggle ? "on" : "off");
        break;
      default: break;
      }
    }
    if (paused) {
      // Nothing runs behind the menu, so composite only on a change.
      pump_menu();
      if (menu.open() && (menu_dirty || menu.dirty())) {
        menu_dirty = false;
        menu.clear_dirty();
        // Dual-window: `display` is opened with only_screen 0, so it's the
        // top screen. Otherwise the layout's primary screen.
        const int menu_screen = dual_window ? 0 : display.current_layout().primary;
        const u32* src[2] = {nds.gpu.framebuffer(0), nds.gpu.framebuffer(1)};
        const u32* fb[2];
        for (int i = 0; i < 2; ++i) {
          std::memcpy(menu_fb[i].data(), src[i], menu_fb[i].size() * 4);
          ds::sdl::dim_framebuffer(menu_fb[i].data(), static_cast<u32>(menu_fb[i].size()));
          fb[i] = menu_fb[i].data();
        }
        // Where panel pixels are writable, drawn there directly. Otherwise
        // a 256x192 scratch goes through the scaler like a frame.
        const bool on_canvas = display.canvas_capable();
        if (!on_canvas) menu.draw(ds_canvas(menu_fb[menu_screen].data()));
        ds::sdl::Display::Target target[2] = {};
        if (dual_window) sync_dual_modes(display, display2);
        bool scaled = display.begin_frame(target);
        if (dual_window) scaled = display2.begin_frame(target) && scaled;
        if (scaled) {
          set_scale_targets(target, true, true);
          for (int i = 0; i < 2; ++i) nds.gpu.scale_image(i, menu_fb[i].data());
          display.finish_views();
          if (dual_window) display2.finish_views();
          ds::sdl::Display::CanvasView cv;
          if (on_canvas && display.canvas(cv)) {
            menu.draw(ds::sdl::Canvas{cv.px, cv.pitch, cv.w, cv.h});
            display.note_canvas_draw_all();
          }
          display.present();
          if (dual_window) display2.present();
          // No frame follows until the menu changes again, so a lazily
          // flipping tier must be pushed now, or the menu shows one press late.
          display.flush();
          if (dual_window) display2.flush();
          set_scale_targets(target, false);
        } else {
          ds::sdl::Display::CanvasView cv;
          if (on_canvas && display.canvas(cv)) {
            menu.draw(ds::sdl::Canvas{cv.px, cv.pitch, cv.w, cv.h});
            display.note_canvas_draw_all();
          }
          display.draw(fb);
          if (dual_window) display2.draw(fb);
          // GPU present pipelines a frame deep: flush now, same as above.
          display.flush();
          if (dual_window) display2.flush();
        }
      }
#if DSPERATE_CHEEVOS
      // Paused: no frame to evaluate, but idle() still works the queue (pending unlock, token refresh).
      if (cheevos_on) { cheevos.idle(); cheevos_catch_up(); cheevos_show(); cheevos_menu_follow(); }
#endif
      SDL_Delay(10);
      continue;
    }
    // Menu over a running game (a network session): machine still runs, so
    // the game is handed a frame with nothing pressed.
    const bool menu_over_live = menu.open();
    if (menu_over_live) { pump_menu(); menu_dirty = menu_dirty || menu.dirty(); }
    input.update_stylus();
    ds::input::Frame in = input.frame();
    if (menu_over_live) {
      const bool lid_was = in.lid;
      in = ds::input::Frame{};
      in.lid = lid_was;   // hinge is console state, not an input
    }
    if (log.reading()) {
      if (!log.read(in)) break;   // the controls still quit; the log ends the run
      ds::input::apply(nds, in);
    } else {
      bool closed;
      if (lid.poll(closed)) { input.set_lid(closed); if (closed) flush_save(); }
      in.lid = input.lid();
      if (input.fake_mic()) input.fake_mic_frame(mic);
      else {
        if (nds.io.mic_used()) open_mic();
        if (mic_alsa.active()) mic_alsa.capture(mic_raw);
        else if (audio.capturing()) mic_raw = audio.capture();
        else mic_raw.clear();
        for (s16& v : mic_raw) {
          dc += (v - dc) * 0.005;
          const double y = (v - dc) * mic_gain;
          v = static_cast<s16>(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
        }
        // Capture arrives in bursts; queue and hand out a frame at a time,
        // dropping backlog so a stall doesn't turn into latency.
        mic_queue.insert(mic_queue.end(), mic_raw.begin(), mic_raw.end());
        if (mic_queue.size() > mic_per_frame * 4) mic_queue.erase(mic_queue.begin(), mic_queue.end() - mic_per_frame * 2);
        const size_t take = mic_queue.size() < mic_per_frame ? mic_queue.size() : mic_per_frame;
        mic.assign(mic_queue.begin(), mic_queue.begin() + take);
        mic_queue.erase(mic_queue.begin(), mic_queue.begin() + take);
        if (mic_gate > 0 && !mic.empty()) {
          double sq = 0;
          for (s16 v : mic) sq += double(v) * v;
          const double rms = std::sqrt(sq / mic.size());
          mic_floor = rms < mic_floor ? rms : mic_floor + (rms - mic_floor) * 0.002;   // drops at once, creeps up
          if (rms < mic_floor * mic_gate + 16) std::fill(mic.begin(), mic.end(), 0);
        }
      }
      if (static const bool mic_log = std::getenv("DS_MIC_LOG") != nullptr; mic_log) {
        static u32 mframes = 0, mtotal = 0; static int mpeak = 0; static double msq = 0;
        for (s16 v : mic) { msq += double(v) * v; if (std::abs(int(v)) > mpeak) mpeak = std::abs(int(v)); }
        mtotal += static_cast<u32>(mic.size());
        if (++mframes == 60) {
          std::fprintf(stderr, "[mic] capture: %u samples/60 frames, peak %d, rms %.0f\n", mtotal, mpeak, mtotal ? std::sqrt(msq / mtotal) : 0.0);
          mframes = mtotal = 0; mpeak = 0; msq = 0;
        }
      }
      ds::input::decimate_mic(in, mic.data(), mic.size());
      if (log.writing()) log.write(in);
      ds::input::apply(nds, in, mic.data(), mic.size());
    }

    // With per-scanline scaling the core writes straight into the
    // panel-sized texture, so the scale cost lands inside run_frame()
    // rather than present; DS_FPS's emu/draw split shifts accordingly.
    bool fast = ff_toggle || input.fast_forward_held();
    if (net_live && fast) {
      static bool said = false;
      if (!said) { said = true; std::fprintf(stderr, "fast forward: not during a network session\n"); }
      fast = false;
    }
    if (fast != was_fast) { was_fast = fast; pacer.reset(); }
    // Policy decides for the frame after the one about to run; what the
    // core settled for this one governs the present.
    if (fs_limit > 0) {
      const int period = nds.gpu.display_phase_period();
      // Limit counts blocks, not frames: on a game whose screens take a
      // whole period to come round, frameskip=3 skips three periods per
      // drawn one, the same ratio a period-1 game gets.
      const int max_blocks = fs_limit;
      if (period > 1 && !fs_period_warned) {
        fs_period_warned = true;
        std::fprintf(stderr, "frameskip: this game drives its screens over %d frames, so it skips %d of every %d\n",
                     period, fs_limit * period, (fs_limit + 1) * period);
      }
      if (fs_left == 0) {                         // a block ended: pick the next one
        // Fixed skips its blocks whatever the clock says; adaptive skips only
        // while it is behind, and both stop at the limit and draw a period.
        const bool want = !fs_adaptive || fs_debt_ms > frame_budget_ms * 0.5;
        const bool skip = max_blocks > 0 && want && fs_blocks < max_blocks;
        fs_blocks = skip ? fs_blocks + 1 : 0;
        fs_in_skip = skip;
        fs_left = period;
      }
      --fs_left;
      nds.gpu.set_frame_skip(fs_in_skip);
    }
    const bool skipped = nds.gpu.will_skip_frame();
    // Games that render one screen per frame (the other via capture) show one
    // stale screen right after a skip; draw the block through and present its last frame.
    const int fs_period = fs_limit > 0 ? nds.gpu.display_phase_period() : 1;
    if (skipped) { ++fs_skipped; fs_drawn_run = 0; } else ++fs_drawn_run;
    const bool fs_partial = fs_period > 1 && fs_drawn_run < fs_period;
    // Say why frameskip is doing nothing, once, rather than looking broken.
    if (!skipped && fs_in_skip && ++fs_refused == 120 && !fs_capture)
      std::fprintf(stderr, "frameskip: this game display-captures its frames, which cannot be skipped exactly;\n"
                           "           drop --no-frameskip-capture (or [emu] frameskip_capture = true) to skip them anyway\n");
    const bool present = pause_pending || shot_pending ? !skipped
                                       : (!skipped && !fs_partial && (!fast || ff_skip <= 0 || frames % static_cast<u64>(ff_skip + 1) == 0));
    // Translucent PiP inset comes up to opaque while touched, holds
    // pip_touch_hold frames, then fades back. Only when the inset IS the bottom screen.
    {
      const Disp::Layout& l = display.current_layout();
      const int rest = static_cast<int>(l.pip_alpha * 255.0 + 0.5);
      const bool inset_is_bottom = l.mode == Disp::Mode::Pip && l.primary == 0;
      if (inset_is_bottom && (in.down || input.stylus_visible())) pip_hold = pip_touch_hold;
      else if (pip_hold > 0) --pip_hold;
      const int want = pip_hold > 0 && pip_touch_hold > 0 ? 255 : rest;
      pip_alpha += std::clamp(want - pip_alpha, -PIP_FADE_STEP, PIP_FADE_STEP);
      display.set_inset_alpha(static_cast<u8>(pip_alpha));
    }
    ds::sdl::Display::Target target[2] = {};
    bool scaled = false;
    if (present && !pause_pending && !shot_pending) {
      // Scanout tiers block here until a buffer is free (DS_FPS "wait").
      const Uint64 tw = SDL_GetPerformanceCounter();
      if (dual_window) sync_dual_modes(display, display2);
      scaled = display.begin_frame(target);
      if (dual_window) scaled = display2.begin_frame(target) && scaled;
      wait_ticks += SDL_GetPerformanceCounter() - tw;
    }
    set_scale_targets(target, scaled);
    fb_current = present && !scaled;   // a skipped frame renders nothing

    const Uint64 t0 = SDL_GetPerformanceCounter();
#if DSPERATE_NET
    if (lan) lan->process();   // ENet and discovery, once per frame, on this thread
    if (slirp) slirp->process();
    // Edge-triggered: the game switching its radio on is when we look too.
    {
      const bool on_air = nds.io.wifi.tx_frames() > 0;
      if (on_air && !radio_was_on && guest_retry_armed && !scanning) begin_guest_scan();
      radio_was_on = on_air;
      if (scanning) {
        lan->scan_step();
        if (SDL_GetTicks() - scan_began_ms >= kScanMs) finish_guest_scan();
      }
    }
    // LAN session: spread the frame's emulation across its period in 1 ms
    // slices ending just before the pacer's deadline, so a peer's CMD is
    // answered within a slice.
    if (lan && !fast) {
      const Uint64 frame_end = std::max(pacer.next(), SDL_GetPerformanceCounter() - static_cast<Uint64>(frame_ns * ticks_per_ns / 2)) + static_cast<Uint64>(frame_ns * ticks_per_ns);
      constexpr u64 slice = ds::ARM9_CLOCK_HZ / 1000;
      constexpr int slices = static_cast<int>(ds::CYCLES_PER_FRAME / slice) + 1;
      for (int k = 1; !nds.run_frame_slice(slice); ++k) {
        const Uint64 due = frame_end - static_cast<Uint64>(frame_ns * ticks_per_ns * (slices - k) / slices);
        const Uint64 now = SDL_GetPerformanceCounter();
        if (due > now) { const double ms = (due - now) / (ticks_per_ns * 1e6); if (ms >= 0.5) SDL_Delay(static_cast<Uint32>(ms)); }
      }
    } else
#endif
    nds.run_frame();
#if DSPERATE_CHEEVOS
    // Exactly once per emulated frame: rcheevos keeps a delta per memory
    // reference, so a second call collapses delta onto current, stopping
    // single-frame edge triggers. Also drains HTTP completions.
    if (cheevos_on) {
      cheevos_apply_state();
      cheevos.frame();
      cheevos_catch_up();
      cheevos_show();
      cheevos_menu_follow();
    }
#endif
    toast_step();

    if (!shortcuts_note.empty()) {
      Toast t;
      t.header = "NAND SHORTCUTS";
      t.title = std::move(shortcuts_note);
      t.frames = TOAST_INFO_FRAMES;
      toast_queue.push_back(std::move(t));
      shortcuts_note.clear();
    }
    // DSP is not emulated: say so on screen once per start.
    if (nds.dsi_dsp_started != dsp_warned) {
      dsp_warned = nds.dsi_dsp_started;
      if (dsp_warned) {
        std::fprintf(stderr, "dsi: this title uses the DSi's DSP, which DSperate does not emulate; it will likely not work\n");
        Toast t;
        t.header = "DSI DSP";
        t.title = "GAME LIKELY WON'T WORK";   // toast lines hold about 24 characters
        t.detail = "DSP IS NOT EMULATED";
        t.frames = TOAST_FRAMES * 2;
        toast_queue.push_back(std::move(t));
      }
    }

    if (nds.exit_requested) {
      std::fprintf(stderr, "dsi: the title has left (a soft reset); quitting\n");
      flush_save();
      input.request_quit();
      nds.exit_requested = false;
    }
    if (nds.power_off) {
      if (boot_firmware) {
        std::string err;
        if (fw_override.empty()) {
          // DSi mode: nothing is kept (see fw_override)
        } else if (nds.firmware_override_dirty() && !nds.save_firmware_override(fw_override, err))
          std::fprintf(stderr, "firmware settings: cannot save %s: %s\n", fw_override.c_str(), err.c_str());
        else if (nds.firmware_override_dirty())
          std::fprintf(stderr, "firmware settings: saved to %s\n", fw_override.c_str());
        std::fprintf(stderr, "power off: rebooting the firmware\n");
        autosave_now();       // the reset below discards the session
#if DSPERATE_JIT
        if (jit) ds::jit::flush_all();
#endif
        nds.reset();          // clears power_off, and re-seeds the clock
        if (nds.dsi) nds.setup_direct_boot();   // the DSi boots from its NAND again
        mp_ever = false;
      } else {
        nds.power_off = false;
      }
    }

    const Uint64 t1 = SDL_GetPerformanceCounter();
    if (present) {
      const bool cursor = input.stylus_visible() && !log.reading();
      // Crosshair is drawn in DS pixels, so it shrinks with a small view;
      // scale back up by the reduction so it stays findable.
      int cursor_size = input.stylus_size();
      {
        const Disp::Layout& cl = display.current_layout();
        double ratio = 1.0;
        if (!dual_window && cl.mode == Disp::Mode::Pip && cl.primary == 0) ratio = cl.pip;
        else if (!dual_window && (cl.mode == Disp::Mode::DominantV || cl.mode == Disp::Mode::DominantH) && cl.primary == 0) ratio = display.dominant_ratio();
        if (ratio < 1.0) cursor_size *= std::clamp(static_cast<int>(1.0 / ratio + 0.5), 1, 4);
      }
      const bool slot_osd = slot_shown > 0;
      if (slot_shown > 0) --slot_shown;
      // FPS when on, FF while fast forwarding, both together ("FF 120").
      std::string fps_text;
      if (fast) fps_text = "FF";
      if (fps_osd) fps_text += (fast ? " " : "") + std::to_string(std::clamp(fps_value, 0, 999));
      const bool fps_field = !fps_text.empty();
      const u32 flash_alpha = flash_left > 0 ? static_cast<u32>(255 * flash_left / FLASH_FRAMES) : 0;
      if (flash_left > 0) --flash_left;
      // Overlays follow Layout::primary to stay where the player's
      // looking. Dual window has no primary, so top screen.
      const Disp::Layout& osd_l = display.current_layout();
      const int osd_screen = dual_window ? 0 : osd_l.primary;
      if (scaled) {
        // Crosshair first, still in DS pixels. Must go down before
        // finish_views(): a PiP-inset bottom screen's target is a side
        // buffer finish_views() then copies into the canvas.
        if (cursor) draw_cursor(CursorDst{target[1].px, target[1].pitch, target[1].h, target[1].xrun}, input.stylus_x(), input.stylus_y(), cursor_size);
        display.finish_views();
        if (dual_window) display2.finish_views();
        ds::sdl::Display::CanvasView cv;
        if (display.canvas_capable() && display.canvas(cv)) {
          const ds::sdl::Canvas c{cv.px, cv.pitch, cv.w, cv.h};
          // Nothing repaints the letterbox, so each overlay must note what it covered.
          const auto note = [&](const ds::sdl::Rect& r) { display.note_canvas_draw(r.x, r.y, r.w, r.h); };
          if (slot_osd) note(draw_label(c, slot_text.c_str(), false));
          if (fps_field) note(draw_label(c, fps_text.c_str(), true));
          if (menu_over_live) { menu.draw(c); display.note_canvas_draw_all(); }
          // Toast's area must be handed back even the frame it stops drawing.
          if (toast_on()) {
            draw_toast_on(c);
            toast_last = ds::sdl::toast_rect(c, toast_now.header, toast_now.title.c_str(),
                                             toast_now.detail.empty() ? nullptr : toast_now.detail.c_str(),
                                             toast_now.points);
          }
          if (toast_last.w) {
            note(toast_last);
            if (!toast_on()) toast_last = {};
          }
          if (flash_alpha) {
            draw_flash(c, flash_alpha);
            display.note_canvas_draw_all();
            ds::sdl::Display::CanvasView cv2;
            if (dual_window && display2.canvas(cv2)) {
              draw_flash(ds::sdl::Canvas{cv2.px, cv2.pitch, cv2.w, cv2.h}, flash_alpha);
              display2.note_canvas_draw_all();
            }
          }
        } else if (target[osd_screen].px) {
          const auto tgt_canvas = [](const ds::sdl::Display::Target& t) {
            return ds::sdl::Canvas{t.px, t.pitch, t.xrun ? static_cast<int>(t.xrun[ds::SCREEN_W]) : static_cast<int>(ds::SCREEN_W), static_cast<int>(t.h)};
          };
          const ds::sdl::Canvas c = tgt_canvas(target[osd_screen]);
          if (slot_osd) draw_label(c, slot_text.c_str(), false);
          if (fps_field) draw_label(c, fps_text.c_str(), true);
          if (menu_over_live) menu.draw(c);
          if (toast_on()) draw_toast_on(c);
          if (flash_alpha) for (int i = 0; i < 2; ++i) if (target[i].px) draw_flash(tgt_canvas(target[i]), flash_alpha);
        }
        display.present();
        if (dual_window) display2.present();
      } else {
        const u32* fb[2] = {nds.gpu.framebuffer(0), nds.gpu.framebuffer(1)};
        if (cursor) {
          std::memcpy(cursor_fb.data(), fb[1], cursor_fb.size() * 4);
          draw_cursor(CursorDst{cursor_fb.data(), ds::SCREEN_W, ds::SCREEN_H, nullptr}, input.stylus_x(), input.stylus_y(), cursor_size);
          fb[1] = cursor_fb.data();
        }
        // Overlay layer where there is one: labels go on the panel's own
        // pixels. Flash stays below: on the overlay it'd upload every frame.
        ds::sdl::Display::CanvasView ocv;
        const bool want_osd = slot_osd || fps_field || toast_on() || menu_over_live;
        const bool osd_on_canvas = display.canvas_capable() && want_osd && display.canvas(ocv);
        if (osd_on_canvas) {
          const ds::sdl::Canvas c{ocv.px, ocv.pitch, ocv.w, ocv.h};
          const auto note = [&](const ds::sdl::Rect& r) { display.note_canvas_draw(r.x, r.y, r.w, r.h); };
          if (slot_osd) note(draw_label(c, slot_text.c_str(), false));
          if (fps_field) note(draw_label(c, fps_text.c_str(), true));
          if (toast_on()) { draw_toast_on(c); display.note_canvas_draw_all(); }
          if (menu_over_live) { menu.draw(c); display.note_canvas_draw_all(); }
        }
        // After the cursor, so overlays on the bottom screen keep the crosshair.
        if (!osd_on_canvas && want_osd) {
          std::memcpy(osd_fb.data(), fb[osd_screen], osd_fb.size() * 4);
          const ds::sdl::Canvas od = ds_canvas(osd_fb.data());
          if (slot_osd) draw_label(od, slot_text.c_str(), false);
          if (fps_field) draw_label(od, fps_text.c_str(), true);
          if (toast_on()) draw_toast_on(od);
          if (menu_over_live) menu.draw(od);
          fb[osd_screen] = osd_fb.data();
        }
        // Last: a screen still pointing at the GPU's own buffer is copied out first.
        if (flash_alpha) for (int i = 0; i < 2; ++i) {
          if (fb[i] == nds.gpu.framebuffer(i)) { std::memcpy(flash_fb[i].data(), fb[i], flash_fb[i].size() * 4); fb[i] = flash_fb[i].data(); }
          draw_flash(ds_canvas(const_cast<u32*>(fb[i])), flash_alpha);
        }
        // A GPU present goes to the present thread on the aux core
        // (present_thread.h); DS_PRESENT_SYNC=1 keeps it here.
        static const bool present_sync = std::getenv("DS_PRESENT_SYNC") != nullptr;

        if (!present_sync && display.gpu_present() && (!dual_window || display2.gpu_present())) {
          ds::sdl::Display* const ds_[2] = {&display, &display2};
          ds::sdl::Display::draw_async(ds_, dual_window ? 2 : 1, fb);
        } else {
          // Pinned (thread layout), the emulation thread presents here as a
          // normal task: the kernel work a present queues on this core (a
          // per-CPU kworker) cannot run under an RT thread that has the core
          // to itself, so it piled up until the per-CPU RT cap (950 ms/s, no
          // runtime sharing on ROCKNIX's kernel) idled the thread -- a 20-33 ms
          // stall about once a second on the heavy titles (ST, GS:DD).
          // DS_PRESENT_RT=1 keeps RT.
          static const bool present_nort = ds::thread_layout_on() && !std::getenv("DS_PRESENT_RT");
          int rt_pol = 0; sched_param rt_sp{};
          const bool drop_rt = present_nort && pthread_getschedparam(pthread_self(), &rt_pol, &rt_sp) == 0 && (rt_pol == SCHED_RR || rt_pol == SCHED_FIFO);
          if (drop_rt) { sched_param z{}; sched_setscheduler(0, SCHED_OTHER, &z); }
          display.draw(fb);
          if (dual_window) display2.draw(fb);
          if (drop_rt) sched_setscheduler(0, rt_pol, &rt_sp);
        }
      }
    }
    // Unlimited limiter outruns the speakers like fast forward: sheds audio the same way.
    audio.push(nds, fast || limiter_off);
    if (shot_pending && present) {
      shot_pending = false;
      screenshot(nds, session.shots_dir, display.current_layout());
      flash_left = FLASH_FRAMES;   // from the next frame: this one is already on the panel and in the file
    }
    if (pause_pending && present) {
      pause_pending = false;
      // Raising the picker and the pause menu are the same stop.
      if (launching) { launching = false; menu.open_games(); }
      else {
        menu.set_slot(state_slot);
        refresh_slots();
#if DSPERATE_CHEEVOS
        if (cheevos_on) cheevos_menu.refresh();
#endif
        menu.set_open(true);
      }
      menu_dirty = true;
      menu_ms = SDL_GetTicks();
      // Under a network session the machine cannot stop, so the menu opens as an overlay.
      if (net_live) display.set_page(true);
      else set_paused(true);
    }
    // Loader cart's picker: launching the card drives both engines'
    // MASTER_BRIGHT to white (Gpu::screens_forced_white); mp_ever tells this
    // apart from Download Play booting its program, which also forces white.
    // Latched, since the white stays up.
    if (nds.io.wifi.mp_active()) mp_ever = true;
    if (launcher) {
      // On the DSi launcher only a fade after it hashed the loader's header
      // for its whitelist counts (NDS::dsi_loader_watch).
      const bool white = nds.gpu.screens_forced_white() && (!nds.dsi || nds.dsi_loader_launched);
      if (white && mp_ever) {
        // Download Play's child program: raising the list here would pause
        // the emulator, which the session can't survive.
        if (!launch_latched) {
          launch_latched = true;
          VLOG("launcher: forced white after a local-wireless session -- Download Play booting "
               "its program, not a card launch; the picker stays down\n");
        }
      } else if (white && !launch_latched) {
        launch_latched = true;
        launching = true;
        VLOG("launcher: the card was launched (frame %llu); raising the list\n", static_cast<unsigned long long>(frames));
      } else if (!white) {
        launch_latched = false;
      }
    }
    // Stop only once the fade is up, so the animation plays on the normal scaled path.
    if (launching) pause_pending = true;
    const Uint64 t2 = SDL_GetPerformanceCounter();
    emu_ticks += t1 - t0;
    draw_ticks += t2 - t1;
    draw_ticks_total += t2 - t1;
    if (static_cast<long>(frames) >= stats_from) {
      frame_ms.push_back(static_cast<double>(t1 - t0) * ticks_to_ms);
      work_ms.push_back(static_cast<double>(t2 - t0) * ticks_to_ms);
      // DS_FRAME_SERIES=<path>: "emu work start end" per frame in run order:
      // ms, then the emu slice's CLOCK_MONOTONIC bounds in ns, so a
      // `perf record --clockid monotonic` can be cut to chosen frames.
      static FILE* series = [] { const char* p = std::getenv("DS_FRAME_SERIES"); return p ? std::fopen(p, "w") : nullptr; }();
      if (series) {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const double tick_ns = ticks_to_ms * 1e6;
        const Uint64 tc = SDL_GetPerformanceCounter();   // taken with ts: maps the counter onto CLOCK_MONOTONIC
        const long long now = static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
        const long long emu_end = now - static_cast<long long>(static_cast<double>(tc - t1) * tick_ns);
        const long long start = emu_end - static_cast<long long>(static_cast<double>(t1 - t0) * tick_ns);
        std::fprintf(series, "%.3f %.3f %lld %lld\n", frame_ms.back(), work_ms.back(), start, emu_end);
      }
      // DS_HITCH_PNG=<ms>: write what is ON SCREEN for any frame whose work
      // exceeds that, named <index>-<ms>. Capped at DS_HITCH_MAX (default 12).
      static const double hitch_ms = [] { const char* p = std::getenv("DS_HITCH_PNG"); return p ? std::atof(p) : 0.0; }();
      static const long hitch_max = [] { const char* p = std::getenv("DS_HITCH_MAX"); return p ? std::atol(p) : 12; }();
      static long hitch_n = 0;
      if (hitch_ms > 0.0 && work_ms.back() > hitch_ms && hitch_n < hitch_max) {
        ++hitch_n;
        char nm[64];
        std::snprintf(nm, sizeof nm, "/hitch-%05zu-%.1fms.png", work_ms.size() - 1, work_ms.back());
        ::mkdir(session.shots_dir.c_str(), 0755);
        const std::string path = session.shots_dir + nm;
        if (write_png(nds, path, display.current_layout()))
          std::fprintf(stderr, "hitch: frame %zu at %.1f ms -> %s\n", work_ms.size() - 1, work_ms.back(), path.c_str());
      }
    }
    if (fs_adaptive && fs_limit > 0) {
      // Only real-time play has a budget to fall behind: unthrottled fast forward always is.
      const double work = static_cast<double>(t2 - t0) * ticks_to_ms;
      fs_debt_ms += (fast && ff_speed <= 0) ? 0.0 : work - frame_budget_ms;
      if (fs_debt_ms < 0) fs_debt_ms = 0;
      const double cap = frame_budget_ms * (fs_limit + 1);
      if (fs_debt_ms > cap) fs_debt_ms = cap;   // a long stall must not buy a run of skips
    }
    // Auto buffer size needs to know if the machine keeps up; fast forward
    // doesn't get a vote, since it's behind by design.
    if (audio.active() && !(fast && ff_speed <= 0))
      audio.auto_tick(static_cast<double>(t2 - t0) * ticks_to_ms <= frame_budget_ms);
    // DS_AUDIO_QUEUE=<path>: one line a frame -- depth, rate-control ppm,
    // target depth, dry count, SPU output rate.
    if (audio.active()) {
      static FILE* aq = [] { const char* p = std::getenv("DS_AUDIO_QUEUE"); return p ? std::fopen(p, "w") : nullptr; }();
      if (aq) std::fprintf(aq, "%.3f %.0f %.2f %llu %.1f\n", audio.queued_frames(), audio.rate_trim_ppm(), audio.target_frames(),
                           static_cast<unsigned long long>(audio.stats().dry), audio.input_rate());
    }
    ds::prof::frame_mark();   // marks the emu slice: present isn't in a stage, lands in "untimed" of work_ms

    const Uint64 t3 = SDL_GetPerformanceCounter();
    // base_scale is the limiter+emu.speed clock; fast forward floors it
    // rather than multiplying (ff_speed is a multiple of real time). 0 uncaps.
    const double scale = fast && ff_speed > 0 ? std::max(base_scale, static_cast<double>(ff_speed)) : base_scale;
    if ((fast && ff_speed <= 0) || limiter_off) pacer.reset();   // unthrottled: do not bank the time it gains
    else pacer.wait(scale);

    pace_ticks += SDL_GetPerformanceCounter() - t3;
    fps_pace_ticks += SDL_GetPerformanceCounter() - t3;

    // DS_CADENCE_LOG=<path>: one line per emulated frame -- frame index,
    // SWAP_BUFFERS count (30 Hz alternates 1,0,1,0; 60 Hz reads 1,1,1,1),
    // polygon count, whether presented (so frameskip isn't mistaken for a
    // cadence change). Wall time excluded, varies with the host.
    if (static const char* cad_path = std::getenv("DS_CADENCE_LOG"); cad_path) {
      static std::FILE* cad = [&] {
        std::FILE* f = std::fopen(cad_path, "w");
        if (f) std::fprintf(f, "# frame swaps polygons presented\n");
        else std::fprintf(stderr, "DS_CADENCE_LOG: cannot write %s\n", cad_path);
        return f;
      }();
      static u64 cad_prev = 0;
      if (cad) {
        const u64 now = nds.gpu3d.swap_count();
        std::fprintf(cad, "%llu %llu %u %d\n", static_cast<unsigned long long>(frames),
                     static_cast<unsigned long long>(now - cad_prev),
                     nds.gpu3d.render_polygon_count(), present ? 1 : 0);
        cad_prev = now;
        if (frames % 256 == 0) std::fflush(cad);
      }
    }
    ++frames;
    // DS_SHOT_AT=N: a screenshot after frame N, for offscreen/replay runs.
    if (static const long shot_at = [] { const char* v = std::getenv("DS_SHOT_AT"); return v ? std::atol(v) : -1L; }(); shot_at >= 0 && frames == static_cast<u64>(shot_at)) shot_pending = true;
    // DS_LAUNCH_AT=<frame>:<path>: start that game at that frame, as the picker would.
    if (static const char* la = std::getenv("DS_LAUNCH_AT"); la && std::strchr(la, ':') && frames == std::strtoull(la, nullptr, 10)) launch_game(std::strchr(la, ':') + 1);
    if (nds.cart && nds.cart->sram_dirty()) {
      if (nds.cart->sram_writes() != sram_writes_seen) { sram_writes_seen = nds.cart->sram_writes(); sram_quiet_since = frames; }
      else if (frames - sram_quiet_since >= 60) flush_save();
    }
    if (dsi_mode && nds.dsi_nand.writes != nand_exported) {
      if (nds.dsi_nand.writes != nand_writes_seen) { nand_writes_seen = nds.dsi_nand.writes; nand_quiet_since = frames; }
      else if (frames - nand_quiet_since >= 120) flush_dsi();
    }
    if (nds.dsi_sd.writes != sd_synced) {
      if (nds.dsi_sd.writes != sd_writes_seen) { sd_writes_seen = nds.dsi_sd.writes; sd_quiet_since = frames; }
      else if (frames - sd_quiet_since >= 120) flush_sd();
    }

    if ((show_fps || fps_osd) && frames % 60 == 0) {   // DS_FPS=1 and/or the on-screen counter
      const Uint64 now = SDL_GetPerformanceCounter();
      const double secs = static_cast<double>(now - fps_mark) / SDL_GetPerformanceFrequency();
      const double to_ms = 1e3 / SDL_GetPerformanceFrequency() / 60.0;
      const double fps = secs > 0 ? 60.0 / secs : 0.0;
      fps_value = fps >= 999.0 ? 999 : static_cast<int>(fps + 0.5);
      if (show_fps) {
        const double wall_ms = static_cast<double>(now - fps_mark) * to_ms;   // per frame, same unit as the rest
        std::fprintf(stderr, "%.1f fps (%.0f%%), emu %.1f ms, present %.1f ms, wait %.1f ms, pace %.1f ms (spin %.0f us), other %.1f ms, audio queued %.1f frames (%+.0f ppm, %.1f -> %u Hz)\n",
                     fps, 100.0 * fps / (ds::ARM9_CLOCK_HZ / double(ds::CYCLES_PER_FRAME)),
                     emu_ticks * to_ms, draw_ticks * to_ms, wait_ticks * to_ms, fps_pace_ticks * to_ms, pacer.spin_us(),
                     wall_ms - (emu_ticks + draw_ticks + wait_ticks + fps_pace_ticks) * to_ms, audio.queued_frames(), audio.rate_trim_ppm(),
                     audio.input_rate(), audio.rate());
        if (fs_limit > 0) std::fprintf(stderr, "  frameskip: %llu frames skipped (%s, limit %d)\n",
                                       static_cast<unsigned long long>(fs_skipped), fs_adaptive ? "adaptive" : "fixed", fs_limit);
      }
      fps_mark = now;
      emu_ticks = draw_ticks = wait_ticks = fps_pace_ticks = 0;
    }
  }

  autosave_now();
  flush_save();
  discard_session_cache();
  // Not after a DSiWare session: nothing a DSi title wrote belongs beside the DS firmware.
  if (!fw_override.empty() && nds.firmware_override_dirty() && !nds.dsi) {
    std::string err;
    if (!nds.save_firmware_override(fw_override, err)) std::fprintf(stderr, "firmware settings: cannot save %s: %s\n", fw_override.c_str(), err.c_str());
    else std::fprintf(stderr, "firmware settings: saved to %s\n", fw_override.c_str());
  }
  if (fs_limit > 0)
    VLOG("frameskip (%s, limit %d): %llu of %llu frames not drawn\n", fs_adaptive ? "adaptive" : "fixed", fs_limit,
         static_cast<unsigned long long>(fs_skipped), static_cast<unsigned long long>(frames));
  if (log.writing()) std::fprintf(stderr, "recorded %u frames to %s\n", log.frames(), record);
  // Printed only on request, unlike the headless frontend which always
  // does (its whole job is measuring). DS_PROFILE implies it: the stage
  // breakdown needs the frame series it annotates.
#if DSPERATE_NET
  if (lan) {
    VLOG("lan: reply/host waits %u, total %.1f ms, max %.1f ms, timeouts %u\n", lan->wait_count(), lan->wait_total_ms(), lan->wait_max_ms(), lan->wait_timeouts());
  }
#endif
  // `dry` is the one that matters: queue was empty when a frame looked at
  // it, so the device played silence, the gap a player hears. Drops with
  // no dry frames means fast-forwarding or a lost device, a different fault.
  if (const ds::sdl::Audio::Stats& as = audio.stats(); as.frames) {
    VLOG("audio: queue depth %.2f..%.2f frames over %llu, dry %llu, under 1 frame %llu, under 0.5 %llu, dropped %llu; device buffer %u samples (%.2f frames), ring overruns %llu\n",
         as.min_depth, as.max_depth, static_cast<unsigned long long>(as.frames), static_cast<unsigned long long>(as.dry),
         static_cast<unsigned long long>(as.under_one), static_cast<unsigned long long>(as.under_half),
         static_cast<unsigned long long>(as.dropped), audio.device_samples(), audio.device_buffer_frames(),
         static_cast<unsigned long long>(nds.spu.ring_overruns()));
  }
  if (pacer.rt_relief()) {
    unsigned relief_frames = 0;
    const double relief_us = pacer.relief_us(&relief_frames);
    VLOG("rt relief: %u of %llu frames gave time back, %.0f us each on those\n",
         relief_frames, static_cast<unsigned long long>(frames), relief_us);
  }
  if (std::getenv("DS_FRAME_STATS") || ds::prof::enabled) {
    // Emulation work only; excluded costs named on their own line. Under
    // DS_PROFILE the probes' own cost comes off first.
    ds::prof::deduct_probe_overhead(frame_ms, &work_ms);
    ds::frame_report(frame_ms);
    // Same statistics over emulation + present: what a missed display
    // frame actually is. Only worth reading with --no-vsync (with vsync
    // the present blocks and the tail pins to the refresh).
    ds::frame_report(work_ms, "work");
    ds::prof::frame_breakdown(frame_ms);
    if (!frame_ms.empty())
      std::fprintf(stderr, "  (emulation only; excluded: present %.1f ms, pacing %.1f ms total over %zu frames)\n",
                   static_cast<double>(draw_ticks_total) * ticks_to_ms,
                   static_cast<double>(pace_ticks) * ticks_to_ms, frame_ms.size());
    if (const auto ps = ds::sdl::PresentThread::get().stats(); ps.jobs)
      std::fprintf(stderr, "present thread: %llu frames, %.3f ms mean, %.3f ms max; the emulation thread waited on it %llu times, %.1f ms total, %llu over 0.5 ms\n",
                   static_cast<unsigned long long>(ps.jobs), static_cast<double>(ps.job_ns) / 1e6 / static_cast<double>(ps.jobs),
                   static_cast<double>(ps.job_max_ns) / 1e6, static_cast<unsigned long long>(ps.waits),
                   static_cast<double>(ps.wait_ns) / 1e6, static_cast<unsigned long long>(ps.long_waits));
    ds::handoff::report(stderr);
  }
  log.close();
  ds::prof::report();
  lid.close();
  mic_alsa.close();
  input.close();
  audio.close();
  display.close();
  display2.close();
  SDL_Quit();
  return 0;
}

// RESET from a game the list started goes back via re-exec: launch_game is
// a one-way trip (a DSiWare pick swaps BIOS pair, firmware, NAND, machine).
int main(int argc, char** argv) {
  const int rc = run(argc, argv);
  if (g_restart && rc == 0) {
    execv("/proc/self/exe", argv);
    std::perror("reset: exec");
    return 1;
  }
  return rc;
}
