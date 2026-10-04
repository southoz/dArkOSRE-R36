// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/cheat/database.h"
#include "frontend/sdl/settings.h"

#include <string>
#include <vector>

namespace ds::sdl {

// A destination for drawing in DS pixel space (256x192): `xrun` maps a DS
// column onto the destination's columns (null: 1:1), `h` is the destination
// height. Used only by the stylus crosshair (guest-space pointer); everything
// else draws in panel pixels, see Canvas.
struct Blit { u32* px; u32 pitch; u32 h; const u16* xrun; };
// DS pixel (x, y) -> the panel pixels it covers in `d`. Columns go by the
// view's width (xrun's last entry) rather than through the run table
// itself, which under chunky is a cell map and loses columns if used directly.
struct BlitRect { u32 x0, x1, y0, y1; };
inline BlitRect blit_rect(const Blit& d, int x, int y) {
  const u32 w = d.xrun ? d.xrun[ds::SCREEN_W] : ds::SCREEN_W;
  return BlitRect{w * static_cast<u32>(x) / ds::SCREEN_W, w * static_cast<u32>(x + 1) / ds::SCREEN_W,
                  d.h * static_cast<u32>(y) / ds::SCREEN_H, d.h * static_cast<u32>(y + 1) / ds::SCREEN_H};
}

// The frontend's own drawing surface in output pixels (whole window/panel,
// not one DS screen). Plain pixel coordinates let a page lay out for the
// screen it's actually on rather than a 256x192 framebuffer.
//
// Tiers with no panel-resolution CPU-writable buffer get a Canvas over a
// 256x192 scratch scaled like a frame instead; see Display::canvas_capable().
struct Canvas { u32* px; u32 pitch; int w, h; };

// What a piece of drawing covered (Display::note_canvas_draw). Empty when
// nothing was drawn.
struct Rect { int x = 0, y = 0, w = 0, h = 0; };

// Face-button position pips: a diamond of four with the named one filled.
// Control bytes so they sit inside an ordinary string ("\x01 A").
constexpr char kFaceSouth = '\x01';   // bottom  (SDL a)
constexpr char kFaceEast  = '\x02';   // right   (SDL b)
constexpr char kFaceWest  = '\x03';   // left    (SDL x)
constexpr char kFaceNorth = '\x04';   // top     (SDL y)

// A 5x7 uppercase font, `scale` times. Lowercase folds to uppercase; anything
// outside the table draws as a space. Returns the x past the string.
// `keep_case` draws a-z as lower-case glyphs instead of folding; used only by
// the name editor, where the player must see which case they're typing.
int  draw_text(const Canvas& d, int x, int y, int scale, u32 colour, const char* s, bool keep_case = false);
int  text_width(int scale, const char* s);
// Glyph scale a canvas of this size should draw at, keeping apparent size on
// glass consistent with the old scale-2-into-256-wide-then-upscale menu.
int  ui_scale(const Canvas& d);
// Halves every pixel's brightness, in place, over a whole framebuffer.
// Interface so menu.cpp need not depend on the RetroAchievements library
// (builds without DSPERATE_CHEEVOS); main.cpp implements it over
// ds::cheevos::Client. Everything is a formatted string: the menu draws
// lines, it doesn't know what an achievement is.
struct CheevosHost {
  virtual ~CheevosHost() = default;

  struct Row {
    std::string title;       // the achievement's name
    std::string detail;      // its description, or measured progress
    u32  points = 0;
    bool unlocked = false;
    bool unsupported = false;   // a condition reads memory DSperate does not map
  };

  // One line for the account page: signed in as whom, signed out, or why
  // there is nothing to show.
  virtual std::string status() const = 0;
  // "2/110 EARNED  1/1090 POINTS", or empty when no set is loaded.
  virtual std::string progress() const = 0;
  virtual bool signed_in() const = 0;
  virtual bool has_set() const = 0;

  virtual int  row_count() const = 0;
  virtual Row  row(int i) const = 0;

  // Both asynchronous: the menu shows whatever status() says on a later frame.
  virtual void sign_in(const std::string& username, const std::string& password) = 0;
  virtual void sign_out() = 0;

  // Enum rather than config keys, so menu.cpp need not know their names; the
  // frontend maps them onto cheevos.* and persists them.
  enum class Option : u8 { Toasts, Screenshot, Encore };
  virtual bool option(Option) const = 0;
  virtual void set_option(Option, bool on) = 0;
};

void dim_framebuffer(u32* px, u32 n);
// Small centred panel with a title, a second line, and a dim third one: the
// "unpacking" notice a first launch of a zipped game shows.
void draw_notice(const Canvas& d, const char* title, const char* line2, const char* line3);

// Achievement unlock, bottom-right corner (game keeps playing behind it, DS
// picture is centred). Sizes to its text up to two thirds of the canvas,
// truncates beyond. `header` is the accent line above the title
// ("ACHIEVEMENT UNLOCKED"), may be null. `detail` may be null. `points`
// drawn only when non-zero (RetroAchievements has 0-point achievements).
void draw_toast(const Canvas& d, const char* header, const char* title, const char* detail, u32 points);
// Where draw_toast will put it, so the caller can tell Display which part of
// the canvas changed rather than repainting all of it.
Rect toast_rect(const Canvas& d, const char* header, const char* title, const char* detail, u32 points);

// The pause menu: a blitted, modal list drawn over the held last frame while
// emulation is stopped. Not an overlay; drawn once per idle tick into a copy
// of the framebuffer and presented through the unscaled path.
//
// Navigation uses the player's own DS bindings (up/down, A, B); the guest
// cannot see them while the menu is up.
class Menu {
public:
  // What the frontend should do after input(). Save/Load act on slot();
  // Launch acts on chosen(); Delete acts on doomed_slot(). Reset restarts
  // whatever was booted.
  enum class Result : u8 { None, Resume, Save, Load, Quit, Launch, Reset, Delete };

  // One game in the picker. `title` is the ROM header's title or the
  // filename; `path` is what gets loaded.
  struct GameEntry { std::string title, path; };

  bool open() const { return open_; }
  void set_open(bool o);

  // The menu edits `codes` in place (the engine's own vector); `groups` give
  // headings and mark alternative sets. Null or empty hides the row entirely.
  void set_cheats(std::vector<cheat::Code>* codes, const std::vector<cheat::Group>* groups);

  // Without a host there is no OPTIONS row.
  void set_settings_host(SettingsHost* host) { host_ = host; }
  // A network session hides SAVE STATE/LOAD STATE/slot row (a state freezes
  // this machine, not the peer) and the page reads MENU instead of PAUSED
  // (emulation keeps running).
  void set_network_session(bool on) {
    net_session_ = on;
    // Root page gained/lost three rows; selection may be past the new end.
    const int n = root_rows();
    if (row_ >= n) row_ = n - 1;
  }
  // Null when the build has no RetroAchievements support, or it's off; the
  // root row then does not appear.
  void set_cheevos_host(CheevosHost* host) { cheevos_ = host; }
  // Set when a toggle changed something; cleared by the frontend once saved.
  bool cheats_dirty() const { return cheats_dirty_; }
  void clear_cheats_dirty() { cheats_dirty_ = false; }

  // Owned by the frontend (built once at boot); the menu only points at it.
  // Null or empty leaves open_games() showing "no games" on the page.
  void set_games(const std::vector<GameEntry>* games) { games_ = games; }
  // Raised as the launcher's own modal page: not reached from the root menu,
  // and B does not back out (nothing behind it -- the loader cart has faded
  // to white). Way out is choosing a game or quitting.
  void open_games();
  // The path of the game picked, valid when update() returned Launch.
  const std::string& chosen() const { return chosen_; }

  int  slot() const { return slot_; }
  // The slot whose state the player confirmed away, valid when update()
  // returned Delete; kAutoSlot for the auto state.
  int  doomed_slot() const { return slot_row_; }
  void set_slot(int s) { slot_ = s; }
  // Shown on the root row so the player can see what a save would overwrite.
  // kAutoSlot: the auto state, on the slot page only so it can be deleted.
  void set_slot_used(int s, bool used) { if (s >= 0 && s <= kAutoSlot) used_[s] = used; }
  // Short word after "SLOT < n >" when a state was refused rather than
  // loaded. Cleared as soon as the player acts on the slot.
  void set_slot_notice(const char* n) { slot_notice_ = n ? n : ""; }
  void clear_slot_notice() { slot_notice_.clear(); }

  // One menu tick. `presses` are button edges since the last call, `held` is
  // what is down now (key repeat), `ms` is elapsed time (repeat, marquee).
  Result update(u32 presses, u32 held, u32 ms);
  Result input(u32 presses) { return update(presses, 0, 0); }
  // Pad face presses by position (Input::take_menu_faces), for the next
  // update. With a pad, the Controls page's CLEAR and DEFAULTS are the west
  // and north buttons whatever DS Y/X are bound to.
  void face_presses(u32 faces) { faces_ |= faces; }
  void   draw(const Canvas& d) const;

  // True when the picture would differ from the last draw. The frontend
  // composites only when this says to.
  bool dirty() const { return dirty_; }
  void clear_dirty() { dirty_ = false; }

  // Root page is a table in menu.cpp (kRoot); this is its length. Panel is
  // sized at draw time from the canvas, no fixed ceiling.
  static constexpr int kRootRows = 9;
  static constexpr int kSlotRows = 5;   // ten slots as two columns of five
  static constexpr int kAutoSlot = 10;  // the auto state, in a row under both columns

private:
  // A page stack rather than a flat state, so B pops wherever pressed. Games
  // is pushed onto an empty stack, since it has nothing behind it and B does
  // not leave it.
  enum class Page : u8 { Root, Slot, Cheats, Games, Options, Emulation, VisualFx, Layout, Controls, DsOptions, TextEdit, Cheevos, CheevosAccount };
  static constexpr int kMaxDepth = 6;
  Page stack_[kMaxDepth] = {Page::Root};
  int  depth_ = 1;
  Page page() const { return stack_[depth_ - 1]; }
  void push(Page p);
  bool pop();                    // false at the root, where B resumes instead
  // One line of the cheats page; headings/notes shown but not selectable.
  struct Line { enum Kind : u8 { Heading, Note, Toggle } kind; int at; };
  bool open_ = false;
  int  row_ = 0;        // the root page's selection
  int  slot_row_ = 0;   // the slot page's, kept apart so backing out lands where it left
  int  slot_ = 0;
  // Slot page's delete mode (Y toggles it): A on a used slot arms it (row
  // reads SURE? in red), second A deletes. Auto state is reachable only here.
  bool slot_delete_ = false, slot_armed_ = false;
  std::string slot_notice_;
  bool used_[kAutoSlot + 1] = {};

  std::vector<cheat::Code>* codes_ = nullptr;
  const std::vector<cheat::Group>* groups_ = nullptr;
  std::vector<Line> lines_;      // the cheats page, headings and all
  int  cheat_row_ = 0;           // index into lines_
  int  cheat_top_ = 0;           // first line shown, for scrolling
  bool cheats_dirty_ = false;

  const std::vector<GameEntry>* games_ = nullptr;
  int  game_row_ = 0;            // index into *games_
  int  game_top_ = 0;            // first game shown, for scrolling
  std::string chosen_;           // the path Result::Launch names

  static constexpr u32 kRepeatDelayMs = 400, kRepeatRateMs = 55;
  // A selected name too long for the panel scrolls sideways: still for half
  // a second, then 26 px/s, pause at the end, then snaps back.
  static constexpr u32 kMarqueeDelayMs = 500, kMarqueeHoldMs = 900, kMarqueePxPerSec = 26;

  bool dirty_ = false;
  int  repeat_dir_ = 0;          // -1 up, +1 down, 0 nothing held
  u32  repeat_ms_ = 0;
  bool repeating_ = false;       // past the initial delay
  u32  marquee_ms_ = 0;          // since the selection last moved

  Result handle(u32 presses);    // the button handling, without the timing
  int marquee_offset(int overflow) const;
  // How far the selected name overruns its row, measured by the last draw so
  // update() can animate it without re-measuring the layout.
  mutable int marquee_overflow_ = 0;
  // Rows the scrolling pages last fitted on screen; only draw knows the
  // canvas, so it's measured there like the marquee. Initial value only
  // matters if a key is pressed before the first draw.
  mutable int visible_ = 12;
  bool have_cheats() const { return codes_ && !codes_->empty(); }
  // Root page hides the cheats row when empty, so screen rows != table rows.
  bool root_visible(int item) const;
  int root_rows() const;
  int root_item(int row) const;
  SettingsHost* host_ = nullptr;
  // One row per page, kept while the menu is open so backing out and back in
  // lands where it was left.
  int  opt_row_ = 0;
  int  set_row_[4] = {};         // Emulation, VisualFx, Layout, DsOptions
  mutable int set_top_[4] = {};  // written by draw; scroll depends on canvas rows fitted
  bool have_options() const { return host_ != nullptr; }
  bool have_states() const { return !net_session_; }
  // The table a settings page shows, and where its row state lives.
  const Setting* table() const;
  int  table_slot() const;
  // Skips rows the host switched off. False when nothing selectable in that
  // direction, leaving the selection where it was.
  bool move_setting_row(int delta);
  void step_setting(int dir);
  void draw_cheats(const Canvas& d) const;
  void draw_cheevos(const Canvas& d) const;
  void draw_cheevos_account(const Canvas& d) const;
  Result handle_cheevos(u32 presses);
  Result handle_cheevos_account(u32 presses);
  void move_cheevos_row(int delta);
  bool have_cheevos() const { return cheevos_ != nullptr; }
  bool net_session_ = false;
  CheevosHost* cheevos_ = nullptr;
  int cheevos_row_ = 0, cheevos_top_ = 0;
  int account_row_ = 0;
  void draw_games(const Canvas& d) const;
  void draw_options(const Canvas& d) const;
  void draw_settings(const Canvas& d) const;
  void draw_controls(const Canvas& d) const;
  Result handle_options(u32 presses);
  Result handle_settings(u32 presses);
  Result handle_controls(u32 presses);
  void draw_text_edit(const Canvas& d) const;
  bool listen_shown_ = false;   // the Controls page last drew the listening prompt
  Result handle_text_edit(u32 presses);
  // Character editor for the two free-text [user] fields: no keyboard, so a
  // character is chosen by cycling (up/down walk the table, shoulders change
  // table, left/right move along the field).
  void open_text_edit();
  // Label kept with the buffer, not looked up at draw time (TextEdit is on
  // top by then). Destination lets one editor serve Setting and both
  // CheevosUser/CheevosPassword prompts; CheevosPassword also hides its input.
  enum class EditDest : u8 { Setting, CheevosUser, CheevosPassword };
  std::string edit_key_, edit_label_, edit_buf_;
  int  edit_pos_ = 0, edit_table_ = 0, edit_max_ = 0;
  EditDest edit_dest_ = EditDest::Setting;
  void open_credential_edit(EditDest dest);
  std::string pending_user_;   // held between the two prompts; password goes
                               // straight to sign_in, never stored
  // Controls page: column (keyboard/pad), position, scroll. `bind_row_`
  // indexes the host's binding list.
  bool bind_pad_ = false;
  int  bind_row_ = 0;
  bool reset_armed_ = false;   // first press of DEFAULTS; the second resets the column
  u32  faces_ = 0;
  mutable int bind_top_ = 0;
  void move_bind_row(int delta);
  void build_lines();
  void move_cheat_row(int delta);
  void move_game_row(int delta);
  // Scrolling pages share repeat/marquee timing, keyed off "has the
  // selection moved"; this is that selection.
  int  list_row() const;
  bool list_page() const { return page() == Page::Cheats || page() == Page::Games || settings_page() || controls_page(); }
  bool settings_page() const { return page() == Page::Emulation || page() == Page::VisualFx || page() == Page::Layout || page() == Page::DsOptions; }
  bool controls_page() const { return page() == Page::Controls; }
  void toggle_cheat();
};

} // namespace ds::sdl
