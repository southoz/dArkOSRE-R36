// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/io/io.h"
#include "core/input/input_log.h"

#include <SDL2/SDL.h>
#include <string>
#include <vector>

namespace ds { struct NDS; }
namespace ds::sdl {

class Display;
class Config;

// Things a hotkey can do. The frontend drains them once a frame.
enum class Action : u8 {
  Quit, Pause, FastForward, FastForwardToggle, SaveState, LoadState, SlotNext, SlotPrev,
  VolumeUp, VolumeDown, Mute, LayoutNext, LayoutPrev, ScreenSwap, PipCornerNext, Fullscreen, Screenshot, Lid, Mic, FpsToggle, Count
};
const char* action_name(Action a);

// Keyboard, game controller and touch folded into the DS's button mask and
// pen position, handed to the core once a frame. Bindings come from the
// config ([keys], [pad], [hotkeys], [padhotkeys]).
class Input {
public:
  void configure(const Config& cfg);

  // DS_DUAL_SCREENS: "upper=<display>[:<touch>...],lower=...,none=<touch>..."
  // gives each physical panel its SDL display and touch devices (a touch is a
  // case-insensitive substring of SDL's device name, or #N for SDL's Nth).
  // Panels, not DS screens: MAIN SCREEN decides which DS screen each shows.
  enum class Panel : s8 { Unmapped = -1, Upper = 0, Lower = 1, None = 2 };
  struct TouchRoute { std::string match; int index = -1; Panel panel = Panel::Lower; };
  static bool parse_dual_screens(const char* s, int& upper, int& lower, std::vector<TouchRoute>& routes, std::string& err);
  void set_touch_routes(std::vector<TouchRoute> r) { touch_routes_ = std::move(r); touch_cache_.clear(); }
  void open_controllers();
  void close();

  // Feeds one SDL event; `display` maps window points onto the screens. In
  // dual-window mode `second` is the other window, routed by windowID.
  void handle(const SDL_Event& e, Display& display, Display* second = nullptr);
  // State for the coming frame. A press+release both arrived since the last
  // frame still counts as held this frame; the release lands on the next one.
  input::Frame frame() {
    const input::Frame f{static_cast<u16>(buttons_ | pressed_ | stick_ | face_stick_), static_cast<u8>(touch_x_), static_cast<u8>(touch_y_), touching_ || touched_ || stylus_down_ != 0};
    pressed_ = 0; stick_pressed_ = 0; touched_ = false;
    menu_fb_pressed_ = 0;   // menu fallback presses die with their frame
    return f;
  }

  // Hotkey actions since the last call, in order.
  std::vector<Action> take_actions() { std::vector<Action> a; a.swap(actions_); return a; }

  // Pause menu: presses since the last call, as a DS button mask, using the
  // player's own bindings (never seen by the guest). Edge-triggered.
  u32 take_menu_presses() {
    const u32 p = pressed_ | stick_pressed_ | menu_fb_pressed_;
    pressed_ = 0; stick_pressed_ = 0; menu_fb_pressed_ = 0;
    return p;
  }
  // What is held right now, for the menu's key repeat (unlike the edges above).
  u32 menu_held() const { return buttons_ | stick_ | menu_fb_held_; }
  // While the menu is open only Pause and Quit hotkeys fire, so a hotkey that
  // shares a control with a DS button can't hide that button from the menu.
  void set_menu_open(bool on) { menu_open_ = on; if (!on) menu_faces_ = 0; }
  // Pad face buttons pressed while the menu is open, by SDL position and
  // regardless of bindings: bit n = SDL_CONTROLLER_BUTTON n (south, east,
  // west, north). Controls-page actions that must not move when rebound.
  u32 take_menu_faces() { const u32 f = menu_faces_; menu_faces_ = 0; return f; }
  bool fast_forward_held() const { return ff_key_ || ff_pad_; }

  // A real lid switch (lid.h) drives set_lid() directly; the `lid` hotkey
  // toggles it. Closing sleeps the game, opening wakes it.
  void set_lid(bool closed) { lid_ = closed; }
  bool lid() const { return lid_; }
  // Fake mic for devices without one: `mic` held = noise at ~80% full scale.
  bool fake_mic() const { return mic_key_ || mic_pad_; }
  void fake_mic_frame(std::vector<s16>& out);

  // Pad-driven pen: moved by a stick's deflection (pad.stylus_axis) and by
  // the d-pad while the pad.stylus_dpad chord is held.
  void update_stylus();
  bool stylus_visible_binding() const;   // some pad control drives the pen
  bool stylus_visible() const { return (stylus_axis_ != StylusAxis::None || stylus_chord_.kind != Bind::None) && pad_ != nullptr && stylus_idle_ < stylus_hide_; }   // hidden after stylus_hide idle frames
  int  stylus_x() const { return static_cast<int>(stylus_fx_); }
  int  stylus_y() const { return static_cast<int>(stylus_fy_); }
  int  stylus_size() const { return stylus_size_; }

  bool quit() const { return quit_; }
  void request_quit() { quit_ = true; }

  // Rebinding, for the Controls page.
  bool has_pad() const { return pad_ != nullptr; }
  // While capturing, the next physical key or pad control is taken as a name
  // instead of fed to the game or matched against a hotkey. `pad` picks
  // which device is listened to.
  void begin_capture(bool pad);
  void cancel_capture() { capturing_ = false; }
  bool capturing() const { return capturing_; }
  // Name of what was pressed, once; taking it ends the capture.
  std::string take_capture();
  // Every binding that would shadow another, one line per clash. Empty when none.
  std::vector<std::string> collisions() const;
  // Config string for each DS button/hotkey action, and its default.
  static const char* button_name(int i);
  static int button_count();
  static const char* key_default(int i);
  static const char* pad_default(int i);
  // A pad binding as the menu draws it (L1/SELECT/position pips). Display
  // only; nothing is written back in this form.
  static std::string pad_label(const std::string& value);
  // Controls that are neither a DS button nor a hotkey: the pad-chord
  // modifier, and the pen's tap button and stick.
  static const char* mod_default(bool pad);
  static const char* stylus_button_default();
  static const char* stylus_dpad_default();
  static const char* stick_face_default();
  static const char* stick_dpad_default();
  static const char* stylus_axis_default(const Config& cfg);
  // Captured stick axis as the stick it belongs to ("left"/"right"), or
  // nullptr if not a stick.
  static const char* stylus_axis_of(const std::string& captured);
  // Axis remapping (pad.axis_<name>): each SDL controller axis fed by one
  // physical axis, optionally inverted, for pads reporting a stick rotated,
  // mirrored or wrong. `i` indexes SDL_GameControllerAxis; value is
  // "[-]<sdl axis>" or "none".
  static int axis_remap_count() { return SDL_CONTROLLER_AXIS_MAX; }
  static std::string axis_remap_key(int i);
  static std::string axis_remap_default(int i);
  // Last axis a capture took, as the physical control pushed ("-righty"),
  // before the remap. Empty if the capture was not an axis.
  const std::string& captured_raw_axis() const { return captured_raw_axis_; }
  static int action_count();
  static const char* key_hot_default(int i);
  static const char* pad_hot_default(int i);
  // A hotkey's second binding, "<action>.alt" in the config. No built-in
  // default; starts unset. DS buttons have none; the pen tap button does too
  // (pad.stylus_button.alt).
  static constexpr int HOT_SLOTS = 2;
  static const char* hot_suffix(int slot) { return slot == 1 ? ".alt" : ""; }

  // Event-level paths for the menu fallback/capture-escape, reached from handle().
  friend struct InputTestAccess;

private:
  // One binding: a keyboard key, a pad button or a pad axis direction, with
  // or without the modifier. `with` is a second pad button that must be held
  // (a chord); the most specific matching binding wins.
  // ModAlone: a hotkey of just "mod", fired when the modifier is released with nothing pressed while it was held.
  struct Bind { enum Kind : u8 { None, Key, PadButton, PadAxis, ModAlone } kind = None; int code = 0; bool neg = false; bool mod = false; int with = -1; };
  static Bind parse_key(const std::string& s);
  static Bind parse_pad(const std::string& s);

  void set(io::Io::Button b, bool down) {
    if (down) { buttons_ |= 1u << b; pressed_ |= 1u << b; } else buttons_ &= ~(1u << b);
  }
  void touch_at(int wx, int wy, Display& display);
  Panel touch_panel(SDL_TouchID id);   // DS_DUAL_SCREENS route for a device, cached on first sight
  Display* finger_target(const SDL_TouchFingerEvent& f, Display& display, Display* second);
  void warn_collisions() const;   // bindings that shadow one another, at configure time
  bool reachable(int ds_button) const;   // is there a control for it on the hardware in hand?
  bool bound_and_present(int ds_button) const;   // bound to a control this hardware has
  bool hotkey_live(Action a, bool down) const { return !down || !menu_open_ || a == Action::Pause || a == Action::Quit; }
  bool shadowed_in_menu(int ds_button) const;    // its control is the modifier or an unmodified Pause/Quit hotkey
  bool pad_control_free(int sdl_button) const;   // nothing bound to it, so the menu fallback has it
  bool key_control_free(SDL_Keycode k) const;
  void fire(Action a, bool down);
  bool key_down(SDL_Keycode k, bool down);
  bool pad_down(const Bind& b, bool down);   // a button or axis edge; true if consumed
  void axis(Uint8 which, Sint16 value);
  void physical_axis(Uint8 which, Sint16 value);   // a raw SDL axis event, through the remap to axis()
  int  logical_of(int physical, Sint16& value) const;   // the first remapped axis a physical one feeds, value signed for it; -1 if none
  // A stick as four DS buttons (the left stick as the d-pad, or either as
  // X/B/Y/A): held while past the deadzone, with edges for the pause menu.
  void stick_as_buttons(u32& held, Sint16 value, io::Io::Button neg, io::Io::Button pos);
  bool capture_event(const SDL_Event& e);
  bool axis_moved(int axis, int value) const;    // past the deadzone this axis needs
  bool is_pad_mod_button(int sdl_button) const;   // the pad's hotkey modifier
  bool is_key_mod(SDL_Keycode k) const;
  // Menu fallback controls, consulted only when key_down()/pad_down() report
  // nothing at all is bound: rescues a player who orphaned the menu's own
  // navigation. Bound controls keep their job; shipped defaults never trigger this.
  void menu_fallback_key(SDL_Keycode k, bool down);
  void menu_fallback_pad(const Bind& b, bool down);
  void menu_fallback(int ds_button, bool down) {
    if (down) { menu_fb_held_ |= 1u << ds_button; menu_fb_pressed_ |= 1u << ds_button; }
    else menu_fb_held_ &= ~(1u << ds_button);
  }

  u32  buttons_ = 0, pressed_ = 0, stick_ = 0;   // held now; pressed since the last frame; stick as d-pad
  u32  stick_prev_ = 0, stick_pressed_ = 0;      // stick-as-d-pad edges, for the pause menu
  // Face-button stick is game-only: on the Controls page X/B are reserved
  // for reset/leave, so this stick must not fire them mid-rebind.
  u32  face_stick_ = 0;
  u32  menu_fb_held_ = 0, menu_fb_pressed_ = 0;  // menu-only fallback, for orphaned bindings
  bool touching_ = false, touched_ = false;
  int  touch_x_ = 0, touch_y_ = 0;
  bool quit_ = false;
  bool lid_ = false, mic_key_ = false, mic_pad_ = false, ff_key_ = false, ff_pad_ = false;
  u32  noise_ = 0x2545F491;
  SDL_GameController* pad_ = nullptr;

  Bind key_map_[io::Io::BTN_COUNT];
  Bind pad_map_[io::Io::BTN_COUNT];
  Bind key_hot_[static_cast<int>(Action::Count)][HOT_SLOTS];
  Bind pad_hot_[static_cast<int>(Action::Count)][HOT_SLOTS];
  Bind key_mod_, pad_mod_;
  bool key_mod_down_ = false, key_mod_used_ = false, pad_mod_down_ = false, pad_mod_used_ = false;
  int  key_mod_alone_ = -1, pad_mod_alone_ = -1;   // the action bound to "mod" alone, or -1
  int  pad_mod_button_ = -1;   // the DS button the pad modifier would otherwise be
  bool axis_state_[SDL_CONTROLLER_AXIS_MAX][2] = {};   // per axis: - and + past the threshold
  int  axis_src_[SDL_CONTROLLER_AXIS_MAX] = {0, 1, 2, 3, 4, 5};   // pad.axis_<name>: the physical axis feeding it, -1 none
  bool axis_inv_[SDL_CONTROLLER_AXIS_MAX] = {};
  enum class StylusAxis : u8 { None, Right, Left };
  // Config spelling: none | left | right. Old stick_dpad = true/false still
  // parse as left/none.
  static bool parse_stick(const std::string& s, StylusAxis& out);
  StylusAxis stylus_axis_ = StylusAxis::Right;
  StylusAxis stick_dpad_ = StylusAxis::Left;   // pad.stick_dpad: a stick that also works the d-pad
  StylusAxis stick_face_ = StylusAxis::None;   // pad.stick_face: a stick that also works X/B/Y/A (up/down/left/right)
  Bind stylus_button_[HOT_SLOTS];   // pressing either touches at the pen's position (pad.stylus_button, .alt)
  u8   stylus_down_ = 0;            // which of them is held, a bit per slot
  Bind stylus_chord_;          // while held, the d-pad moves the pen instead of the game
  bool stylus_chord_down_ = false;
  u32  stylus_dpad_ = 0;       // d-pad directions held under the chord (bit per DS button)
  u32  held_ = 0;              // SDL pad buttons currently down (bit per button)
  int  deadzone_ = 12000;
  int  stylus_x_ = 0, stylus_y_ = 0;          // raw stick
  int  dstick_x_ = 0, dstick_y_ = 0;          // raw d-pad stick: it moves the pen while the chord is held
  double stylus_fx_ = 128, stylus_fy_ = 96;    // pen position, DS pixels
  double stylus_speed_ = 4.0;
  int  stylus_size_ = 2;
  int  stylus_hide_ = 90, stylus_idle_ = 1 << 30;   // frames without movement or a touch; starts hidden
  std::vector<Action> actions_;
  bool capturing_ = false, capture_pad_ = false;
  bool menu_open_ = false;
  u32  menu_faces_ = 0;
  // pad.face_fix: SDL face button -> the positional one the kernel says it is
  // (identity unless the mapping has a/b or x/y swapped); see pad_faces.h.
  bool face_fix_ = true;
  u8   face_remap_[4] = {0, 1, 2, 3};
  void detect_faces();
  std::vector<TouchRoute> touch_routes_;
  std::vector<std::pair<SDL_TouchID, Panel>> touch_cache_;
  std::string captured_;
  std::string captured_raw_axis_;
  bool capture_swallow_ = false;   // swallows release of whatever was captured
  bool capture_mod_ = false;      // the modifier is held: what follows is a chord
};

} // namespace ds::sdl
