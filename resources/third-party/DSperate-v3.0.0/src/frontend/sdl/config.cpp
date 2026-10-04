// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace ds::sdl {

namespace {

std::string trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  const size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// "section.key" -> {section, key}
void split_key(const std::string& key, std::string& sec, std::string& name) {
  const size_t dot = key.find('.');
  sec = dot == std::string::npos ? "" : key.substr(0, dot);
  name = dot == std::string::npos ? key : key.substr(dot + 1);
}

// Keys of the GPU 3D raster, which is gone: still read so an old file loads,
// but only to say they do nothing now.
bool retired(const std::string& key) {
  for (const char* k : {"video.gpu_raster", "video.gpu_defer", "video.internal_res", "video.smooth3d", "emu.gpu_irq_avoid"})
    if (key == k) return true;
  return false;
}

} // namespace

std::string Config::dir() {
  std::string base;
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) base = x;
  else if (const char* h = std::getenv("HOME"); h && *h) base = std::string(h) + "/.config";
  else base = ".";
  ::mkdir(base.c_str(), 0755);
  const std::string d = base + "/dsperate";
  ::mkdir(d.c_str(), 0755);
  ::mkdir((d + "/games").c_str(), 0755);
  return d;
}

std::string Config::game_path_code(const char code[4]) {
  std::string c;
  for (int i = 0; i < 4; ++i) c += (code[i] >= 0x20 && code[i] < 0x7f && code[i] != '/') ? code[i] : '_';
  return dir() + "/games/" + c + ".ini";
}

std::string Config::game_path_rom(const std::string& rom) {
  std::string base = rom.substr(rom.find_last_of('/') + 1);
  if (const size_t dot = base.find_last_of('.'); dot != std::string::npos && dot > 0) base.resize(dot);
  if (base.empty()) return "";
  return dir() + "/games/" + base + ".ini";
}

bool Config::load(const std::string& path) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line, section;
  while (std::getline(f, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') { section = trim(line.substr(1, line.size() - 2)); continue; }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    if (const size_t c = v.find_first_of("#;"); c != std::string::npos) v = trim(v.substr(0, c));
    const std::string key = section.empty() ? k : section + "." + k;
    if (retired(key)) { std::fprintf(stderr, "config: %s is retired (3D is drawn on the CPU only) and is ignored (%s)\n", key.c_str(), path.c_str()); continue; }
    kv_[key] = v;
  }
  return true;
}

std::string Config::str(const std::string& key, const std::string& def) const {
  const auto it = kv_.find(key);
  return it == kv_.end() ? def : it->second;
}

int Config::num(const std::string& key, int def) const {
  const auto it = kv_.find(key);
  if (it == kv_.end() || it->second.empty()) return def;
  return static_cast<int>(std::strtol(it->second.c_str(), nullptr, 0));
}

double Config::real(const std::string& key, double def) const {
  const auto it = kv_.find(key);
  if (it == kv_.end() || it->second.empty()) return def;
  return std::atof(it->second.c_str());
}

bool Config::flag(const std::string& key, bool def) const {
  const auto it = kv_.find(key);
  if (it == kv_.end()) return def;
  const std::string& v = it->second;
  if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
  if (v == "0" || v == "false" || v == "no" || v == "off") return false;
  return def;
}

bool Config::store(const std::string& path, const std::string& key, const std::string& value) {
  std::string sec, name;
  split_key(key, sec, name);
  std::vector<std::string> lines;
  {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) lines.push_back(line);
  }
  size_t sec_start = std::string::npos, sec_end = lines.size();
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string t = trim(lines[i]);
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
      if (sec_start != std::string::npos) { sec_end = i; break; }
      if (trim(t.substr(1, t.size() - 2)) == sec) sec_start = i;
    }
  }
  const std::string entry = name + " = " + value;
  if (sec_start == std::string::npos) {
    if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
    lines.push_back("[" + sec + "]");
    lines.push_back(entry);
  } else {
    bool done = false;
    for (size_t i = sec_start + 1; i < sec_end; ++i) {
      const std::string t = trim(lines[i]);
      if (t.empty() || t[0] == '#' || t[0] == ';') continue;
      const size_t eq = t.find('=');
      if (eq != std::string::npos && trim(t.substr(0, eq)) == name) { lines[i] = entry; done = true; break; }
    }
    if (!done) {
      size_t at = sec_end;
      while (at > sec_start + 1 && trim(lines[at - 1]).empty()) --at;
      lines.insert(lines.begin() + static_cast<long>(at), entry);
    }
  }
  std::ofstream f(path);
  if (!f) return false;
  for (const std::string& l : lines) f << l << '\n';
  return true;
}

void Config::write_default(const std::string& path, bool force) {
  if (!force && std::ifstream(path)) return;
  std::ofstream f(path);
  if (!f) return;
  f <<
R"(# DSperate settings. Command-line flags override this file. Two files next
# to it override it for one game: games/<rom name>.ini (the ROM's filename
# without .nds) and games/<GAMECODE>.ini (the four letters printed as
# "game: ... [XXXX]" at start); the filename one wins, and is where a layout
# picked with the hotkey is remembered.
# Every line is a comment showing the default; delete the # to change one.

[user]                          # what games see as the console's owner when no firmware dump is
                                # given (with a dump, its own settings apply), and the DSi's
                                # settings when DSiWare runs without a NAND
# nickname = DSperate           # up to 10 characters
# message =                     # up to 26 characters
# birthday_month = 1
# birthday_day = 1
# colour = 0                    # 0 grey, 1 brown, 2 red, 3 pink, 4 orange, 5 yellow, 6 lime,
                                # 7 green, 8 dark green, 9 turquoise, 10 blue, 11 dark blue,
                                # 12 purple, 13 violet, 14 magenta, 15 dark pink
# language = 1                  # 0 Japanese, 1 English, 2 French, 3 German, 4 Italian, 5 Spanish
                                # (DSi: 6 Chinese, 7 Korean; a language the title's region lacks
                                # falls back to that region's own)

[paths]
# bios9 = /path/to/bios9.bin    # unset or missing, a built-in replacement BIOS (FreeBIOS) runs
# bios7 = /path/to/bios7.bin    # games, with a note at start; it cannot boot the DS menu and
                                # its timing is not hardware accurate
# firmware = /path/to/firmware.bin  # unset or missing, a generated firmware carrying [user] above
# bios9i = /path/to/biosdsi9.bin  # the DSi BIOS pair: DSiWare (a .nds/.dsi/.srl/.cia, loose or zipped,
# bios7i = /path/to/biosdsi7.bin  # marked [DSi] in the game list or named on the command line) runs on
                                # the DSi machine and needs both; without them it is refused
# dsi_nand = /path/to/nand.bin  # --dsi-mode's NAND (a dump with its nocash footer) when --dsi-nand
                                # is not given. Only --dsi-mode uses it (or a game named
                                # BootMenuDSi.nds, which is the same thing without a command line):
                                # DSiWare named on the command line or picked from the game list
                                # still runs without the NAND
# dsi_firmware = /path/to/dsifirmware.bin  # the DSi's firmware, used instead of firmware above
                                # whenever the machine is a DSi (--dsi-mode, DSiWare) and --firmware
                                # is not given; unset or missing, firmware above as before
# dsi_font = /path/to/TWLFontTable.dat  # --dsi-mode without a NAND: a console's DSi system font
                                # (/sys/TWLFontTable.dat) instead of DSperate's own, which is used
                                # when this is unset; [user] above is the DSi's settings there too
# dsi_sd = /path/to/folder        # the DSi's SD card: this folder, as a card the size of its contents
                                # plus 128 MB (at most 32 GB); what the DSi writes, adds or removes
                                # on the card is carried back into the folder
# firmware_override = /path/to/firmware.bin.ovr
                                # settings changed inside the firmware's own menu (nickname, colour,
                                # language...) are kept here, never in the dump. Delete it to reset.
                                # Default: <firmware>.ovr
# games = /path/to/games        # the library the loader card lists on a firmware boot (.nds and .zip,
                                # one level; DSiWare .nds/.dsi/.srl/.cia too, zipped or not, marked
                                # [DSi]). Unset, the picker says so.
# dsi_games = /path/to/folder   # the DSi games folder (e.g. a frontend's own DSiWare folder): where
                                # [emu] dsi_nand_shortcuts keeps its .dspr.nds files. Unset: games
                                # above. The loader card lists the shortcuts from here too
# saves = /path/to/saves        # battery saves. Default: next to the ROM
# states = /path/to/states      # save states (and the autosave's PNG). Default: next to the ROM
# screenshots = /path/to/shots  # the screenshot hotkey. Default: the states directory
# cache = /path/to/cache        # where zipped games are unpacked when they cannot be unpacked beside
                                # the zip (a read-only card). Default: <zip dir>/.dsperate/. Kept
                                # between runs (see [cart]); never /tmp, which is RAM on a handheld
# cheats = /path/to/usrcheat.dat # Action Replay database (the file, or a directory holding
                                #   one). Default: beside the ROM, then in this directory.
                                #   Loading it lists the codes; none are on until enabled

[loader]
# The card the DS menu shows on a firmware boot, and the DSi Menu under
# --dsi-mode (or a game named BootMenuDSi.nds, the flag without a command
# line). Launching it raises the game list. Built in; a BootMenu.nds next
# to this file replaces it (tools/mkcart.py makes one with your own icon). The
# DSi Menu refuses the card once its launch fade is over ("An error has
# occurred"), which the list covers.
# title = Game Menu             # first banner line
# subtitle = Dariragan! Dagozuban!   # second line; empty for one line
# card = true                   # false boots the firmware with the slot empty

[video]
# scale = 2                     # window size, in DS pixels
# fullscreen = false
# layout = vertical             # vertical | horizontal | single | pip | dominant_v | dominant_h
# layout_cycle = vertical,horizontal,single,pip,dominant_v,dominant_h   # what the layout hotkeys step through
# screen = top                  # top | bottom: the screen shown alone (single), large (pip) or dominant;
                                # with dual_window, the one on the upper panel
# pip_corner = br               # tl | tr | bl | br: where the pip inset sits
# pip_scale = 0.33              # inset size relative to the large screen
# pip_alpha = 1.0               # inset opacity at rest, 0..1
# pip_touch_hold = 60           # frames the inset stays opaque after the bottom screen was touched; 0 = never fades
# dominant_ratio = auto         # auto: the dominant screen takes the largest whole scale that leaves the other
                                # at least dominant_threshold of it, and the other grows into the room left;
                                # or a number, the smaller screen's size relative to the dominant one (0.5)
# dominant_threshold of it, and the other grows into the room left
# dominant_threshold = 0.25     # auto: the smallest secondary it accepts, 0.1..0.99
# dual_window = false           # one window per panel (dual-screen handhelds). DS_DUAL_SCREENS in the
                                # environment sets each panel's SDL display and touch devices, e.g.
                                # "upper=1,lower=0:goodix" (touch by name substring or #N; "none=<touch>"
                                # ignores one). Touch devices' names are printed on first touch
# screen_gap = 0                # pixels between the two screens (vertical, horizontal, dominant_*); the menu offers
                                # 0-128 and auto, the file takes any whole number and a negative one overlaps
                                # them. auto: the pair is fitted with no gap and pushed to the panel's edges, the
                                # spare room along the pair going between the screens
# integer_scale = off           # off | under | over: whole panel pixels per DS pixel -- under letterboxes at the
                                # largest that fits, over crops at the smallest that covers (the edge between
                                # the screens is kept: a stacked pair crops its outer edges, a dual-window top
                                # screen keeps its bottom row and the bottom screen its top)
# linear = false                # bilinear filter instead of nearest (on the A30 display-engine tier: the
                                # driver's own scaler filter). Overrides lcd_grid, seam and chunky
# lcd_grid = 0                  # LCD pixel grid: one dark seam per DS pixel, strength 0 (off) .. 1 (black)
# seam = dark                   # dark: the grid above | blend: only the panel pixel straddling two DS
                                # pixels is blended ("sharp shimmerless") | blend_linear: the same in
                                # linear light
# chunky = false                # draw blocks of DS pixels as one flat cell, for panels at odd scales:
                                # mean (or true) | extreme | mode | tl | min | max | false. The A30
                                # display-engine tier draws the cells in its scaler: mean only
# chunky_threshold = 180        # extreme only: luma distance (0..255) an outlier needs to win over the mean
# chunky_cell = auto            # panel pixels per cell: auto (4..16, whatever divides the screen) | pair
                                # (2x2 DS pixels) | N (or the nearest size below N that divides the screen)
# aa = false                    # 3D anti-aliasing: on the CPU raster the hardware's edge blend (edge
                                # pixels blended by coverage, when the game asks for it), on the GPU
                                # raster 4x MSAA. The older mode words accurate / enhanced read as true.
# gpu3d = false                 # draw the 3D layer on the GPU (Vulkan, 1x, MSAA with aa): the polygon
                                # list goes to the GPU after each swap and the compositor reads the
                                # finished frame, at most one frame late under load. Frees the CPU of the
                                # 3D raster; the CPU raster stays the exact reference and the fallback
                                # where there is no Vulkan. Pairs with gpu_present = rga on the RK3566,
                                # which keeps the GPU for the 3D layer alone.
# sink = auto                   # where frames go. auto picks per device: the display engine's hardware
                                # scaler where /dev/disp answers (Miyoo A30 and other Allwinner boards),
                                # /dev/fb0 on the H700 handhelds' mali-only SDL2, KMS page flips on a bare
                                # console, zero-copy dma-bufs under Wayland, else SDL's window surface or
                                # renderer. Or name one: disp | fbdev | kms | dmabuf | surface | renderer
                                # (one that won't open falls back to the renderer). The older disp = and
                                # fbdev = keys are still read, and say so
# gpu_present = true            # true | false | rga | vulkan. true: the panel is scaled and composed off
                                # the CPU by the RGA (the Rockchip 2D accelerator, through V4L2: leaves the
                                # GPU to the 3D layer) where the device has one, else by Vulkan (a compute
                                # dispatch into the scanout dma-buf). rga / vulkan: that one only. Declines
                                # cleanly to the CPU scanline scaler where neither works (no dma-buf
                                # scanout, no Vulkan, no RGA).
# sink = auto                   # where frames go. auto picks per device: the display engine's hardware
                                # scaler where /dev/disp answers (Miyoo A30 and other Allwinner boards),
                                # /dev/fb0 on the H700 handhelds' mali-only SDL2, KMS page flips on a bare
                                # console, zero-copy dma-bufs under Wayland, else SDL's window surface or
                                # renderer. Or name one: disp | fbdev | kms | dmabuf | surface | renderer
                                # (one that won't open falls back to the renderer). The older disp = and
                                # fbdev = keys are still read, and say so
# gpu_present = true            # true | false | rga. rga: the Rockchip 2D accelerator (V4L2 rockchip-rga)
                                # scales into the scanout buffer instead, leaving the GPU to the 3D layer.
                                # true: scale and compose the panel on the GPU (Vulkan, into the scanout
                                # dma-buf) instead of on the CPU scanline scaler. Default on where the
                                # device has it; declines cleanly and falls back to the scanline tier
                                # where it does not (no Vulkan, no DRM or no dma-heap). Measured on the
                                # RG DS Plus: over-budget frames 72->42 % on Spirit Tracks, 47->28 % on
                                # Golden Sun, 44->32 % on NSMB, with the longest stutter falling from
                                # 291 frames to 3-9. Known cost: one 37-80 ms frame per ~1600 on Golden
                                # Sun, the Mali tiler-heap stall
# vsync = true
# fps = false                   # frames-per-second counter in a corner of the primary screen. Counts
                                # presented frames, so frameskip and fast forward show. The fps hotkey
                                # toggles it (unbound by default)

[audio]
# enabled = true
# volume = 100                  # 0..100
# driver = pipewire             # SDL audio backend tried first (pipewire, alsa, pulseaudio...); empty =
                                # SDL's choice; SDL_AUDIODRIVER overrides
# native_rate = true            # open the device at its own rate and resample here (false: 32768 Hz,
                                # the sound daemon resamples)
# buffer_size = auto           # how much sound is held ahead: auto, or milliseconds (20..200).
                                # Lower is less delay and less slack before a late frame is heard as
                                # a gap. auto starts at 50 ms and raises it only when the machine is
                                # keeping up and still ran the queue dry -- depth answers an
                                # occasional hitch, never a machine that cannot keep up, where the
                                # fix is frameskip. Replaces latency_frames, which
                                # counted whole DS frames (~16.7 ms each) and is still read here
# mic = true                    # open the microphone
# mic_dev = plughw:0,0          # ALSA capture device
# mic_gain = 0.25
# mic_gate = 5

[cart]
# cache_mb = 2048               # unpacked zipped games kept per directory, in MB; the least recently
                                # launched go first when a new one would not fit. 0 = keep all
# cache = keep                  # keep: unpacked games stay for the next launch | session: deleted at
                                # exit, for a card with no room to spare
# preload = auto                # auto | true | false: read the whole ROM into memory when it loads
                                # (pages stay reclaimable) instead of as the game touches it. auto does
                                # it for a ROM on a network share (NFS, SMB, sshfs and other FUSE
                                # mounts) that fits with 128 MB to spare, where a page read mid-frame
                                # is a round trip and shows as a hitch; local storage stays demand-paged

[emu]
# realtime = rr                 # rr | fifo | off: real-time scheduling for the emulator's threads
#                               # (needs root or an rtprio limit)
# rt_priority = 5
# rt_relief = true              # with realtime on: the kernel caps the real-time class at
                                # sched_rt_runtime_us of every sched_rt_period_us (95 % of a second
                                # on a stock distro) and enforces it in one lump, stopping the
                                # thread dead for up to 50 ms. A scene with no slack to sleep in
                                # wears that as a hitch about every second. This gives the same 5 %
                                # back ~0.8 ms at a time instead. false = let the kernel take it
# pacing = auto                 # auto | sleep | busy: how the wait for the next frame is spent.
                                # A governor that sets the CPU clock from how busy the last few
                                # milliseconds looked (ondemand, conservative, powersave) reads
                                # that sleep as an idle machine and clocks down under the
                                # emulator; busy holds the core to the deadline instead, which
                                # costs a core's idle power. auto = busy only where such a
                                # governor is in charge. No system setting is changed either way
# host_cores = 0                # cores the emulator's threads size themselves for; 0 = detect
# thread_layout = auto          # auto (on with 4+ cores) | off: pin each thread role to a core --
                                # emulation on its own, the 2D engines each paired with a 3D band,
                                # audio/SDL/driver threads on the interrupt core (DS_PIN overrides)
# jit = true                    # false = interpreter (much slower; for comparison)
# timing = fast                 # CPU cycle model: fast (the default: a constant cost per memory access
                                # and per jump; much cheaper to emulate, and games cannot tell in
                                # practice) | exact (melonDS's per-access model: memory wait states,
                                # fetch/data overlap, the ARM7's main-RAM rules; also the cartridge
                                # bus clock per ROM word, which fast skips). Read at launch
# idle_skip = 1                 # skip a CPU busy-wait: 0 = never | 1 = only the GXSTAT swap poll |
                                # all = every proven poll loop
# autosave = false              # on quit, save a state to the hidden "auto" slot
                                # (<states>/<GAMECODE>.auto.dss). Covers Ctrl-C and a launcher's
                                # SIGTERM, not SIGKILL. Resume with autoload below (or --load-state
                                # <that path>); the slot never shows in the menu. Skipped during a
                                # replay or recording
# dsi_nand_shortcuts = false    # a <title>.dspr.nds file in paths.dsi_games (else paths.games) for
                                # each DSiWare title on paths.dsi_nand, named in plain ASCII (a FAT
                                # card may take nothing else): opening one starts the title straight from the NAND
                                # (its saves and settings), skipping the DSi Menu and its slow load.
                                # Kept up to date at every start; false removes every .dspr.nds there
# dsi_hide_installed = false    # --dsi-mode: hide the NAND dump's own DSiWare from the DSi Menu for
                                # the session (the dump is never changed); --dsi-hide-installed
# autoload = false              # when a game starts and its auto slot exists, resume from it instead
                                # of booting. Applies to a game named on the command line and to one
                                # launched from the loader cart's picker; never to a cart-less
                                # firmware boot, and never during a replay or recording. --load-state
                                # wins over it, and an auto state that cannot be read is a warning,
                                # not a failure: the game boots as usual. The state is kept, so a
                                # SIGKILL still leaves the last clean exit's state to resume from
# autosave_png = false          # a picture with the auto state, laid out as the window shows it:
                                # true = <states>/<GAMECODE>.auto.png | a path = that file (for a
                                # launcher's game switcher). Taken from the emulator's own frame, so
                                # it works on panels a screen grabber cannot read
# fast_forward = false          # start fast-forwarding (the hotkeys toggle it)
# limiter = auto                # the rate the emulator is held to: auto (the console's own
                                # 59.8261 Hz -- the default, and the only exact one), 30, 60 (0.29 %
                                # fast, in step with a 60 Hz panel), 120, 144, 240, or off
                                # (no limiter: still presents and still plays sound)
# speed = 100                   # percent of that rate the game runs at (25..400)
# ff_speed = 0                  # fast forward runs at least this many times real time (a floor,
                                # not a multiple of the limiter); 0 = unlimited whatever the limiter says
# ff_skip = 3                   # while fast-forwarding, present one frame in ff_skip+1
# frameskip = 0                 # skip drawing up to N frames in a row (0 = off). The game runs
                                # exactly as usual, only the drawing is skipped. A frame the game
                                # display-captures is always drawn: it reads those pixels back
# frameskip_mode = adaptive     # adaptive: skip only while behind real time, up to `frameskip` |
                                # fixed: always skip that many of every frameskip+1
# frameskip_capture = true      # INEXACT, and on by default: skip display-capture frames too. The
                                # game then reads back an older frame than the hardware would, so a
                                # capture-driven effect can visibly lag. Set it to false for exactness
                                # -- but a game that captures every frame (Pokemon B/W, Golden Sun)
                                # then never skips, and frameskip does nothing at all on it, which is
                                # why this is on. Skipping runs in whole display periods: on a game
                                # that draws its screens on alternate frames, frameskip = 3 skips six
                                # of every eight

# DS buttons: a b x y l r start select up down left right.
# Keyboard values are SDL key names ("x", "Return", "Right Shift", "F5").
[keys]
# a = x
# b = z
# x = s
# y = a
# l = q
# r = w
# start = Return
# select = Right Shift
# up = Up
# down = Down
# left = Left
# right = Right

# Controller values are SDL controller button names (a b x y back guide
# start leftstick rightstick leftshoulder rightshoulder dpup dpdown dpleft
# dpright) or axes (+leftx -lefty +righttrigger ...). SDL names buttons by
# position, so the DS's A is the pad's "b".
# These aliases are accepted anywhere an SDL name is, and say the same thing
# without having to remember whose letters SDL used:
#   north east south west  = y b a x   (where the button sits, not its letter)
#   select func            = back guide
#   l1 r1 l2 r2 l3 r3      = leftshoulder rightshoulder +lefttrigger
#                            +righttrigger leftstick rightstick
# The menu shows every binding in these terms; the file is always written back
# in SDL's names, so a binding made in the menu and one typed here agree.
[pad]
# a = b
# b = a
# x = y
# y = x
# l = leftshoulder
# r = rightshoulder
# start = start
# select = back
# up = dpup
# down = dpdown
# left = dpleft
# right = dpright
# stick_dpad = left             # left | right | none: a stick that also works the d-pad
                                # (true/false are the old spellings of left/none)
# stick_face = none             # none | left | right: a stick that also works the face buttons,
                                # laid out as the DS has them (up X, down B, left Y, right A).
                                # The pen's stick (stylus_axis) keeps the pen in both cases.
# stick_deadzone = 12000
# face_fix = auto               # auto: if the controller's SDL mapping has a/b or x/y swapped against
                                # the kernel's button positions, correct it (logged at startup); off: trust SDL
# axis_leftx = leftx            # Axis remapping, for a pad whose mapping reports a stick rotated,
# axis_lefty = lefty            # mirrored or on the wrong axis: each axis names the physical one
# axis_rightx = rightx          # that feeds it, with a leading - to invert it, or none. E.g. a left
# axis_righty = righty          # stick turned a quarter: axis_leftx = lefty, axis_lefty = -leftx.
# axis_lefttrigger = lefttrigger    # Everything else here (bindings, stick_dpad, the pen) sees the
# axis_righttrigger = righttrigger  # remapped axes. The Controls page sets a row by pushing the stick
                                    # right (X) or down (Y), or pressing the trigger.
# stylus_axis = right           # right | left | none: the stick that moves the pen over the
                                # bottom screen (left takes the stick away from the d-pad)
# stylus_dpad = none            # a button; while held the d-pad, and the stick_dpad stick, move the pen (e.g. leftshoulder)
# stylus_button = rightstick    # touches at the pen's position; mod+<it> is free for a hotkey
# stylus_button.alt = none      # a second tap control, like a hotkey's .alt (e.g. +righttrigger)
# stylus_speed = 4.0            # pen pixels per frame at full tilt
# stylus_size = 2               # crosshair scale: arm width and centre dot, DS pixels (scaled up by the
                                # view's reduction when the bottom screen is the PiP inset / dominant secondary)
# stylus_hide = 90              # frames idle before the crosshair hides (0 = never shown)

# Hotkeys: quit pause fast_forward (held) fast_forward_toggle save_state
# ("pause" opens the pause menu: it stops the machine and draws save state,
# load state and the slot list over the held frame, driven by the DS buttons)
# load_state slot_next slot_prev volume_up volume_down mute layout_next
# layout_prev screen_swap pip_corner_next fullscreen screenshot lid mic (held) fps. A value is a key name, or
# "mod+name" meaning the modifier must be held with it; "none" unbinds.
# "mod" alone fires when the modifier is released with nothing else pressed
# while it was held (pause = mod, quit = mod+start); one-shot actions only.
# Every action also takes a second control, "<action>.alt" (pause.alt = F1,
# padhotkeys.lid.alt = mod+dpup), which fires the same action; it is unset
# unless you set it. DS buttons have one binding each.
[hotkeys]
# modifier = none               # keyboard modifier, e.g. "Left Ctrl"
# quit = Escape
# pause = p
# fast_forward = Tab
# fast_forward_toggle = none
# save_state = F5
# load_state = F7
# slot_next = F3
# slot_prev = F2
# volume_up = =
# volume_down = -
# mute = 0
# layout_next = F4
# layout_prev = F10
# screen_swap = F6              # which screen is alone / large / dominant (or first)
# pip_corner_next = F8
# fullscreen = f
# screenshot = F9
# lid = l
# mic = m
# fps = none                    # the on-screen frames-per-second counter

# The same on the controller. The modifier is the pad's mode/home button
# ("guide"); if it doubles as a DS button it is withheld from the game
# while held and delivered as a tap when released alone, unless a hotkey
# is "mod". A chord
# "mod+start+back" needs both buttons (SDL calls the DS Select "back").
[padhotkeys]
# modifier = guide
# quit = mod+start+back
# pause = mod+start
# fast_forward = mod++righttrigger
# save_state = mod+rightshoulder
# load_state = mod+leftshoulder
# slot_next = mod+dpright
# slot_prev = mod+dpleft
# volume_up = none
# volume_down = none
# layout_next = mod+back
# layout_prev = mod+x
# screen_swap = mod+y
# pip_corner_next = none
# screenshot = none
# mic = leftstick
# fps = none

[cheevos]
# RetroAchievements, Casual mode (formerly Softcore). Hardcore is NOT supported!
# enabled = false
# username =                    # who to sign in as (--cheevos-user overrides it for one run). The password is never stored in plaintext;
                                # a successful sign-in keeps the token RetroAchievements returns, in
                                # <config>/cheevos.token, mode 0600
# token_file =                  # a token file another program wrote, in PPSSPP's format (the token
                                # alone). --cheevos-token sets this. It wins over the stored sign-in
                                # and is never written to; the username still comes from above
# use_system_login = true       # when DSperate has no sign-in of its own, use the one the CFW's front
                                # end already made.
# toasts = true                 # show a panel at the bottom of the screen when an achievement unlocks,
                                # and for sign-in and connection messages. Off leaves them in the log
# encore = false                # allow achievements to be triggered again even if already achieved.
                                # For replaying a game and watching the set unlock as it goes. Nothing
                                # is credited twice, so this changes what DSperate shows, not what the
                                # account holds. Read when a game loads, so it takes effect next launch
# auto_screenshot = false       # take a screenshot when an achievement is acquired. Goes to the
                                # screenshots directory named like any other, and shows the game rather
                                # than the notice over it
)";
}

} // namespace ds::sdl
