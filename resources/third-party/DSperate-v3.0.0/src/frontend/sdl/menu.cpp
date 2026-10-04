// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/menu.h"
#include "core/io/io.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <iterator>

namespace ds::sdl {
namespace {

// 5x7 glyphs, one byte per row, bits 4..0 left to right; ' ' (0x20) to ']'
// (0x5D) -- brackets are there for the cheats page's checkboxes.
constexpr u8 kFont[92][7] = {
  { 0, 0, 0, 0, 0, 0, 0}, { 4, 4, 4, 4, 4, 0, 4}, {10,10, 0, 0, 0, 0, 0}, {10,10,31,10,31,10,10},
  { 4,15,20,14, 5,30, 4}, {24,25, 2, 4, 8,19, 3}, { 8,20,20, 8,21,18,13}, { 4, 4, 0, 0, 0, 0, 0},
  { 2, 4, 8, 8, 8, 4, 2}, { 8, 4, 2, 2, 2, 4, 8}, { 0, 4,21,14,21, 4, 0}, { 0, 4, 4,31, 4, 4, 0},
  { 0, 0, 0, 0, 4, 4, 8}, { 0, 0, 0,31, 0, 0, 0}, { 0, 0, 0, 0, 0,12,12}, { 1, 1, 2, 4, 8,16,16},
  {14,17,19,21,25,17,14}, { 4,12, 4, 4, 4, 4,14}, {14,17, 1, 2, 4, 8,31}, {31, 2, 4, 2, 1,17,14},
  { 2, 6,10,18,31, 2, 2}, {31,16,30, 1, 1,17,14}, { 6, 8,16,30,17,17,14}, {31, 1, 2, 4, 8, 8, 8},
  {14,17,17,14,17,17,14}, {14,17,17,15, 1, 2,12},
  { 0,12,12, 0,12,12, 0}, { 0,12,12, 0,12, 4, 8}, { 2, 4, 8,16, 8, 4, 2}, { 0, 0,31, 0,31, 0, 0},
  { 8, 4, 2, 1, 2, 4, 8}, {14,17, 1, 2, 4, 0, 4}, {14,17,23,21,23,16,14},
  {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14}, {28,18,17,17,17,18,28},
  {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16}, {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
  {14, 4, 4, 4, 4, 4,14}, { 7, 2, 2, 2, 2,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
  {17,27,21,21,17,17,17}, {17,17,25,21,19,17,17}, {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
  {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17}, {15,16,16,14, 1, 1,30}, {31, 4, 4, 4, 4, 4, 4},
  {17,17,17,17,17,17,14}, {17,17,17,17,17,10, 4}, {17,17,17,21,21,27,17}, {17,17,10, 4,10,17,17},
  {17,17,10, 4, 4, 4, 4}, {31, 1, 2, 4, 8,16,31},
  { 6, 4, 4, 4, 4, 4, 6}, {16,16, 8, 4, 2, 1, 1}, {12, 4, 4, 4, 4, 4,12},
  // Face-button positions, indices 62..65 (\x01..\x04, Menu::kFaceSouth and
  // friends). Diamond of four pips with the named one filled.
  { 0, 4, 0,17,14,14,14}, { 0, 4, 3,19, 3, 4, 0}, { 0, 4,24,25,24, 4, 0}, {14,14,14,17, 0, 4, 0},
  // Lower case, indices 66..91. Rest of the menu folds a-z onto capitals;
  // only the name editor needs true case (n vs N for a password). Reached
  // through glyph_cased(), never glyph().
  { 0, 0,14, 1,15,17,15}, {16,16,22,25,17,17,30}, { 0, 0,14,16,16,17,14},   // a b c
  { 1, 1,13,19,17,17,15}, { 0, 0,14,17,31,16,14}, { 6, 9, 8,28, 8, 8, 8},   // d e f
  { 0, 0,15,17,15, 1,14}, {16,16,22,25,17,17,17}, { 4, 0,12, 4, 4, 4,14},   // g h i
  { 2, 0, 6, 2, 2,18,12}, {16,16,18,20,24,20,18}, {12, 4, 4, 4, 4, 4,14},   // j k l
  { 0, 0,26,21,21,21,21}, { 0, 0,22,25,17,17,17}, { 0, 0,14,17,17,17,14},   // m n o
  { 0, 0,30,17,30,16,16}, { 0, 0,13,19,15, 1, 1}, { 0, 0,22,25,16,16,16},   // p q r
  { 0, 0,15,16,14, 1,30}, { 8, 8,28, 8, 8, 9, 6}, { 0, 0,17,17,17,19,13},   // s t u
  { 0, 0,17,17,17,10, 4}, { 0, 0,17,17,21,21,10}, { 0, 0,17,10, 4,10,17},   // v w x
  { 0, 0,17,17,15, 1,14}, { 0, 0,31, 2, 4, 8,31}};                          // y z

constexpr int kGlyphW = 5, kGlyphH = 7, kAdvance = 6;   // advance includes the one-pixel gap

// Panel geometry, derived at draw time from the canvas rather than fixed for
// a 256x192 one. Rows are `row_h` tall around a `glyph_px` glyph so the
// selection bar sits evenly above and below its text; every measure is a
// multiple of the glyph scale, so scale 2 (a 256x192 canvas) reproduces the
// old fixed constants exactly.
struct Metrics {
  int s;            // glyph scale for ordinary rows
  int list_s;       // ... and for the scrolling list pages
  int glyph_px, row_h, title_y, rule_y, rows_y, pad;
  int list_row_h, list_rows_y;
};

// Metrics for an explicit glyph scale. Pages that must shrink to fit step
// this down directly: deriving a smaller scale by shrinking the canvas and
// re-asking does not always converge.
Metrics metrics_for(int s) {
  Metrics m{};
  m.s = std::clamp(s, 2, 10);
  // Cheat names are sentences and a game can have thousands, so list pages
  // drop to half scale (about thirty characters across, twelve at a time).
  m.list_s = std::max(1, m.s / 2);
  m.glyph_px = kGlyphH * m.s;
  m.row_h    = m.glyph_px + 2 * m.s;
  m.title_y  = 4 * m.s;
  m.rule_y   = m.title_y + m.glyph_px + 3 * m.s;
  m.rows_y   = m.rule_y + 4 * m.s;
  m.pad      = 4 * m.s;
  m.list_row_h  = kGlyphH * m.list_s + 4 * m.list_s;
  m.list_rows_y = m.rule_y + 3 * m.s;
  return m;
}

Metrics metrics(const Canvas& d) { return metrics_for(ui_scale(d)); }

int panel_height(const Metrics& m, int rows) { return m.rows_y + rows * m.row_h + m.pad; }
// List pages take most of the canvas: rows they cannot show are rows the player scrolls to.
int list_panel_w(const Canvas& d, const Metrics& m) { return std::min(d.w - m.pad, 120 * m.s); }
int list_panel_h(const Canvas& d, const Metrics& m) { return std::min(d.h - m.pad, 89 * m.s); }
int list_visible(const Metrics& m, int panel_h) {
  return std::max(1, (panel_h - m.list_rows_y - m.pad) / m.list_row_h);
}

int glyph(char c) {
  // Face-position pips live below the printable range rather than after ']':
  // the free codes there overlap 'a', which the fold below would swallow.
  if (c >= 1 && c <= 4) return 62 + c - 1;
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  const int i = static_cast<int>(static_cast<unsigned char>(c)) - 0x20;
  return (i >= 0 && i < 62) ? i : 0;
}

// Keeps a lower-case letter lower case, for the name editor only.
int glyph_cased(char c) {
  if (c >= 'a' && c <= 'z') return 66 + (c - 'a');
  return glyph(c);
}

// Text arrives as UTF-8 but the font is ASCII: a Latin letter with a mark
// folds onto the letter it decorates ("Ōkamiden"); anything else draws '?'.
// Returns the next glyph's ASCII code and advances `p` past its bytes.
char next_char(const char*& p) {
  const unsigned char b0 = static_cast<unsigned char>(*p++);
  if (b0 < 0x80) return static_cast<char>(b0);
  int len = b0 >= 0xF0 ? 4 : b0 >= 0xE0 ? 3 : b0 >= 0xC0 ? 2 : 0;
  if (len == 0) return '?';                        // stray continuation byte
  u32 cp = b0 & (0x7F >> len);
  for (int i = 1; i < len; ++i, ++p) {
    const unsigned char b = static_cast<unsigned char>(*p);
    if ((b & 0xC0) != 0x80) return '?';            // truncated sequence: leave the tail for the next glyph
    cp = (cp << 6) | (b & 0x3F);
  }
  // Latin-1 Supplement, U+00C0..U+00FF.
  static constexpr char kLatin1[64 + 1] =
      "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs"
      "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";
  // Latin Extended-A, U+0100..U+017F.
  static constexpr char kLatinA[128 + 1] =
      "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiIiJjKkkLlLlLlL"
      "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";
  if (cp >= 0xC0 && cp <= 0xFF) return kLatin1[cp - 0xC0];
  if (cp >= 0x100 && cp <= 0x17F) return kLatinA[cp - 0x100];
  return '?';
}

// Drops the last code point, so trimming for width never leaves a lead byte behind.
void pop_char(std::string& s) {
  while (!s.empty()) {
    const unsigned char b = static_cast<unsigned char>(s.back());
    s.pop_back();
    if ((b & 0xC0) != 0x80) break;
  }
}

// Columns outside [clip_x0, clip_x1) are dropped, letting a name scroll
// under the panel edge instead of over it; draw sets this around a
// scrolling row, default admits everything.
int g_clip_x0 = 0, g_clip_x1 = 1 << 30;

void fill_rect(const Canvas& d, int x, int y, int w, int h, u32 colour) {
  const int x0 = std::max({x, 0, g_clip_x0}), x1 = std::min({x + w, d.w, g_clip_x1});
  const int y0 = std::max(y, 0), y1 = std::min(y + h, d.h);
  for (int yy = y0; yy < y1; ++yy) {
    u32* row = d.px + static_cast<size_t>(yy) * d.pitch;
    for (int xx = x0; xx < x1; ++xx) row[xx] = colour;
  }
}

constexpr u32 kInk = 0xFFFFFFFF, kDim = 0xFF909090, kPanel = 0xFF101018, kEdge = 0xFF5060A0, kSel = 0xFF3050A0;
constexpr u32 kEdgeText = 0xFFA0B0E0, kPanelEdgeDim = 0xFF303040;   // group headings; scroll-bar track
constexpr u32 kDanger = 0xFFFF5050;  // slot delete mode, slot about to go
constexpr u32 kWarn = 0xFFFFC050;   // slot row when a state was refused

// The panel every page sits in: a filled box with a one-pixel edge.
void panel(const Canvas& d, int x, int y, int w, int h) {
  fill_rect(d, x, y, w, h, kPanel);
  fill_rect(d, x, y, w, 1, kEdge);
  fill_rect(d, x, y + h - 1, w, 1, kEdge);
  fill_rect(d, x, y, 1, h, kEdge);
  fill_rect(d, x + w - 1, y, 1, h, kEdge);
}

// A scroll bar for a list page, because the list gives no other clue how long
// it is: some games have five thousand cheats, and a library can be as long.
// Drawn only when there is something off screen.
void scroll_bar(const Canvas& d, const Metrics& m, int px0, int py0, int panel_w, int visible, int n, int top) {
  if (n <= visible) return;
  const int w = std::max(2, m.list_s * 2);
  const int track_x = px0 + panel_w - w - m.list_s * 3, track_y = py0 + m.list_rows_y - m.list_s * 2;
  const int track_h = visible * m.list_row_h;
  fill_rect(d, track_x, track_y, w, track_h, kPanelEdgeDim);
  int bar = track_h * visible / n;
  if (bar < 4 * m.list_s) bar = 4 * m.list_s;
  const int span = track_h - bar;
  fill_rect(d, track_x, track_y + (span > 0 ? span * top / (n - visible) : 0), w, bar, kEdge);
}

// The root page, in order. A null label is the slot row: formatted from the
// current slot, opens the slot page rather than returning a result.
constexpr int kSlotRow = 2;
constexpr int kCheatRow = 3;   // hidden when no database matched this ROM
constexpr int kOptionsRow = 4; // hidden when the frontend gave no settings host
constexpr int kCheevosRow = 5; // hidden unless RetroAchievements is built and on
// The Options page's own entries, and which of them has no page yet.
constexpr int kOptionPages = 5, kDsOptionsRow = 4;
constexpr struct RootItem { const char* label; Menu::Result result; } kRoot[] = {
  {"SAVE STATE", Menu::Result::Save},
  {"LOAD STATE", Menu::Result::Load},
  {nullptr,      Menu::Result::None},
  {"CHEATS",     Menu::Result::None},
  {"OPTIONS",    Menu::Result::None},
  {"ACHIEVEMENTS", Menu::Result::None},
  {"RESUME",     Menu::Result::Resume},
  {"RESET",      Menu::Result::Reset},
  {"QUIT",       Menu::Result::Quit},
};
static_assert(static_cast<int>(std::size(kRoot)) == Menu::kRootRows, "kRootRows must match the table");
static_assert(kRoot[kOptionsRow].result == Menu::Result::None, "kOptionsRow must name the options row");
static_assert(kRoot[kSlotRow].label == nullptr, "kSlotRow must name the slot row");
static_assert(kRoot[kCheatRow].result == Menu::Result::None, "kCheatRow must name the cheats row");
static_assert(kRoot[kCheevosRow].result == Menu::Result::None, "kCheevosRow must name the achievements row");

} // namespace

int text_width(int scale, const char* s) {
  int n = 0;
  for (const char* p = s; *p;) { next_char(p); ++n; }   // glyphs, not bytes
  return n > 0 ? n * kAdvance * scale - scale : 0;   // no gap after the last glyph
}

int draw_text(const Canvas& d, int x, int y, int scale, u32 colour, const char* s, bool keep_case) {
  for (const char* p = s; *p;) {
    const char ch = next_char(p);
    const u8* g = kFont[keep_case ? glyph_cased(ch) : glyph(ch)];
    for (int r = 0; r < kGlyphH; ++r)
      for (int c = 0; c < kGlyphW; ++c)
        if ((g[r] >> (kGlyphW - 1 - c)) & 1)
          fill_rect(d, x + c * scale, y + r * scale, scale, scale, colour);
    x += kAdvance * scale;
  }
  return x - scale;
}

int ui_scale(const Canvas& d) {
  // Twice the canvas's reduction against a DS screen, matching the old
  // scale-2-into-256x192-then-upscale size; a 256x192 canvas lands back on 2.
  const double r = std::min(static_cast<double>(d.w) / ds::SCREEN_W, static_cast<double>(d.h) / ds::SCREEN_H);
  const int s = static_cast<int>(std::lround(2.0 * r));
  return std::clamp(s, 2, 10);
}

void dim_framebuffer(u32* px, u32 n) {
  for (u32 i = 0; i < n; ++i) px[i] = 0xFF000000 | ((px[i] >> 1) & 0x007F7F7F);
}



void Menu::set_open(bool o) {
  // Layout-affecting changes commit here, not as they're asked for, so the
  // player doesn't watch the screen jump mid-read. Every way out goes through here.
  if (!o && open_ && host_) host_->commit();
  open_ = o;
  depth_ = 1;
  stack_[0] = Page::Root;
  row_ = 0;
}

void Menu::set_cheats(std::vector<cheat::Code>* codes, const std::vector<cheat::Group>* groups) {
  codes_ = codes;
  groups_ = groups;
  lines_.clear();
}

// Hides rows with nothing behind them (cheats with no match, options with no host).
bool Menu::root_visible(int item) const {
  // RESET goes with the state rows: it ends a session just as surely.
  if (kRoot[item].result == Result::Save || kRoot[item].result == Result::Load ||
      kRoot[item].result == Result::Reset || item == kSlotRow)
    return have_states();
  if (item == kCheatRow) return have_cheats();
  if (item == kOptionsRow) return have_options();
  if (item == kCheevosRow) return have_cheevos();
  return true;
}

int Menu::root_rows() const {
  int n = 0;
  for (int i = 0; i < kRootRows; ++i) if (root_visible(i)) ++n;
  return n;
}

int Menu::root_item(int row) const {
  for (int i = 0; i < kRootRows; ++i) if (root_visible(i) && row-- == 0) return i;
  return kRootRows - 1;
}

void Menu::push(Page p) {
  if (depth_ < kMaxDepth) stack_[depth_++] = p;
}

bool Menu::pop() {
  if (depth_ <= 1) return false;
  --depth_;
  return true;
}

// Display list: a heading wherever the group changes, then every code. Notes
// keep their place so a "(M) must be on" stays with the codes it applies to.
void Menu::build_lines() {
  lines_.clear();
  if (!codes_) return;
  int last = -2;
  for (size_t i = 0; i < codes_->size(); ++i) {
    const cheat::Code& c = (*codes_)[i];
    if (c.group != last) {
      if (c.group >= 0 && groups_ && static_cast<size_t>(c.group) < groups_->size())
        lines_.push_back({Line::Heading, c.group});
      last = c.group;
    }
    lines_.push_back({c.is_note() ? Line::Note : Line::Toggle, static_cast<int>(i)});
  }
}

// Moves to the next selectable line in `delta`'s direction, stopping at the
// ends rather than wrapping.
void Menu::move_cheat_row(int delta) {
  if (lines_.empty()) return;
  const int n = static_cast<int>(lines_.size());
  int at = cheat_row_;
  for (int step = 0; step < n; ++step) {
    at += delta > 0 ? 1 : -1;
    if (at < 0 || at >= n) return;
    if (lines_[static_cast<size_t>(at)].kind == Line::Toggle) {
      cheat_row_ = at;
      // Scroll only as far as it takes to bring the selection back into view.
      if (cheat_row_ < cheat_top_) cheat_top_ = cheat_row_;
      if (cheat_row_ >= cheat_top_ + visible_) cheat_top_ = cheat_row_ - visible_ + 1;
      if (cheat_top_ > n - visible_) cheat_top_ = n - visible_;
      if (cheat_top_ < 0) cheat_top_ = 0;
      return;
    }
  }
}

void Menu::toggle_cheat() {
  if (!codes_ || lines_.empty()) return;
  const Line& l = lines_[static_cast<size_t>(cheat_row_)];
  if (l.kind != Line::Toggle) return;
  cheat::Code& c = (*codes_)[static_cast<size_t>(l.at)];
  c.enabled = !c.enabled;
  cheats_dirty_ = true;
  // In a group marked as alternatives, turning one on turns the rest off.
  if (!c.enabled || c.group < 0 || !groups_ || static_cast<size_t>(c.group) >= groups_->size()) return;
  if (!(*groups_)[static_cast<size_t>(c.group)].exclusive) return;
  for (size_t i = 0; i < codes_->size(); ++i) {
    cheat::Code& other = (*codes_)[i];
    if (i != static_cast<size_t>(l.at) && other.group == c.group) other.enabled = false;
  }
}

void Menu::open_games() {
  open_ = true;
  depth_ = 1;
  stack_[0] = Page::Games;
  game_row_ = 0;
  game_top_ = 0;
  chosen_.clear();
  marquee_ms_ = 0;
  dirty_ = true;
}

// No headings in the games list, so a plain clamped step; same scroll rule as cheats.
void Menu::move_game_row(int delta) {
  if (!games_ || games_->empty()) return;
  const int n = static_cast<int>(games_->size());
  const int at = game_row_ + delta;
  game_row_ = at < 0 ? 0 : at >= n ? n - 1 : at;
  if (game_row_ < game_top_) game_top_ = game_row_;
  if (game_row_ >= game_top_ + visible_) game_top_ = game_row_ - visible_ + 1;
  if (game_top_ > n - visible_) game_top_ = n - visible_;
  if (game_top_ < 0) game_top_ = 0;
}

int Menu::list_row() const {
  if (page() == Page::Games) return game_row_;
  // Not in list_page(): achievements has its own shoulder paging and no key
  // repeat, but does marquee, restarted on "has the selection moved".
  if (page() == Page::Cheevos) return cheevos_row_;
  if (settings_page()) return set_row_[table_slot()];
  if (controls_page()) return bind_row_;
  return cheat_row_;
}

int Menu::marquee_offset(int overflow) const {
  if (overflow <= 0) return 0;
  const u32 scroll_ms = static_cast<u32>(overflow) * 1000u / kMarqueePxPerSec;
  const u32 cycle = kMarqueeDelayMs + scroll_ms + kMarqueeHoldMs;
  const u32 t = marquee_ms_ % cycle;    // still, scroll, hold, and round again
  if (t < kMarqueeDelayMs) return 0;
  if (t < kMarqueeDelayMs + scroll_ms)
    return static_cast<int>((t - kMarqueeDelayMs) * kMarqueePxPerSec / 1000u);
  return overflow;
}

Menu::Result Menu::update(u32 presses, u32 held, u32 ms) {
  using B = io::Io::Button;
  const int was_row = list_row();
  const int before = marquee_offset(marquee_overflow_);
  if (presses || faces_) dirty_ = true;

  // Key repeat on list pages only; other pages are a handful of rows where a
  // held direction would overshoot more often than it helps.
  if (list_page()) {
    const int dir = (held & (1u << B::BTN_UP)) ? -1 : (held & (1u << B::BTN_DOWN)) ? 1 : 0;
    if (dir != repeat_dir_) { repeat_dir_ = dir; repeat_ms_ = 0; repeating_ = false; }
    else if (dir != 0) {
      repeat_ms_ += ms;
      for (u32 step = repeating_ ? kRepeatRateMs : kRepeatDelayMs;
           repeat_ms_ >= step; step = kRepeatRateMs) {
        repeat_ms_ -= step;
        repeating_ = true;
        if (page() == Page::Games) move_game_row(dir);
        else if (settings_page()) move_setting_row(dir);
        else if (controls_page()) move_bind_row(dir);
        else move_cheat_row(dir);
        dirty_ = true;
      }
    }
  } else {
    repeat_dir_ = 0; repeat_ms_ = 0; repeating_ = false;
  }

  const Result r = handle(presses);
  faces_ = 0;

  // A name only scrolls once the selection has settled on it.
  if (list_row() != was_row) marquee_ms_ = 0;
  else marquee_ms_ += ms;
  if (marquee_offset(marquee_overflow_) != before) dirty_ = true;
  return r;
}

Menu::Result Menu::handle(u32 presses) {
  using B = io::Io::Button;
  const auto hit = [&](B b) { return (presses >> b) & 1; };
  if (page() == Page::Slot) {
    // Ten slots as two columns of five: up/down walk a column, left/right
    // cross between them. slot_row_ is 0-4 left, 5-9 right; distinct from
    // row_ so popping back leaves the root page's selection untouched.
    // Delete mode: Y in and out, B out. A arms a used slot, A again deletes;
    // any other press disarms.
    const bool confirm = hit(B::BTN_A) || hit(B::BTN_START);
    if (slot_armed_ && presses && !confirm) { slot_armed_ = false; return Result::None; }   // the press that backs out does only that
    if (hit(B::BTN_Y)) {
      slot_delete_ = !slot_delete_;
      slot_armed_ = false;
      if (!slot_delete_ && slot_row_ == kAutoSlot) slot_row_ = slot_;
      return Result::None;
    }
    if (slot_delete_ && hit(B::BTN_B)) {
      slot_delete_ = false;
      if (slot_row_ == kAutoSlot) slot_row_ = slot_;
      return Result::None;
    }
    if (slot_delete_ && confirm) {
      if (!used_[slot_row_]) return Result::None;
      if (!slot_armed_) { slot_armed_ = true; return Result::None; }
      slot_armed_ = false;
      if (slot_row_ == slot_) slot_notice_.clear();
      return Result::Delete;
    }
    // Both columns are a cell longer in delete mode: the auto state's row.
    {
      int col = slot_row_ == kAutoSlot ? 0 : slot_row_ / kSlotRows;
      int r = slot_row_ == kAutoSlot ? kSlotRows : slot_row_ % kSlotRows;
      const int len = kSlotRows + (slot_delete_ ? 1 : 0);
      if (hit(B::BTN_UP))   r = (r + len - 1) % len;
      if (hit(B::BTN_DOWN)) r = (r + 1) % len;
      if ((hit(B::BTN_LEFT) || hit(B::BTN_RIGHT)) && r < kSlotRows) col ^= 1;
      slot_row_ = r == kSlotRows ? kAutoSlot : col * kSlotRows + r;
    }
    if (hit(B::BTN_B)) { pop(); return Result::None; }
    if (hit(B::BTN_A) || hit(B::BTN_START)) { slot_ = slot_row_; slot_notice_.clear(); pop(); }
    return Result::None;
  }
  if (page() == Page::Games) {
    // A is the only way off it -- see open_games() on why B is inert.
    if (hit(B::BTN_UP))   move_game_row(-1);
    if (hit(B::BTN_DOWN)) move_game_row(+1);
    if (hit(B::BTN_L)) move_game_row(-visible_);
    if (hit(B::BTN_R)) move_game_row(+visible_);
    if ((hit(B::BTN_A) || hit(B::BTN_START)) && games_ && !games_->empty()) {
      chosen_ = (*games_)[static_cast<size_t>(game_row_)].path;
      return Result::Launch;
    }
    return Result::None;
  }
  if (page() == Page::Options) return handle_options(presses);
  if (settings_page()) return handle_settings(presses);
  if (controls_page()) return handle_controls(presses);
  if (page() == Page::TextEdit) return handle_text_edit(presses);
  if (page() == Page::Cheevos) return handle_cheevos(presses);
  if (page() == Page::CheevosAccount) return handle_cheevos_account(presses);
  if (page() == Page::Cheats) {
    if (hit(B::BTN_UP))   move_cheat_row(-1);
    if (hit(B::BTN_DOWN)) move_cheat_row(+1);
    if (hit(B::BTN_L)) for (int i = 0; i < visible_; ++i) move_cheat_row(-1);
    if (hit(B::BTN_R)) for (int i = 0; i < visible_; ++i) move_cheat_row(+1);
    if (hit(B::BTN_A) || hit(B::BTN_START)) toggle_cheat();
    if (hit(B::BTN_B)) pop();
    return Result::None;
  }
  const int rows = root_rows();
  if (hit(B::BTN_UP))   row_ = (row_ + rows - 1) % rows;
  if (hit(B::BTN_DOWN)) row_ = (row_ + 1) % rows;
  // Left/right are a shortcut on the slot row.
  const int item = root_item(row_);
  if (item == kSlotRow) {
    if (hit(B::BTN_LEFT))  { slot_ = (slot_ + 9) % 10; slot_notice_.clear(); }
    if (hit(B::BTN_RIGHT)) { slot_ = (slot_ + 1) % 10; slot_notice_.clear(); }
  }
  if (hit(B::BTN_B)) return Result::Resume;
  if (!hit(B::BTN_A) && !hit(B::BTN_START)) return Result::None;
  if (item == kSlotRow) { push(Page::Slot); slot_row_ = slot_; slot_delete_ = slot_armed_ = false; return Result::None; }
  if (item == kCheatRow) {
    build_lines();
    cheat_row_ = 0;
    cheat_top_ = 0;
    if (!lines_.empty() && lines_[0].kind != Line::Toggle) move_cheat_row(+1);
    push(Page::Cheats);
    return Result::None;
  }
  if (item == kOptionsRow) { push(Page::Options); return Result::None; }
  if (item == kCheevosRow) {
    // Always the account page: answers signed-in/has-set/why-not and carries
    // the switches; the achievement list is one row away from there.
    account_row_ = 0;
    push(Page::CheevosAccount);
    return Result::None;
  }
  return kRoot[item].result;
}

// Truncated to fit, with an ellipsis.
namespace {
std::string fit(const std::string& text, int scale, int width_px) {
  if (text_width(scale, text.c_str()) <= width_px) return text;
  std::string out = text;
  while (!out.empty() && text_width(scale, (out + "...").c_str()) > width_px) pop_char(out);
  return out + "...";
}
} // namespace

void draw_notice(const Canvas& d, const char* title, const char* line2, const char* line3) {
  const Metrics m = metrics(d);
  const int w = std::min(d.w - 2 * m.pad, 116 * m.s);
  const int h = m.title_y + m.glyph_px * 2 + 3 * m.s + kGlyphH * m.list_s + 11 * m.s;
  const int x0 = (d.w - w) / 2, y0 = (d.h - h) / 2;
  panel(d, x0, y0, w, h);
  const auto centred = [&](int y, int scale, u32 ink, const char* s) {
    draw_text(d, x0 + (w - text_width(scale, s)) / 2, y, scale, ink, s);
  };
  // Long titles are cut to the panel with an ellipsis, as the game list does.
  centred(y0 + m.title_y, m.s, kInk, fit(title, m.s, w - 8 * m.s).c_str());
  centred(y0 + m.title_y + m.glyph_px + 3 * m.s, m.s, kInk, line2);
  centred(y0 + m.title_y + m.glyph_px * 2 + 6 * m.s, m.list_s, kDim, line3);
}

// An unlock, while the game is being played. Bottom-right and only as wide
// as it needs to be, unlike draw_notice's centred panel.
namespace {
// One place decides the geometry, so draw_toast and toast_rect cannot
// disagree and leave a strip of an old toast on the canvas.
struct ToastBox { Metrics m; int x0, y0, w, h, lines; };

ToastBox toast_box(const Canvas& d, const char* header, const char* title, const char* detail, u32 points) {
  ToastBox b{};
  b.m = metrics(d);
  const Metrics& m = b.m;
  std::string head = title ? title : "";
  if (points) head += "  " + std::to_string(points) + "P";
  b.lines = 1 + (header && *header ? 1 : 0) + (detail && *detail ? 1 : 0);

  // Wide enough for whatever it has to say, up to two thirds of the screen
  // (past that it starts hiding the game); truncated beyond that.
  const int want = std::max({text_width(m.list_s, head.c_str()),
                             header ? text_width(m.list_s, header) : 0,
                             detail ? text_width(m.list_s, detail) : 0});
  const int cap = std::min(d.w - 2 * m.pad, d.w * 2 / 3);
  b.w = std::clamp(want + 6 * m.list_s, std::min(40 * m.list_s, cap), cap);
  b.h = 2 * m.list_s + b.lines * m.list_row_h + 2 * m.list_s;
  // Bottom right: the DS picture is centred, so the corner is least likely
  // to cover something being read.
  b.x0 = std::max(0, d.w - b.w - 2 * m.pad);
  // One text row of clearance below, not four: the old gap floated it a whole
  // banner's height off the bottom, which read as neither anchored nor
  // centred.
  b.y0 = std::max(0, d.h - b.h - m.list_row_h);
  return b;
}
} // namespace

Rect toast_rect(const Canvas& d, const char* header, const char* title, const char* detail, u32 points) {
  const ToastBox b = toast_box(d, header, title, detail, points);
  return Rect{b.x0, b.y0, b.w, b.h};
}

void draw_toast(const Canvas& d, const char* header, const char* title, const char* detail, u32 points) {
  const ToastBox b = toast_box(d, header, title, detail, points);
  const Metrics& m = b.m;
  panel(d, b.x0, b.y0, b.w, b.h);
  std::string head = title ? title : "";
  // Points only when there are any: 0-point achievements exist and "0P" reads like a bug.
  if (points) head += "  " + std::to_string(points) + "P";
  const int avail = b.w - 4 * m.list_s;
  const int x = b.x0 + 2 * m.list_s;
  int y = b.y0 + 3 * m.list_s;
  if (header && *header) {
    draw_text(d, x, y, m.list_s, kEdgeText, fit(header, m.list_s, avail).c_str());
    y += m.list_row_h;
  }
  draw_text(d, x, y, m.list_s, kInk, fit(head, m.list_s, avail).c_str());
  if (detail && *detail)
    draw_text(d, x, y + m.list_row_h, m.list_s, kDim, fit(detail, m.list_s, avail).c_str());
}

// The scrolling pages share their frame: panel, centred title, rule, and the
// geometry every row is laid out against. `visible_` is measured here
// because only draw knows the canvas.
namespace {
struct ListFrame { Metrics m; int px0, py0, w, h, visible, text_x, avail; };

ListFrame list_frame(const Canvas& d, const char* title) {
  ListFrame f{};
  f.m = metrics(d);
  f.w = list_panel_w(d, f.m);
  f.h = list_panel_h(d, f.m);
  f.px0 = (d.w - f.w) / 2;
  f.py0 = (d.h - f.h) / 2;
  f.visible = list_visible(f.m, f.h);
  panel(d, f.px0, f.py0, f.w, f.h);
  // Fitted, not just centred, so a long title doesn't run off both edges.
  const std::string t = fit(title, f.m.s, f.w - 4 * f.m.s);
  draw_text(d, f.px0 + (f.w - text_width(f.m.s, t.c_str())) / 2, f.py0 + f.m.title_y, f.m.s, kInk, t.c_str());
  fill_rect(d, f.px0 + f.m.pad, f.py0 + f.m.rule_y, f.w - 2 * f.m.pad, std::max(1, f.m.s / 2), kEdge);
  // The scroll bar's track sits in the right margin, so rows stop short of it.
  f.text_x = f.px0 + f.m.pad;
  f.avail = f.w - 2 * f.m.pad - 6 * f.m.list_s;
  return f;
}
} // namespace

void Menu::draw_cheats(const Canvas& d) const {
  // Heading counts the codes, not the lines, to match the frontend's log.
  size_t on = 0;
  if (codes_) for (const cheat::Code& c : *codes_) if (c.enabled) ++on;
  char title[32];
  std::snprintf(title, sizeof title, "CHEATS  %zu ON", on);
  const ListFrame f = list_frame(d, title);
  const Metrics& m = f.m;
  visible_ = f.visible;

  if (lines_.empty()) {
    draw_text(d, f.text_x + m.list_s * 2, f.py0 + m.list_rows_y + m.list_s * 4, m.list_s, kDim, "NO CHEATS FOR THIS GAME");
    return;
  }

  const int n = static_cast<int>(lines_.size());
  // move_cheat_row keeps this in range; clamped again because draw must be
  // safe whatever the caller did.
  int top = cheat_top_;
  if (top > n - f.visible) top = n - f.visible;
  if (top < 0) top = 0;

  marquee_overflow_ = 0;   // set below if the selected row is actually too long
  for (int i = 0; i < f.visible && top + i < n; ++i) {
    const Line& l = lines_[static_cast<size_t>(top + i)];
    const int ry = f.py0 + m.list_rows_y + i * m.list_row_h;
    const bool sel = top + i == cheat_row_;
    if (sel) fill_rect(d, f.px0 + m.list_s * 4, ry - m.list_s * 2, f.w - m.list_s * 14, m.list_row_h, kSel);
    if (l.kind == Line::Heading) {
      const std::string& name = (*groups_)[static_cast<size_t>(l.at)].name;
      draw_text(d, f.text_x, ry, m.list_s, kEdgeText, fit(name, m.list_s, f.avail).c_str());
      continue;
    }
    const cheat::Code& c = (*codes_)[static_cast<size_t>(l.at)];
    if (l.kind == Line::Note) {
      draw_text(d, f.text_x + 4 * m.list_s, ry, m.list_s, kDim, fit(c.name, m.list_s, f.avail - 4 * m.list_s).c_str());
      continue;
    }
    const char* box = c.enabled ? "[X] " : "[ ] ";
    const u32 ink = c.enabled || sel ? kInk : kDim;
    if (!sel) {
      draw_text(d, f.text_x, ry, m.list_s, ink, fit(std::string(box) + c.name, m.list_s, f.avail).c_str());
      continue;
    }
    // The selected row scrolls its name; the checkbox stays put.
    const int box_w = text_width(m.list_s, box) + m.list_s;
    draw_text(d, f.text_x, ry, m.list_s, ink, box);
    const int name_x = f.text_x + box_w, name_avail = f.avail - box_w;
    marquee_overflow_ = text_width(m.list_s, c.name.c_str()) - name_avail;
    if (marquee_overflow_ <= 0) {
      draw_text(d, name_x, ry, m.list_s, ink, c.name.c_str());
      continue;
    }
    const int clip0 = g_clip_x0, clip1 = g_clip_x1;
    g_clip_x0 = name_x;
    g_clip_x1 = name_x + name_avail;
    draw_text(d, name_x - marquee_offset(marquee_overflow_), ry, m.list_s, ink, c.name.c_str());
    g_clip_x0 = clip0;
    g_clip_x1 = clip1;
  }

  scroll_bar(d, m, f.px0, f.py0, f.w, f.visible, n, top);
}

void Menu::draw_games(const Canvas& d) const {
  const int n = games_ ? static_cast<int>(games_->size()) : 0;
  char title[32];
  std::snprintf(title, sizeof title, "GAMES  %d", n);
  const ListFrame f = list_frame(d, title);
  const Metrics& m = f.m;
  visible_ = f.visible;

  if (n == 0) {
    const char* why[] = {"NO GAMES FOUND -- SET", "[PATHS] GAMES IN THE", "CONFIG FILE"};
    for (int i = 0; i < 3; ++i)
      draw_text(d, f.text_x + m.list_s * 2, f.py0 + m.list_rows_y + m.list_s * 4 + i * m.list_row_h, m.list_s, kDim, why[i]);
    return;
  }

  int top = game_top_;
  if (top > n - f.visible) top = n - f.visible;
  if (top < 0) top = 0;

  marquee_overflow_ = 0;
  for (int i = 0; i < f.visible && top + i < n; ++i) {
    const std::string& name = (*games_)[static_cast<size_t>(top + i)].title;
    const int ry = f.py0 + m.list_rows_y + i * m.list_row_h;
    const bool sel = top + i == game_row_;
    if (sel) fill_rect(d, f.px0 + m.list_s * 4, ry - m.list_s * 2, f.w - m.list_s * 14, m.list_row_h, kSel);
    if (!sel) {
      draw_text(d, f.text_x, ry, m.list_s, kDim, fit(name, m.list_s, f.avail).c_str());
      continue;
    }
    marquee_overflow_ = text_width(m.list_s, name.c_str()) - f.avail;
    if (marquee_overflow_ <= 0) { draw_text(d, f.text_x, ry, m.list_s, kInk, name.c_str()); continue; }
    const int clip0 = g_clip_x0, clip1 = g_clip_x1;
    g_clip_x0 = f.text_x;
    g_clip_x1 = f.text_x + f.avail;
    draw_text(d, f.text_x - marquee_offset(marquee_overflow_), ry, m.list_s, kInk, name.c_str());
    g_clip_x0 = clip0;
    g_clip_x1 = clip1;
  }
  scroll_bar(d, m, f.px0, f.py0, f.w, f.visible, n, top);
}

// ---------------------------------------------------------------------------
// The achievement pages.

// Two lines per achievement: name, then description or measured progress.
void Menu::draw_cheevos(const Canvas& d) const {
  char title[40] = "ACHIEVEMENTS";
  if (cheevos_ && cheevos_->row_count() > 0) {
    int unlocked = 0;
    for (int i = 0; i < cheevos_->row_count(); ++i) if (cheevos_->row(i).unlocked) ++unlocked;
    std::snprintf(title, sizeof title, "ACHIEVEMENTS  %d/%d", unlocked, cheevos_->row_count());
  }
  const ListFrame f = list_frame(d, title);
  const Metrics& m = f.m;
  // Two text rows per entry; list_visible() assumes one-line rows.
  const int entry_h = 2 * m.list_row_h + 2 * m.list_s;   // a little air between entries
  visible_ = std::max(1, (f.h - m.list_rows_y - m.pad) / entry_h);

  const int n = cheevos_ ? cheevos_->row_count() : 0;
  if (n == 0) {
    draw_text(d, f.text_x + m.list_s * 2, f.py0 + m.list_rows_y + m.list_s * 4, m.list_s, kDim,
              "NO ACHIEVEMENTS LOADED");
    return;
  }

  const int top = std::clamp(cheevos_top_, 0, std::max(0, n - visible_));
  marquee_overflow_ = 0;   // set below if the selected row's detail is too long
  for (int i = 0; i < visible_ && top + i < n; ++i) {
    const int at = top + i;
    const CheevosHost::Row r = cheevos_->row(at);
    const int y = f.py0 + m.list_rows_y + i * entry_h;
    if (at == cheevos_row_)
      fill_rect(d, f.px0 + m.list_s * 2, y - m.list_s * 2, f.w - m.list_s * 10, entry_h, kSel);

    // Unlocked is the thing the eye should find, so it is marked at the left
    // and drawn bright; unsupported is dimmer still than locked, because it is
    // not something the player can do anything about.
    const char* mark = r.unsupported ? "-" : (r.unlocked ? "*" : " ");
    const u32 ink = r.unsupported ? kPanelEdgeDim : (r.unlocked ? kInk : kDim);
    std::string head = std::string(mark) + " " + r.title;
    if (r.points) head += "  " + std::to_string(r.points) + "P";
    draw_text(d, f.text_x, y, m.list_s, ink, fit(head, m.list_s, f.avail).c_str());

    const std::string detail = r.unsupported ? "NOT SUPPORTED BY THIS EMULATOR YET" : r.detail;
    if (detail.empty()) continue;
    const int dx = f.text_x + 4 * m.list_s, davail = f.avail - 4 * m.list_s;
    const int dy = y + m.list_row_h;
    // Selected entry scrolls its description instead of cutting it.
    if (at != cheevos_row_) {
      draw_text(d, dx, dy, m.list_s, kPanelEdgeDim, fit(detail, m.list_s, davail).c_str());
      continue;
    }
    marquee_overflow_ = text_width(m.list_s, detail.c_str()) - davail;
    if (marquee_overflow_ <= 0) {
      draw_text(d, dx, dy, m.list_s, kPanelEdgeDim, detail.c_str());
      continue;
    }
    const int clip0 = g_clip_x0, clip1 = g_clip_x1;
    g_clip_x0 = dx;
    g_clip_x1 = dx + davail;
    draw_text(d, dx - marquee_offset(marquee_overflow_), dy, m.list_s, kPanelEdgeDim, detail.c_str());
    g_clip_x0 = clip0;
    g_clip_x1 = clip1;
  }
  scroll_bar(d, m, f.px0, f.py0, f.w, visible_, n, top);
}

// Keeps the selection on screen and stops at the ends rather than wrapping.
void Menu::move_cheevos_row(int delta) {
  const int n = cheevos_ ? cheevos_->row_count() : 0;
  if (n == 0) return;
  cheevos_row_ = std::clamp(cheevos_row_ + delta, 0, n - 1);
  const int vis = std::max(1, visible_);
  if (cheevos_row_ < cheevos_top_) cheevos_top_ = cheevos_row_;
  if (cheevos_row_ >= cheevos_top_ + vis) cheevos_top_ = cheevos_row_ - vis + 1;
  cheevos_top_ = std::clamp(cheevos_top_, 0, std::max(0, n - vis));
}

Menu::Result Menu::handle_cheevos(u32 presses) {
  using B = io::Io::Button;
  const auto hit = [&](B b) { return (presses >> b) & 1; };
  if (hit(B::BTN_UP))   move_cheevos_row(-1);
  if (hit(B::BTN_DOWN)) move_cheevos_row(+1);
  if (hit(B::BTN_L)) move_cheevos_row(-std::max(1, visible_));
  if (hit(B::BTN_R)) move_cheevos_row(+std::max(1, visible_));
  // A goes to the account; the list itself is for reading only.
  if (hit(B::BTN_A) || hit(B::BTN_START)) { account_row_ = 0; push(Page::CheevosAccount); }
  if (hit(B::BTN_B)) pop();
  return Result::None;
}

namespace {
// Account page rows, chosen at draw and handle time from the same state.
enum class AccountRow : u8 { SignIn, SignOut, List, Toasts, Screenshot, Encore };
constexpr int kAccountRows = 6;
int account_rows(const CheevosHost* h, AccountRow out[kAccountRows]) {
  int n = 0;
  if (!h) return 0;
  if (h->signed_in()) out[n++] = AccountRow::SignOut;
  else out[n++] = AccountRow::SignIn;
  if (h->has_set() && h->row_count() > 0) out[n++] = AccountRow::List;
  out[n++] = AccountRow::Toasts;
  out[n++] = AccountRow::Screenshot;
  out[n++] = AccountRow::Encore;
  return n;
}

// Switch rows draw as "LABEL        ON".
struct AccountLabel { const char* text; bool toggle; CheevosHost::Option opt; };
AccountLabel account_label(AccountRow r) {
  switch (r) {
  case AccountRow::SignOut:    return {"SIGN OUT", false, CheevosHost::Option::Toasts};
  case AccountRow::SignIn:     return {"SIGN IN", false, CheevosHost::Option::Toasts};
  case AccountRow::List:       return {"VIEW ACHIEVEMENTS", false, CheevosHost::Option::Toasts};
  case AccountRow::Toasts:     return {"UNLOCK NOTICES", true, CheevosHost::Option::Toasts};
  case AccountRow::Screenshot: return {"SCREENSHOT ON UNLOCK", true, CheevosHost::Option::Screenshot};
  // Read only when a game loads, so it can't affect one already running.
  case AccountRow::Encore:     return {"ENCORE (NEXT LAUNCH)", true, CheevosHost::Option::Encore};
  }
  return {"", false, CheevosHost::Option::Toasts};
}
} // namespace

void Menu::draw_cheevos_account(const Canvas& d) const {
  const ListFrame f = list_frame(d, "RETROACHIEVEMENTS");
  const Metrics& m = f.m;

  const std::string status = cheevos_ ? cheevos_->status() : std::string{};
  int y = f.py0 + m.list_rows_y;
  // Status can be long; wrapped over as many lines as it takes rather than cut.
  std::string rest = status;
  const int cols = std::max(8, f.avail / (kAdvance * m.list_s));
  while (!rest.empty()) {
    std::string line = rest;
    if (static_cast<int>(line.size()) > cols) {
      size_t cut = line.rfind(' ', static_cast<size_t>(cols));
      if (cut == std::string::npos || cut == 0) cut = static_cast<size_t>(cols);
      line = rest.substr(0, cut);
      rest = rest.substr(cut == static_cast<size_t>(cols) ? cut : cut + 1);
    } else {
      rest.clear();
    }
    draw_text(d, f.text_x, y, m.list_s, kDim, line.c_str());
    y += m.list_row_h;
  }

  const std::string prog = cheevos_ ? cheevos_->progress() : std::string{};
  if (!prog.empty()) {
    y += m.list_row_h / 2;
    draw_text(d, f.text_x, y, m.list_s, kEdgeText, prog.c_str());
    y += m.list_row_h;
  }
  y += m.list_row_h;

  AccountRow rows[kAccountRows];
  const int n = account_rows(cheevos_, rows);
  for (int i = 0; i < n; ++i) {
    const AccountLabel L = account_label(rows[i]);
    if (i == account_row_)
      fill_rect(d, f.px0 + m.list_s * 2, y - m.list_s * 2, f.w - m.list_s * 10, m.list_row_h, kSel);
    draw_text(d, f.text_x, y, m.list_s, kInk, L.text);
    if (L.toggle) {
      const char* state = cheevos_->option(L.opt) ? "ON" : "OFF";
      // Right-aligned, in the scroll bar's margin.
      draw_text(d, f.px0 + f.w - m.pad - text_width(m.list_s, state), y, m.list_s,
                cheevos_->option(L.opt) ? kInk : kDim, state);
    }
    y += m.list_row_h;
  }
}

Menu::Result Menu::handle_cheevos_account(u32 presses) {
  using B = io::Io::Button;
  const auto hit = [&](B b) { return (presses >> b) & 1; };
  AccountRow rows[kAccountRows];
  const int n = account_rows(cheevos_, rows);
  if (n == 0) { if (hit(B::BTN_B)) pop(); return Result::None; }
  if (hit(B::BTN_UP))   account_row_ = (account_row_ + n - 1) % n;
  if (hit(B::BTN_DOWN)) account_row_ = (account_row_ + 1) % n;
  if (hit(B::BTN_B)) { pop(); return Result::None; }
  const AccountRow at = rows[std::clamp(account_row_, 0, n - 1)];
  // Left/right toggle too, matching the settings pages and slot row.
  const AccountLabel L = account_label(at);
  if (L.toggle && (hit(B::BTN_LEFT) || hit(B::BTN_RIGHT) || hit(B::BTN_A) || hit(B::BTN_START))) {
    cheevos_->set_option(L.opt, !cheevos_->option(L.opt));
    return Result::None;
  }
  if (!hit(B::BTN_A) && !hit(B::BTN_START)) return Result::None;
  switch (at) {
  case AccountRow::SignIn:
    // Two prompts: name then password.
    open_credential_edit(EditDest::CheevosUser);
    break;
  case AccountRow::SignOut:
    cheevos_->sign_out();
    account_row_ = 0;
    break;
  case AccountRow::List:
    cheevos_row_ = 0;
    cheevos_top_ = 0;
    push(Page::Cheevos);
    break;
  case AccountRow::Toasts:
  case AccountRow::Screenshot:
  case AccountRow::Encore:
    break;   // handled above
  }
  return Result::None;
}

const Setting* Menu::table() const {
  switch (page()) {
  case Page::Emulation: return kEmuSettings;
  case Page::VisualFx:  return kVideoSettings;
  case Page::Layout:    return kLayoutSettings;
  case Page::DsOptions: return kUserSettings;
  default:              return kEmuSettings;
  }
}

int Menu::table_slot() const {
  switch (page()) {
  case Page::VisualFx:  return 1;
  case Page::Layout:    return 2;
  case Page::DsOptions: return 3;
  default:              return 0;
  }
}

// Stops at the ends rather than wrapping. A row the host has switched off
// can still be highlighted, so its reason shows in the note; it just
// does not step.
bool Menu::move_setting_row(int delta) {
  const Setting* t = table();
  const int n = settings_count(t);
  const int slot = table_slot();
  const int at = set_row_[slot] + (delta > 0 ? 1 : -1);
  if (at < 0 || at >= n) return false;
  set_row_[slot] = at;
  return true;
}

void Menu::step_setting(int dir) {
  const Setting* t = table();
  const Setting& s = t[set_row_[table_slot()]];
  if (!host_->enabled(s)) return;
  if (s.type == Setting::Type::Text) return;   // A opens the editor instead
  const std::string cur = host_->get(s.key);
  const std::string next = step_value(s, cur, dir, *host_);
  if (next == cur) return;
  host_->set(s.key, next);
}

Menu::Result Menu::handle_options(u32 presses) {
  using B = io::Io::Button;
  const auto hit = [&](B b) { return (presses >> b) & 1; };
  const int rows = kOptionPages + (host_->has_game() ? 1 : 0);
  if (hit(B::BTN_UP))   opt_row_ = (opt_row_ + rows - 1) % rows;
  if (hit(B::BTN_DOWN)) opt_row_ = (opt_row_ + 1) % rows;
  const bool save_row = host_->has_game() && opt_row_ == kOptionPages;
  // Left/right work the save-to switch in place, as they do the slot row.
  if (save_row && (hit(B::BTN_LEFT) || hit(B::BTN_RIGHT) || hit(B::BTN_A) || hit(B::BTN_START)))
    host_->set_save_per_game(!host_->save_per_game());
  if (hit(B::BTN_B)) { pop(); return Result::None; }
  if (save_row || (!hit(B::BTN_A) && !hit(B::BTN_START))) return Result::None;
  static constexpr Page kPages[kOptionPages] = {Page::Emulation, Page::VisualFx, Page::Layout, Page::Controls, Page::DsOptions};
  push(kPages[opt_row_]);
  if (page() == Page::Controls) {
    // Open on the pad column when one is plugged in; keyboard otherwise.
    bind_pad_ = host_->has_pad();
    bind_row_ = 0;
    bind_top_ = 0;
    reset_armed_ = false;
    return Result::None;
  }
  set_row_[table_slot()] = 0;
  return Result::None;
}

Menu::Result Menu::handle_settings(u32 presses) {
  using B = io::Io::Button;
  const auto hit = [&](B b) { return (presses >> b) & 1; };
  if (hit(B::BTN_UP))    move_setting_row(-1);
  if (hit(B::BTN_DOWN))  move_setting_row(+1);
  if (hit(B::BTN_LEFT))  step_setting(-1);
  if (hit(B::BTN_RIGHT)) step_setting(+1);
  // A steps a setting forward too, so the page can be worked with one button.
  if (hit(B::BTN_A) || hit(B::BTN_START)) {
    const Setting& cur = table()[set_row_[table_slot()]];
    if (cur.type == Setting::Type::Text && host_->enabled(cur)) open_text_edit();
    else step_setting(+1);
  }
  if (hit(B::BTN_B))     pop();
  return Result::None;
}

void Menu::draw_options(const Canvas& d) const {
  static constexpr const char* kItems[kOptionPages] = {"EMULATION", "VISUAL FX", "LAYOUT", "CONTROLS", "DS OPTIONS"};
  Metrics m = metrics(d);
  const int rows = kOptionPages + (host_ && host_->has_game() ? 1 : 0);
  const bool per_game = host_ && host_->save_per_game();
  char save_row[40];
  std::snprintf(save_row, sizeof save_row, "SAVE TO < %s >", per_game ? "THIS GAME" : "GLOBAL");
  // Sized for the widest row the page can ever draw, not the current one, so
  // toggling save-to doesn't resize the panel.
  static constexpr const char* kWidestSaveRow = "SAVE TO < THIS GAME >";
  const auto widest_row = [&](const Metrics& mm) {
    int w = 0;
    for (int i = 0; i < kOptionPages; ++i) w = std::max(w, text_width(mm.s, kItems[i]));
    if (rows > kOptionPages) w = std::max(w, text_width(mm.s, kWidestSaveRow));
    return w;
  };
  // Step the scale down rather than clip, as the root page does.
  const int side = 3 * m.pad;
  while (m.s > 2 && (widest_row(m) + 2 * side > d.w || panel_height(m, rows) > d.h)) m = metrics_for(m.s - 1);
  const int panel_w = std::min(d.w - 2 * m.pad, widest_row(m) + 2 * (3 * m.pad));
  const int panel_h = panel_height(m, rows);
  const int px0 = (d.w - panel_w) / 2, py0 = (d.h - panel_h) / 2;
  panel(d, px0, py0, panel_w, panel_h);
  draw_text(d, px0 + (panel_w - text_width(m.s, "OPTIONS")) / 2, py0 + m.title_y, m.s, kInk, "OPTIONS");
  fill_rect(d, px0 + m.pad, py0 + m.rule_y, panel_w - 2 * m.pad, std::max(1, m.s / 2), kEdge);
  for (int i = 0; i < rows; ++i) {
    const int ry = py0 + m.rows_y + i * m.row_h;
    if (i == opt_row_) fill_rect(d, px0 + m.pad, ry - m.s, panel_w - 2 * m.pad, m.row_h, kSel);
    if (i < kOptionPages) {
      draw_text(d, px0 + m.pad + 3 * m.s, ry, m.s, kInk, kItems[i]);
      continue;
    }
    draw_text(d, px0 + m.pad + 3 * m.s, ry, m.s, kInk, save_row);
  }
}

void Menu::draw_settings(const Canvas& d) const {
  static constexpr const char* kTitles[4] = {"EMULATION", "VISUAL FX", "LAYOUT", "DS OPTIONS"};
  const ListFrame f = list_frame(d, kTitles[table_slot()]);
  const Metrics& m = f.m;
  visible_ = f.visible;
  const Setting* t = table();
  const int n = settings_count(t);
  const int sel = set_row_[table_slot()];

  // Note under the list: two lines for the note, a third for what it takes to apply.
  const int note_lines = 2, note_rows = note_lines + 1;
  const int visible = std::max(1, f.visible - note_rows);
  int top = set_top_[table_slot()];
  if (sel < top) top = sel;
  if (sel >= top + visible) top = sel - visible + 1;
  if (top > n - visible) top = n - visible;
  if (top < 0) top = 0;
  set_top_[table_slot()] = top;

  const int value_w = f.avail / 2;
  for (int i = 0; i < visible && top + i < n; ++i) {
    const Setting& s = t[top + i];
    const int ry = f.py0 + m.list_rows_y + i * m.list_row_h;
    const bool on = host_->enabled(s);
    const bool is_sel = top + i == sel;
    if (is_sel) fill_rect(d, f.px0 + m.list_s * 4, ry - m.list_s * 2, f.w - m.list_s * 14, m.list_row_h, kSel);
    const u32 ink = on ? kInk : kPanelEdgeDim;
    draw_text(d, f.text_x, ry, m.list_s, ink, fit(s.label, m.list_s, f.avail - value_w).c_str());
    const std::string v = on ? display_value(s, host_->get(s.key)) : "--";
    // A text field is opened, not stepped, so it gets no arrows.
    const bool steps = s.type != Setting::Type::Text;
    const std::string shown = on && is_sel && steps ? "< " + v + " >" : v;
    draw_text(d, f.text_x + f.avail - std::min(value_w, text_width(m.list_s, shown.c_str())),
              ry, m.list_s, ink, fit(shown, m.list_s, value_w).c_str());
  }
  scroll_bar(d, m, f.px0, f.py0, f.w, visible, n, top);

  // The note, under a rule at the foot of the panel.
  const int note_y = f.py0 + m.list_rows_y + visible * m.list_row_h + m.list_s;
  fill_rect(d, f.px0 + m.pad, note_y, f.w - 2 * m.pad, std::max(1, m.list_s), kPanelEdgeDim);
  const Setting& cur = t[sel];
  const char* why = host_->disabled_reason(cur);
  const char* line = why && *why ? why : cur.note;
  const u32 note_ink = why && *why ? kEdgeText : kDim;
  // What it takes to see the change; uses the second note line.
  const char* when = (cur.flags & FlagRestart) ? "RESTART REQUIRED"
                   : (cur.flags & FlagDeferred) ? "APPLIED WHEN THE MENU CLOSES" : nullptr;
  if (when && !host_->enabled(cur)) when = nullptr;
  // DS Options with a firmware dump: changes go beside the dump, not the config.
  std::string when_text;
  if (when) {
    when_text = when;
    if (page() == Page::DsOptions)
      if (const char* src = host_->user_settings_note()) { when_text += " - "; when_text += src; }
    when = when_text.c_str();
  }
  if (line) {
    const int lines = note_lines;
    // Wrapped on spaces rather than cut with an ellipsis.
    std::string rest = line;
    for (int i = 0; i < lines && !rest.empty(); ++i) {
      std::string take = rest;
      size_t cut = std::string::npos;
      while (text_width(m.list_s, take.c_str()) > f.avail) {
        cut = take.find_last_of(' ');
        if (cut == std::string::npos) break;
        take.resize(cut);
      }
      // One word longer than the row: cut it rather than loop forever.
      if (text_width(m.list_s, take.c_str()) > f.avail) take = fit(take, m.list_s, f.avail);
      draw_text(d, f.text_x, note_y + 2 * m.list_s + i * m.list_row_h, m.list_s, note_ink, take.c_str());
      if (take.size() >= rest.size()) break;
      rest.erase(0, take.size());
      while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
    }
  }
  if (when) draw_text(d, f.text_x, note_y + 2 * m.list_s + note_lines * m.list_row_h, m.list_s, kEdgeText, fit(when, m.list_s, f.avail).c_str());
}

// Controls page: two columns of bindings (keyboard, pad); shoulders switch.
void Menu::move_bind_row(int delta) {
  const int n = host_->binding_count(bind_pad_);
  if (n <= 0) return;
  const int at = bind_row_ + delta;
  bind_row_ = at < 0 ? 0 : at >= n ? n - 1 : at;
}

Menu::Result Menu::handle_controls(u32 presses) {
  using B = io::Io::Button;
  const auto hit = [&](B b) { return (presses >> b) & 1; };
  // The list is not fixed length: a hotkey's second row appears/disappears as
  // its first is bound/cleared, so the cursor's row may no longer exist.
  move_bind_row(0);
  // Redraw when listening stops, even from a device-cancelled capture
  // (Escape, wrong-column press) that leaves nothing else for the menu to notice.
  if (const bool listening = host_->capturing(); listening != listen_shown_) {
    listen_shown_ = listening;
    dirty_ = true;
  }
  // While listening, presses are swallowed elsewhere; collected here instead.
  if (host_->capturing()) {
    const std::string got = host_->take_capture();
    if (!got.empty()) {
      const SettingsHost::Binding b = host_->binding(bind_pad_, bind_row_);
      if (!b.key.empty()) host_->bind(b.key, got);
      dirty_ = true;
    }
    return Result::None;
  }
  // With a pad, CLEAR and DEFAULTS are the west and north buttons by position,
  // so rebinding DS Y/X on this page can't move them. Keyboard has no
  // positions: DS Y/X as bound.
  const bool pad = host_->has_pad();
  const bool clear = pad ? (faces_ >> 2) & 1 : hit(B::BTN_Y);
  const bool reset = pad ? (faces_ >> 3) & 1 : hit(B::BTN_X);
  // DEFAULTS wipes the whole column, so it takes a second press; any other
  // press backs out and does only that.
  if (reset_armed_ && (presses || faces_)) {
    reset_armed_ = false;
    dirty_ = true;
    if (reset) host_->reset_bindings(bind_pad_);
    return Result::None;
  }
  if (hit(B::BTN_UP))   move_bind_row(-1);
  if (hit(B::BTN_DOWN)) move_bind_row(+1);
  // Shoulders swap columns rather than paging (list is short, two columns).
  if (hit(B::BTN_L) || hit(B::BTN_R)) {
    bind_pad_ = !bind_pad_;
    bind_row_ = 0;
    bind_top_ = 0;
  }
  // A alone opens a capture: START may be the control being bound, so it
  // cannot also be the opener.
  if (hit(B::BTN_A)) host_->begin_capture(bind_pad_);
  // CLEAR: no key press means "no key".
  if (clear) {
    const SettingsHost::Binding b = host_->binding(bind_pad_, bind_row_);
    if (!b.key.empty()) host_->bind(b.key, "none");
  }
  if (reset) { reset_armed_ = true; dirty_ = true; }
  if (hit(B::BTN_B)) pop();
  return Result::None;
}

void Menu::draw_controls(const Canvas& d) const {
  char title[32];
  std::snprintf(title, sizeof title, "CONTROLS  %s", bind_pad_ ? "PAD" : "KEYBOARD");
  const ListFrame f = list_frame(d, title);
  const Metrics& m = f.m;
  visible_ = f.visible;

  const int n = host_->binding_count(bind_pad_);
  // Drawing is const, so this clamp is local; handle_controls() moves the cursor back.
  const int sel = std::min(bind_row_, std::max(0, n - 1));
  // Three rows at the foot: legend over two lines (doesn't fit one), plus a clash line.
  const int foot_rows = 3;
  const int visible = std::max(1, f.visible - foot_rows);
  int top = bind_top_;
  if (sel < top) top = sel;
  if (sel >= top + visible) top = sel - visible + 1;
  if (top > n - visible) top = n - visible;
  if (top < 0) top = 0;
  bind_top_ = top;

  const bool listening = host_->capturing();
  const int value_w = f.avail / 2;
  for (int i = 0; i < visible && top + i < n; ++i) {
    const SettingsHost::Binding b = host_->binding(bind_pad_, top + i);
    const int ry = f.py0 + m.list_rows_y + i * m.list_row_h;
    const bool is_sel = top + i == sel;
    if (is_sel) fill_rect(d, f.px0 + m.list_s * 4, ry - m.list_s * 2, f.w - m.list_s * 14, m.list_row_h, kSel);
    draw_text(d, f.text_x, ry, m.list_s, kInk, fit(b.label, m.list_s, f.avail - value_w).c_str());
    // The row being rebound says so where its value was.
    const std::string v = is_sel && listening ? "PRESS ANY..." : (b.value.empty() || b.value == "none" ? "--" : b.value);
    const u32 ink = is_sel && listening ? kEdgeText : (v == "--" ? kDim : kInk);
    draw_text(d, f.text_x + f.avail - std::min(value_w, text_width(m.list_s, v.c_str())),
              ry, m.list_s, ink, fit(v, m.list_s, value_w).c_str());
  }
  scroll_bar(d, m, f.px0, f.py0, f.w, visible, n, top);

  const int foot_y = f.py0 + m.list_rows_y + visible * m.list_row_h + m.list_s;
  fill_rect(d, f.px0 + m.pad, foot_y, f.w - 2 * m.pad, std::max(1, m.list_s), kPanelEdgeDim);
  // Footer names DS buttons by position pip, since the pad in hand may print
  // different letters on the same four buttons.
  const bool pad = host_->has_pad();
  const char* help1 = listening ? "PRESS THE CONTROL TO BIND,"
                    : reset_armed_ ? (pad ? "\x04 AGAIN: RESET ALL" : "X AGAIN: RESET ALL")
                    : pad ? "\x02 BIND   \x03 CLEAR   \x04 DEFAULTS" : "A BIND   Y CLEAR   X DEFAULTS";
  const char* help2 = listening ? "OR ESCAPE TO CANCEL"
                    : host_->has_pad() ? "L/R KEYBOARD OR PAD" : "L/R SWAP COLUMN";
  draw_text(d, f.text_x, foot_y + 2 * m.list_s, m.list_s, reset_armed_ && !listening ? kDanger : kDim, fit(help1, m.list_s, f.avail).c_str());
  draw_text(d, f.text_x, foot_y + 2 * m.list_s + m.list_row_h, m.list_s, kDim, fit(help2, m.list_s, f.avail).c_str());
  const std::vector<std::string> clash = host_->collisions();
  if (!clash.empty())
    draw_text(d, f.text_x, foot_y + 2 * m.list_s + 2 * m.list_row_h, m.list_s, kEdgeText,
              fit(clash[0], m.list_s, f.avail).c_str());
}

// Firmware stores each byte as one UTF-16 unit (firmware_gen.cpp put_utf16),
// so the tables are ASCII.
namespace {
const char* const kCharTables[] = {
  " ABCDEFGHIJKLMNOPQRSTUVWXYZ",
  " abcdefghijklmnopqrstuvwxyz",
  " 0123456789.,!?'-&+()/:@",
};
const char* const kCharTableNames[] = {"CAPITALS", "LOWERCASE", "NUM+SYM"};
constexpr int kCharTableCount = 3;
// Widest a field is laid out before it wraps (two lines, like the console's own screen).
constexpr int kEditCols = 13;

// Which table a character belongs to, so an existing name opens on the right one.
int table_of(char c) {
  for (int t = 0; t < kCharTableCount; ++t)
    if (std::strchr(kCharTables[t], c)) return t;
  return 0;
}
} // namespace

void Menu::open_text_edit() {
  const Setting& s = table()[set_row_[table_slot()]];
  edit_dest_ = EditDest::Setting;
  edit_key_ = s.key;
  edit_label_ = s.label;
  edit_max_ = s.lo;
  const std::string cur = host_->get(s.key);
  edit_buf_ = cur.empty() ? default_value(s) : cur;
  if (static_cast<int>(edit_buf_.size()) > edit_max_) edit_buf_.resize(static_cast<size_t>(edit_max_));
  if (edit_buf_.empty()) edit_buf_ = " ";   // one space, so there is a character to cycle
  edit_pos_ = 0;
  edit_table_ = table_of(edit_buf_[0]);
  push(Page::TextEdit);
}

// Same editor, collecting a credential instead of a setting. Field limits
// are RetroAchievements' own (username 20 chars, password generous).
void Menu::open_credential_edit(EditDest dest) {
  edit_dest_ = dest;
  edit_key_.clear();
  edit_label_ = dest == EditDest::CheevosUser ? "RA USERNAME" : "RA PASSWORD";
  edit_max_ = dest == EditDest::CheevosUser ? 20 : 32;
  edit_buf_ = " ";
  edit_pos_ = 0;
  edit_table_ = table_of(edit_buf_[0]);
  push(Page::TextEdit);
}

Menu::Result Menu::handle_text_edit(u32 presses) {
  using B = io::Io::Button;
  const auto hit = [&](B b) { return (presses >> b) & 1; };
  const char* tab = kCharTables[edit_table_];
  const int n = static_cast<int>(std::strlen(tab));
  if (hit(B::BTN_UP) || hit(B::BTN_DOWN)) {
    // A character from another table starts the walk at its beginning.
    const char* at = std::strchr(tab, edit_buf_[static_cast<size_t>(edit_pos_)]);
    int i = at ? static_cast<int>(at - tab) : 0;
    i = (i + (hit(B::BTN_UP) ? 1 : n - 1)) % n;
    edit_buf_[static_cast<size_t>(edit_pos_)] = tab[i];
  }
  if (hit(B::BTN_L) || hit(B::BTN_R)) {
    edit_table_ = (edit_table_ + (hit(B::BTN_L) ? kCharTableCount - 1 : 1)) % kCharTableCount;
    // Move the character under the cursor into the new table.
    const char c = edit_buf_[static_cast<size_t>(edit_pos_)];
    const char* from = kCharTables[table_of(c)];
    if (const char* at = std::strchr(from, c)) {
      const int i = static_cast<int>(at - from);
      const char* to = kCharTables[edit_table_];
      if (i < static_cast<int>(std::strlen(to))) edit_buf_[static_cast<size_t>(edit_pos_)] = to[i];
    }
  }
  if (hit(B::BTN_LEFT) && edit_pos_ > 0) {
    --edit_pos_;
    edit_table_ = table_of(edit_buf_[static_cast<size_t>(edit_pos_)]);
  }
  if (hit(B::BTN_RIGHT)) {
    // Past the end grows the field, up to what the firmware keeps.
    if (edit_pos_ + 1 < static_cast<int>(edit_buf_.size())) {
      ++edit_pos_;
      edit_table_ = table_of(edit_buf_[static_cast<size_t>(edit_pos_)]);
    } else if (static_cast<int>(edit_buf_.size()) < edit_max_) {
      edit_buf_ += ' ';
      ++edit_pos_;
    }
  }
  if (hit(B::BTN_A) || hit(B::BTN_START)) {
    // Trailing spaces are an artefact of moving right, not part of the name.
    std::string out = edit_buf_;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    // For a credential every space goes, not just trailing: a space in the
    // middle means the cursor passed over that slot unchosen, and neither
    // field may contain one anyway.
    if (edit_dest_ != EditDest::Setting)
      out.erase(std::remove(out.begin(), out.end(), ' '), out.end());
    switch (edit_dest_) {
    case EditDest::Setting:
      host_->set(edit_key_.c_str(), out);
      pop();
      break;
    case EditDest::CheevosUser:
      pending_user_ = out;
      pop();
      // Straight on to the password, so the two prompts read as one action.
      if (!pending_user_.empty()) open_credential_edit(EditDest::CheevosPassword);
      break;
    case EditDest::CheevosPassword:
      pop();
      if (cheevos_) cheevos_->sign_in(pending_user_, out);
      pending_user_.clear();
      out.assign(out.size(), ' ');
      break;
    }
    // Deliberately no reset of edit_dest_ here: CheevosUser opens the
    // password prompt from inside this switch, and a reset would misroute it.
    return Result::None;
  }
  if (hit(B::BTN_B)) {   // B abandons: nothing was written until A
    pop();
    pending_user_.clear();   // so a cancelled sign-in cannot be completed later
  }
  return Result::None;
}

void Menu::draw_text_edit(const Canvas& d) const {
  static constexpr const char* kHelp1 = "UP/DOWN LETTER   L/R TABLE";
  static constexpr const char* kHelp2 = "\x02 DONE   \x01 CANCEL";
  Metrics m = metrics(d);
  // Wide enough for the field or the help line, whichever is wider; scale
  // steps down rather than clip. A long field wraps over two lines, as the
  // console's own settings screen does.
  const int cols = edit_max_ > kEditCols ? (edit_max_ + 1) / 2 : edit_max_;
  const int rows = (edit_max_ + cols - 1) / (cols > 0 ? cols : 1);
  const auto want = [&](const Metrics& mm) {
    return std::max({cols * kAdvance * mm.s, text_width(mm.s, edit_label_.c_str()),
                     text_width(mm.list_s, kHelp1), text_width(mm.list_s, kHelp2)}) + 8 * mm.s;
  };
  while (m.s > 2 && want(m) > d.w) m = metrics_for(m.s - 1);
  const int panel_w = std::min(d.w - 2 * m.pad, want(m));
  // The field, then the table's name and the two help lines under it.
  const int panel_h = m.rows_y + rows * m.row_h + m.s + 3 * m.list_row_h + m.pad;
  const int px0 = (d.w - panel_w) / 2, py0 = (d.h - panel_h) / 2;
  panel(d, px0, py0, panel_w, panel_h);

  draw_text(d, px0 + (panel_w - text_width(m.s, edit_label_.c_str())) / 2, py0 + m.title_y, m.s, kInk, edit_label_.c_str());
  fill_rect(d, px0 + m.pad, py0 + m.rule_y, panel_w - 2 * m.pad, std::max(1, m.s / 2), kEdge);

  const int fx = px0 + m.pad + 2 * m.s, fy = py0 + m.rows_y;
  // Highlighted rather than underlined: the font has no descender room.
  for (int i = 0; i < static_cast<int>(edit_buf_.size()); ++i) {
    const int cx = fx + (i % cols) * kAdvance * m.s, cy = fy + (i / cols) * m.row_h;
    if (i == edit_pos_) fill_rect(d, cx - m.s, cy - m.s, kAdvance * m.s, m.glyph_px + 2 * m.s, kSel);
    // Password masks everything but the character being chosen (must stay
    // visible to cycle blind). A space is not masked: it's an unchosen slot,
    // not a real character, and '*' would misreport the field's length.
    const char c = edit_buf_[static_cast<size_t>(i)];
    const bool mask = edit_dest_ == EditDest::CheevosPassword && i != edit_pos_ && c != ' ';
    const char one[2] = {mask ? '*' : c, 0};
    draw_text(d, cx, cy, m.s, kInk, one, true);
  }
  // Font is uppercase-only; the table name is the only case indicator.
  const int below = fy + rows * m.row_h + m.s;
  const int avail = panel_w - 6 * m.s;
  draw_text(d, fx, below, m.list_s, kEdgeText, kCharTableNames[edit_table_]);
  draw_text(d, fx, below + m.list_row_h, m.list_s, kDim, fit(kHelp1, m.list_s, avail).c_str());
  draw_text(d, fx, below + 2 * m.list_row_h, m.list_s, kDim, fit(kHelp2, m.list_s, avail).c_str());
}

void Menu::draw(const Canvas& d) const {
  if (page() == Page::Games) { draw_games(d); return; }
  if (page() == Page::Cheats) { draw_cheats(d); return; }
  if (page() == Page::Cheevos) { draw_cheevos(d); return; }
  if (page() == Page::CheevosAccount) { draw_cheevos_account(d); return; }
  if (page() == Page::Options) { draw_options(d); return; }
  if (settings_page()) { draw_settings(d); return; }
  if (controls_page()) { draw_controls(d); return; }
  if (page() == Page::TextEdit) { draw_text_edit(d); return; }
  const bool slots = page() == Page::Slot;
  Metrics m = metrics(d);
  const int rows = slots ? kSlotRows + 2 : root_rows();   // ten slots in two columns, plus auto state and a help line
  // 75 glyphs is a floor for a short menu.
  const auto wanted_w = [&](const Metrics& mm) {
    int w = (slots ? 100 : 75) * mm.s;
    if (slots) return w;
    for (int i = 0; i < kRootRows; ++i) {
      if (!root_visible(i)) continue;
      // The slot row formats its own text; "SLOT < 0 >" is its widest form.
      const char* label = kRoot[i].label ? kRoot[i].label : (slot_notice_.empty() ? "SLOT < 0 >" : "SLOT < 0 > REJECTED");
      w = std::max(w, text_width(mm.s, label) + 6 * mm.s + 2 * mm.pad);
    }
    return w;
  };
  // Step the glyph scale down rather than let a tall page run off a short canvas.
  while (m.s > 2 && (panel_height(m, rows) > d.h || wanted_w(m) > d.w)) m = metrics_for(m.s - 1);
  const int scale = m.s, row_h = m.row_h, title_y = m.title_y, rule_y = m.rule_y, rows_y = m.rows_y;
  const int panel_w = std::min(d.w - 2 * m.pad, wanted_w(m));
  const int panel_h = panel_height(m, rows);
  const int px0 = (d.w - panel_w) / 2;
  const int py0 = (d.h - panel_h) / 2;
  panel(d, px0, py0, panel_w, panel_h);

  // Not PAUSED during a network session: the game behind this page keeps running.
  const char* title = slots ? (slot_delete_ ? "DELETE STATE" : "STATE SLOT") : (net_session_ ? "MENU" : "PAUSED");
  draw_text(d, px0 + (panel_w - text_width(scale, title)) / 2, py0 + title_y, scale, slots && slot_delete_ ? kDanger : kInk, title);
  if (slots) {
    const char* help = slot_delete_ ? "\x03 DONE" : "\x03 DELETE";
    draw_text(d, px0 + (panel_w - text_width(scale, help)) / 2, py0 + rows_y + (kSlotRows + 1) * row_h, scale, kDim, help);
  }
  fill_rect(d, px0 + m.pad, py0 + rule_y, panel_w - 2 * m.pad, std::max(1, m.s / 2), kEdge);

  char buf[24];
  for (int i = 0; i < (slots ? kAutoSlot + 1 : root_rows()); ++i) {
    // Slots fill a column at a time: 0-4 left, 5-9 right, auto state below both.
    const int col = slots && i != kAutoSlot ? i / kSlotRows : 0;
    const int cell_w = slots && i != kAutoSlot ? (panel_w - 2 * m.pad) / 2 : panel_w - 2 * m.pad;
    const int cell_x = px0 + m.pad + col * cell_w;
    const int ry = py0 + rows_y + (slots ? (i == kAutoSlot ? kSlotRows : i % kSlotRows) : i) * row_h;
    if (i == (slots ? slot_row_ : row_)) fill_rect(d, cell_x, ry - m.s, cell_w, row_h, kSel);
    const char* label = buf;
    const int item = slots ? i : root_item(i);
    const bool doomed = slots && slot_armed_ && i == slot_row_;
    const char* tag = doomed ? "SURE?" : used_[i] ? "USED" : "EMPTY";
    if (slots && i == kAutoSlot) std::snprintf(buf, sizeof buf, "AUTO %s", tag);
    else if (slots) std::snprintf(buf, sizeof buf, "%d %s", i, tag);
    else if (item == kSlotRow) std::snprintf(buf, sizeof buf, "SLOT < %d >%s%s", slot_,
                                             slot_notice_.empty() ? "" : " ", slot_notice_.c_str());
    else label = kRoot[item].label;
    // An empty slot reads dimmer: the menu says what is there before the player commits.
    const bool weak = (slots && (!used_[i] || (i == kAutoSlot && !slot_delete_))) || (!slots && kRoot[item].result == Result::Load && !used_[slot_]);
    // A refused state warns in colour until the player moves off it; the
    // console log says which BIOS, and why.
    const bool warn = !slots && item == kSlotRow && !slot_notice_.empty();
    const u32 ink = doomed ? kDanger : warn ? kWarn : (weak && i != (slots ? slot_row_ : row_) ? kDim : kInk);
    draw_text(d, cell_x + 3 * m.s, ry, scale, ink, label);
  }
}

} // namespace ds::sdl
