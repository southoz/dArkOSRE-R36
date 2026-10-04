// SPDX-License-Identifier: GPL-3.0-or-later
// The menu's way back from a broken binding.
//
// The pause menu is navigated with the player's own bindings, so a player who
// binds DS A to nothing reachable can no longer open the page that would put
// it back. Two things stop that being a trip to a text editor: a fallback
// layer over controls nothing is bound to, and a warning on the page itself.
// Both are only correct if they stay quiet when nothing is wrong, which is
// what most of this file checks.
#include "frontend/sdl/input.h"
#include "frontend/sdl/pad_faces.h"
#include "frontend/sdl/config.h"
#include "check.h"

#include <string>

namespace ds::sdl {

// The event-level entry points, which handle() reaches only with a Display in
// hand (input.h, InputTestAccess).
struct InputTestAccess {
  // A press and release of one pad button, down the path handle() uses.
  static void tap_pad(Input& in, SDL_GameControllerButton bt) {
    Input::Bind b; b.kind = Input::Bind::PadButton; b.code = bt;
    for (const bool down : {true, false})
      if (!in.pad_down(b, down)) in.menu_fallback_pad(b, down);
  }
  static void tap_key(Input& in, SDL_Keycode k) {
    for (const bool down : {true, false})
      if (!in.key_down(k, down)) in.menu_fallback_key(k, down);
  }
  static bool capture(Input& in, SDL_Keycode k) {
    SDL_Event e{}; e.type = SDL_KEYDOWN; e.key.keysym.sym = k;
    return in.capture_event(e);
  }
  static bool capture_key(Input& in, SDL_Keycode k, bool down) {
    SDL_Event e{}; e.type = down ? SDL_KEYDOWN : SDL_KEYUP; e.key.keysym.sym = k;
    return in.capture_event(e);
  }
  static bool capture_pad(Input& in, SDL_GameControllerButton b, bool down) {
    SDL_Event e{}; e.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP; e.cbutton.button = b;
    return in.capture_event(e);
  }
  static bool capture_axis(Input& in, SDL_GameControllerAxis a, Sint16 v) {
    SDL_Event e{}; e.type = SDL_CONTROLLERAXISMOTION; e.caxis.axis = a; e.caxis.value = v;
    return in.capture_event(e);
  }
  static bool capture_other(Input& in, Uint32 type) {
    SDL_Event e{}; e.type = type;
    return in.capture_event(e);
  }
  static bool reachable(const Input& in, int b) { return in.reachable(b); }
  static bool pad(Input& in, SDL_GameControllerButton bt, bool down) {
    Input::Bind b; b.kind = Input::Bind::PadButton; b.code = bt;
    return in.pad_down(b, down);
  }
  static bool pad_axis(Input& in, SDL_GameControllerAxis a, bool neg, bool down) {
    Input::Bind b; b.kind = Input::Bind::PadAxis; b.code = a; b.neg = neg;
    return in.pad_down(b, down);
  }
  static void axis(Input& in, SDL_GameControllerAxis a, Sint16 v) { in.axis(a, v); }
  static void physical_axis(Input& in, SDL_GameControllerAxis a, Sint16 v) { in.physical_axis(a, v); }
  static bool pen_down(const Input& in) { return in.stylus_down_ != 0; }
  static bool key_free(const Input& in, SDL_Keycode k) { return in.key_control_free(k); }
  static u32 fb_pressed(const Input& in) { return in.menu_fb_pressed_; }
  static u32 fb_held(const Input& in) { return in.menu_fb_held_; }
  static bool quit(const Input& in) { return in.quit_; }
};

namespace {

using B = io::Io::Button;
using T = InputTestAccess;
constexpr u32 kA = 1u << B::BTN_A, kB = 1u << B::BTN_B;

// A configured Input, with whatever the caller wants to break.
struct Rig {
  Config cfg;
  Input in;
  Rig& set(const char* k, const char* v) { cfg.set(k, v); return *this; }
  Input& go() { in.configure(cfg); return in; }
};

// Under the shipped defaults every fallback control is already bound, so the
// layer must contribute nothing at all: a control with a job keeps it, and
// cannot fire two menu actions at once.
void test_fallback_is_inert_by_default() {
  Rig r; Input& in = r.go();
  T::tap_pad(in, SDL_CONTROLLER_BUTTON_B);          // east: DS A, by the default map
  CHECK(in.take_menu_presses() == kA);
  CHECK(T::fb_pressed(in) == 0 && T::fb_held(in) == 0);
}

void test_fallback_rescues_an_orphaned_button() {
  Rig r; Input& in = r.set("pad.a", "none").go();
  T::tap_pad(in, SDL_CONTROLLER_BUTTON_B);
  CHECK(in.take_menu_presses() == kA);              // east confirms again
}

// The rescue must never double up: east bound to DS B cancels, and does not
// also confirm.
void test_fallback_yields_to_a_real_binding() {
  Rig r; Input& in = r.set("pad.b", "b").set("pad.a", "none").go();
  T::tap_pad(in, SDL_CONTROLLER_BUTTON_B);
  CHECK(in.take_menu_presses() == kB);
}

// A fallback press dies with the frame it happened on: an unbound Escape
// pressed during play must not be waiting to cancel the next menu opened.
void test_fallback_press_does_not_outlive_the_frame() {
  Rig r; Input& in = r.set("pad.a", "none").go();
  T::tap_pad(in, SDL_CONTROLLER_BUTTON_B);
  in.frame();
  CHECK(in.take_menu_presses() == 0);
}

// Escape is the quit hotkey by default, so it is bound and never reaches the
// fallback. It becomes the menu's cancel only for a player who took quit off it.
void test_escape_is_still_quit() {
  Rig r; Input& in = r.go();
  T::tap_key(in, SDLK_ESCAPE);
  CHECK(in.take_menu_presses() == 0);
  CHECK(T::quit(in));
}

void test_escape_cancels_once_quit_moves() {
  Rig r; Input& in = r.set("hotkeys.quit", "none").go();
  T::tap_key(in, SDLK_ESCAPE);
  CHECK(in.take_menu_presses() == kB);
  CHECK(!T::quit(in));
}

// Rebinding the pad column used to be a one-way door: a key event fell past
// the capture and reached the ordinary handler, so Escape -- the one key a
// player will try -- fired the quit hotkey and took the emulator with it.
void test_pad_capture_swallows_keys_and_escape_cancels() {
  Rig r; Input& in = r.go();
  in.begin_capture(true);
  CHECK(T::capture(in, SDLK_x));                    // swallowed
  CHECK(in.capturing());                            // and bound nothing
  CHECK(in.take_capture().empty());
  in.begin_capture(true);
  CHECK(T::capture(in, SDLK_ESCAPE));
  CHECK(!in.capturing());
  CHECK(!T::quit(in));
}

// The modifier is the first half of most of the shipped pad hotkeys
// ("mod+start"), so taking it the moment it goes down left no way to enter one
// from the page. Held, it prefixes what follows; released alone, it binds
// itself.
void test_capture_composes_a_chord_with_the_modifier() {
  // guide is the default padhotkeys.modifier.
  { Rig r; Input& in = r.go();
    in.begin_capture(true);
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_GUIDE, true));
    CHECK(in.take_capture().empty());                     // nothing yet: it may be a chord
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_START, true));
    CHECK(in.take_capture() == "mod+start"); }
  // Released with nothing pressed after it, it was not a chord.
  { Rig r; Input& in = r.go();
    in.begin_capture(true);
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_GUIDE, true));
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_GUIDE, false));
    CHECK(in.take_capture() == "guide"); }
  // An axis takes the prefix too: "mod++righttrigger" is a shipped default.
  { Rig r; Input& in = r.go();
    in.begin_capture(true);
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_GUIDE, true));
    CHECK(T::capture_axis(in, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32000));
    CHECK(in.take_capture() == "mod++righttrigger"); }
  // A pad whose modifier has been moved follows it, and the old one is then
  // an ordinary button.
  { Rig r; Input& in = r.set("padhotkeys.modifier", "leftstick").go();
    in.begin_capture(true);
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_GUIDE, true));
    CHECK(in.take_capture() == "guide"); }
  { Rig r; Input& in = r.set("padhotkeys.modifier", "leftstick").go();
    in.begin_capture(true);
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_LEFTSTICK, true));
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_Y, true));
    CHECK(in.take_capture() == "mod+y"); }
  // The keyboard column has no modifier by default, so nothing waits there.
  { Rig r; Input& in = r.go();
    in.begin_capture(false);
    CHECK(T::capture_key(in, SDLK_j, true));
    CHECK(in.take_capture() == "J"); }
  { Rig r; Input& in = r.set("hotkeys.modifier", "left ctrl").go();
    in.begin_capture(false);
    CHECK(T::capture_key(in, SDLK_LCTRL, true));
    CHECK(in.take_capture().empty());
    CHECK(T::capture_key(in, SDLK_j, true));
    CHECK(in.take_capture() == "mod+J"); }
}

// The keyboard column cannot be satisfied from a controller, and a handheld
// has no keyboard at all -- so opening it there used to be a dead end, with
// the pad falling through to the menu underneath instead of getting out.
void test_keyboard_capture_backs_out_on_pad_input() {
  { Rig r; Input& in = r.go();
    in.begin_capture(false);
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_A, true));   // swallowed...
    CHECK(!in.capturing());                                     // ... and backs out
    CHECK(in.take_capture().empty());                           // binding nothing
  }
  // A stick counts once it has actually been moved; its resting noise does not
  // cancel anything, but is still eaten rather than reaching the menu.
  { Rig r; Input& in = r.go();
    in.begin_capture(false);
    CHECK(T::capture_axis(in, SDL_CONTROLLER_AXIS_LEFTX, 900));
    CHECK(in.capturing());
    CHECK(T::capture_axis(in, SDL_CONTROLLER_AXIS_LEFTX, 30000));
    CHECK(!in.capturing());
    CHECK(in.take_capture().empty()); }
  // A key still binds, and Escape still cancels.
  { Rig r; Input& in = r.go();
    in.begin_capture(false);
    CHECK(T::capture_key(in, SDLK_j, true));
    CHECK(in.take_capture() == "J"); }
  // The window closing must still reach the frontend, capture or no capture.
  { Rig r; Input& in = r.go();
    in.begin_capture(false);
    CHECK(!T::capture_other(in, SDL_QUIT));
    CHECK(in.capturing()); }
}

// The warning is only useful if it means what it says, so it must account for
// every remaining way in: START confirms on every page but Controls, and the
// fallback layer holds any control nothing is bound to.
void test_reachable_counts_every_way_in() {
  { Rig r; Input& in = r.go();
    CHECK(T::reachable(in, B::BTN_A) && T::reachable(in, B::BTN_B));
    CHECK(in.collisions().empty()); }
  // A cleared, but START still opens rows.
  { Rig r; Input& in = r.set("keys.a", "none").go();
    CHECK(T::reachable(in, B::BTN_A)); }
  { Rig r; Input& in = r.set("keys.a", "none").set("keys.start", "F1").go();
    CHECK(T::reachable(in, B::BTN_A)); }
  // Both cleared frees Return, which the fallback then claims.
  { Rig r; Input& in = r.set("keys.a", "none").set("keys.start", "none").go();
    CHECK(T::key_free(in, SDLK_RETURN));
    CHECK(T::reachable(in, B::BTN_A)); }
  // Only with Return taken by something else is there nothing left.
  { Rig r; Input& in = r.set("keys.a", "none").set("keys.start", "none").set("keys.select", "Return").go();
    CHECK(!T::reachable(in, B::BTN_A));
    const std::vector<std::string> w = in.collisions();
    CHECK(!w.empty());
    // First: the page shows one line, and this is the one that cannot be
    // recovered from without a text editor.
    CHECK(w[0].find("UNREACHABLE") != std::string::npos); }
}

// A hotkey on DS A's own control used to hide it from the menu too, so a
// stray unmodified screenshot = A locked the player out. In the menu only
// Pause and Quit still fire; those two (and the modifier) stay a real lockout.
void test_hotkey_cannot_hide_a_from_the_menu() {
  constexpr u32 kA = 1u << B::BTN_A;
  { Rig r; Input& in = r.set("keys.a", "X").set("hotkeys.screenshot", "X").go();
    T::tap_key(in, SDLK_x);
    CHECK(in.take_actions().size() == 1);             // in game the hotkey wins
    CHECK(in.take_menu_presses() == 0);
    in.set_menu_open(true);
    T::tap_key(in, SDLK_x);
    CHECK(in.take_actions().empty());                 // in the menu, A does
    CHECK(in.take_menu_presses() == kA);
    CHECK(T::reachable(in, B::BTN_A)); }
  { Rig r; Input& in = r.set("keys.a", "X").set("keys.start", "none").set("keys.select", "Return").set("hotkeys.pause", "X").go();
    CHECK(!T::reachable(in, B::BTN_A));
    CHECK(in.collisions()[0].find("UNREACHABLE") != std::string::npos); }
}

// Face presses by position reach the menu whatever they're bound to, and only
// while it is open.
void test_menu_faces_ignore_bindings() {
  Rig r; Input& in = r.set("pad.x", "a").set("pad.y", "b").go();
  T::tap_pad(in, SDL_CONTROLLER_BUTTON_Y);
  CHECK(in.take_menu_faces() == 0);
  in.set_menu_open(true);
  T::tap_pad(in, SDL_CONTROLLER_BUTTON_Y);
  T::tap_pad(in, SDL_CONTROLLER_BUTTON_X);
  CHECK(in.take_menu_faces() == ((1u << 3) | (1u << 2)));
  CHECK(in.take_menu_faces() == 0);
}

// "mod" alone: the modifier released with nothing else pressed while it was
// held fires that hotkey; any other press in between makes it a chord run.
void test_modifier_alone_is_a_hotkey() {
  using SB = SDL_GameControllerButton;
  const auto only = [](Input& in, Action a) { const std::vector<Action> v = in.take_actions(); return v.size() == 1 && v[0] == a; };
  { Rig r; Input& in = r.set("padhotkeys.modifier", "guide").set("padhotkeys.pause", "mod").set("padhotkeys.quit", "mod+start").go();
    T::tap_pad(in, SDL_CONTROLLER_BUTTON_GUIDE);
    CHECK(only(in, Action::Pause));
    T::pad(in, SDL_CONTROLLER_BUTTON_GUIDE, true);
    T::tap_pad(in, SDL_CONTROLLER_BUTTON_START);
    T::pad(in, SDL_CONTROLLER_BUTTON_GUIDE, false);
    CHECK(in.quit() && in.take_actions().empty());          // the chord, and no pause on the modifier's release
    T::pad(in, SDL_CONTROLLER_BUTTON_GUIDE, true);
    T::tap_pad(in, static_cast<SB>(SDL_CONTROLLER_BUTTON_DPAD_UP));
    T::pad(in, SDL_CONTROLLER_BUTTON_GUIDE, false);
    CHECK(in.take_actions().empty()); }                     // a DS button counts as a press too
  { Rig r; Input& in = r.set("hotkeys.modifier", "Left Ctrl").set("hotkeys.pause", "mod").go();
    T::tap_key(in, SDLK_LCTRL);
    CHECK(only(in, Action::Pause)); }
  // Held actions can't fire on release: refused.
  { Rig r; Input& in = r.set("padhotkeys.modifier", "guide").set("padhotkeys.fast_forward", "mod").go();
    T::tap_pad(in, SDL_CONTROLLER_BUTTON_GUIDE);
    CHECK(in.take_actions().empty() && !in.fast_forward_held()); }
  // It takes the lone press from a DS button on the modifier, and says so.
  { Rig r; Input& in = r.set("padhotkeys.modifier", "guide").set("pad.x", "guide").set("padhotkeys.pause", "mod").go();
    T::tap_pad(in, SDL_CONTROLLER_BUTTON_GUIDE);
    CHECK(only(in, Action::Pause));
    CHECK(in.take_menu_presses() == 0);
    bool said = false;
    for (const std::string& w : in.collisions()) said |= w.find("HIDES PAD") != std::string::npos;
    CHECK(said); }
  // Binding a chord in the menu: capture ends on the chord, before the
  // modifier comes up; that release must not fire "mod" (here: close the menu).
  { Rig r; Input& in = r.set("padhotkeys.modifier", "guide").set("padhotkeys.pause", "mod").go();
    in.begin_capture(true);
    T::capture_pad(in, SDL_CONTROLLER_BUTTON_GUIDE, true);
    T::capture_pad(in, SDL_CONTROLLER_BUTTON_START, true);
    CHECK(in.take_capture() == "mod+start");
    CHECK(!in.capturing());
    T::pad(in, SDL_CONTROLLER_BUTTON_START, false);
    T::pad(in, SDL_CONTROLLER_BUTTON_GUIDE, false);
    CHECK(in.take_actions().empty());
    T::tap_pad(in, SDL_CONTROLLER_BUTTON_GUIDE);                // the next lone press still works
    CHECK(only(in, Action::Pause)); }
}

// DS_DUAL_SCREENS: a display index per panel, then touch devices by name
// (case-insensitive substring) or #index; "none" takes touch devices only.
void test_dual_screens_parse() {
  using P = Input::Panel;
  { int up = 0, lo = 1; std::vector<Input::TouchRoute> r; std::string err;
    CHECK(Input::parse_dual_screens("upper=1,lower=0:Goodix:#2,none=pen", up, lo, r, err));
    CHECK(up == 1 && lo == 0 && r.size() == 3);
    CHECK(r[0].panel == P::Lower && r[0].match == "goodix" && r[0].index < 0);
    CHECK(r[1].panel == P::Lower && r[1].index == 2);
    CHECK(r[2].panel == P::None && r[2].match == "pen"); }
  { int up = 0, lo = 1; std::vector<Input::TouchRoute> r; std::string err;
    CHECK(Input::parse_dual_screens("lower=goodix", up, lo, r, err));   // touch only: displays untouched
    CHECK(up == 0 && lo == 1 && r.size() == 1 && r[0].panel == P::Lower); }
  for (const char* bad : {"middle=1", "none=3", "upper"}) {
    int up = 0, lo = 1; std::vector<Input::TouchRoute> r; std::string err;
    CHECK(!Input::parse_dual_screens(bad, up, lo, r, err) && !err.empty());
  }
}

// pad.face_fix's check: SDL's evdev numbering, then each SDL face button's
// kernel key against its position. Values from the RG DS Plus.
void test_face_positions() {
  // Keys: south east north west tl tr tl2 tr2 select start mode thumbl thumbr, dpad.
  const std::vector<int> keys = {0x220, 0x221, 0x222, 0x223, 0x130, 0x131, 0x133, 0x134, 0x136, 0x137,
                                 0x138, 0x139, 0x13a, 0x13b, 0x13c, 0x13d, 0x13e};
  const std::vector<int> codes = sdl_button_codes(keys);
  CHECK(codes.size() == 17 && codes[0] == 0x130 && codes[2] == 0x133 && codes[3] == 0x134 && codes[13] == 0x220);
  int phys[4] = {0, 1, 2, 3};
  { const int bind[4] = {0, 1, 2, 3};                          // ROCKNIX: x:b2 (north), y:b3 (west)
    CHECK(face_positions_from(codes, bind, phys));
    CHECK(phys[0] == 0 && phys[1] == 1 && phys[2] == 3 && phys[3] == 2); }
  { const int bind[4] = {0, 1, 3, 2};                          // positional already: nothing to do
    CHECK(!face_positions_from(codes, bind, phys)); }
  { const int bind[4] = {1, 0, 2, 3};                          // both pairs by label
    CHECK(face_positions_from(codes, bind, phys));
    CHECK(phys[0] == 1 && phys[1] == 0 && phys[2] == 3 && phys[3] == 2); }
  { const int bind[4] = {0, 2, 1, 3};                          // not a pair swap: left alone
    CHECK(!face_positions_from(codes, bind, phys)); }
  { const int bind[4] = {0, 1, 2, 8};                          // y on a shoulder: left alone
    CHECK(!face_positions_from(codes, bind, phys)); }
}

// The pen follows a stick, not one of its two axes, so the Controls page
// stores which stick the player pushed rather than the axis event it saw.
void test_stylus_axis_reads_the_stick() {
  for (const char* a : {"+leftx", "-leftx", "+lefty", "-lefty"})
    CHECK(std::string(Input::stylus_axis_of(a)) == "left");
  for (const char* a : {"+rightx", "-rightx", "+righty", "-righty"})
    CHECK(std::string(Input::stylus_axis_of(a)) == "right");
  // A trigger or a button says nothing about which stick, so the row keeps
  // what it had rather than guessing.
  for (const char* a : {"+lefttrigger", "+righttrigger", "leftstick", "a", "", "+"})
    CHECK(Input::stylus_axis_of(a) == nullptr);
}

// The page offers to reset these rows, so its idea of a default has to be the
// one configure() actually starts from.
void test_extra_defaults_match_configure() {
  CHECK(std::string(Input::mod_default(true)) == "guide");
  CHECK(std::string(Input::mod_default(false)) == "none");
  CHECK(std::string(Input::stylus_button_default()) == "rightstick");
  CHECK(std::string(Input::stylus_dpad_default()) == "none");
  { Config c; CHECK(std::string(Input::stylus_axis_default(c)) == "right"); }
  // "pad.stylus_stick = false" is the old spelling of "none" and still counts.
  { Config c; c.set("pad.stylus_stick", "false");
    CHECK(std::string(Input::stylus_axis_default(c)) == "none"); }
  // And a pad configured through those defaults binds what they say.
  { Rig r; Input& in = r.go();
    in.begin_capture(true);
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_GUIDE, true));
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_GUIDE, false));
    CHECK(in.take_capture() == Input::mod_default(true)); }
}

// Display only: the file keeps SDL's names, and every alias means exactly what
// the SDL spelling means.
void test_pad_labels() {
  CHECK(Input::pad_label("back") == "SELECT");
  CHECK(Input::pad_label("guide") == "FUNC BTN");
  CHECK(Input::pad_label("leftshoulder") == "L1");
  CHECK(Input::pad_label("+righttrigger") == "R2");
  CHECK(Input::pad_label("mod+start+back") == "MOD+START+SELECT");
  CHECK(Input::pad_label("b") == "\x02 EAST");      // the position pip and the position
  CHECK(Input::pad_label("+leftx") == "+LEFTX");    // no entry: upper case, as before
  const char* pairs[][2] = {{"south", "a"}, {"north", "y"}, {"east", "b"}, {"west", "x"},
                            {"select", "back"}, {"func", "guide"}, {"funcbtn", "guide"},
                            {"l1", "leftshoulder"}, {"r1", "rightshoulder"},
                            {"l2", "+lefttrigger"}, {"r2", "+righttrigger"},
                            {"l3", "leftstick"}, {"r3", "rightstick"},
                            {"mod+l1", "mod+leftshoulder"}, {"mod+start+select", "mod+start+back"}};
  for (const auto& p : pairs) CHECK(Input::pad_label(p[0]) == Input::pad_label(p[1]));
}

// The pen's tap button takes a second binding, pad.stylus_button.alt, and
// either one taps; releasing one while the other is held keeps the touch.
void test_stylus_tap_alt_binding() {
  Rig r; r.set("pad.stylus_button.alt", "+righttrigger"); Input& in = r.go();
  CHECK(T::pad(in, SDL_CONTROLLER_BUTTON_RIGHTSTICK, true));   // the default tap
  CHECK(T::pen_down(in));
  CHECK(T::pad_axis(in, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, false, true));   // the second
  CHECK(T::pad(in, SDL_CONTROLLER_BUTTON_RIGHTSTICK, false));
  CHECK(T::pen_down(in));                                       // still held by the alt
  CHECK(T::pad_axis(in, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, false, false));
  CHECK(!T::pen_down(in));
  in.frame();                                                   // the tap itself lasts one frame
  CHECK(in.frame().down == false);
  // Unset, the trigger is nobody's: it falls through like any other control.
  Rig r2; Input& in2 = r2.go();
  CHECK(!T::pad_axis(in2, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, false, true));
  CHECK(!T::pen_down(in2));
}

// pad.stick_face: a stick's four directions are X/B/Y/A, laid out as the DS
// has them, and the pen's stick is never taken.
void test_stick_face_buttons() {
  constexpr u32 kX = 1u << B::BTN_X, kY = 1u << B::BTN_Y;
  {
    Rig r; r.set("pad.stick_face", "left"); r.set("pad.stick_dpad", "false"); Input& in = r.go();
    T::axis(in, SDL_CONTROLLER_AXIS_LEFTY, -30000);           // up: X
    CHECK((in.frame().buttons & (kX | kA | kB | kY)) == kX);
    T::axis(in, SDL_CONTROLLER_AXIS_LEFTY, 0);
    T::axis(in, SDL_CONTROLLER_AXIS_LEFTX, 30000);            // right: A
    CHECK((in.frame().buttons & (kX | kA | kB | kY)) == kA);
    T::axis(in, SDL_CONTROLLER_AXIS_LEFTX, -30000);           // left: Y
    CHECK((in.frame().buttons & (kX | kA | kB | kY)) == kY);
    T::axis(in, SDL_CONTROLLER_AXIS_LEFTX, 0);
    T::axis(in, SDL_CONTROLLER_AXIS_LEFTY, 30000);            // down: B
    CHECK((in.frame().buttons & (kX | kA | kB | kY)) == kB);
    T::axis(in, SDL_CONTROLLER_AXIS_LEFTY, 0);
    CHECK((in.frame().buttons & (kX | kA | kB | kY)) == 0);
    CHECK((in.frame().buttons & (1u << B::BTN_UP)) == 0);   // stick_dpad off: not the d-pad
  }
  {
    // The right stick is the pen by default, so stick_face = right is inert.
    Rig r; r.set("pad.stick_face", "right"); Input& in = r.go();
    T::axis(in, SDL_CONTROLLER_AXIS_RIGHTY, -30000);
    CHECK((in.frame().buttons & (kX | kA | kB | kY)) == 0);
  }
  {
    // Move the pen to the left stick and the right one is free to be ABXY.
    Rig r; r.set("pad.stick_face", "right"); r.set("pad.stylus_axis", "left"); Input& in = r.go();
    T::axis(in, SDL_CONTROLLER_AXIS_RIGHTY, -30000);
    CHECK((in.frame().buttons & (kX | kA | kB | kY)) == kX);
  }
  {
    // Or take the pen off the sticks altogether, as the menu's clear does.
    Rig r; r.set("pad.stick_face", "right"); r.set("pad.stylus_axis", "none"); Input& in = r.go();
    T::axis(in, SDL_CONTROLLER_AXIS_RIGHTY, -30000);
    CHECK((in.frame().buttons & (kX | kA | kB | kY)) == kX);
    CHECK(!in.stylus_visible_binding());
    // ...and the menu never sees it: X there resets the column, which would
    // put the pen back on the stick the player just took it from.
    CHECK(in.take_menu_presses() == 0 && in.menu_held() == 0);
  }
  {
    // The d-pad stick still drives the menu.
    Rig r; Input& in = r.go();
    T::axis(in, SDL_CONTROLLER_AXIS_LEFTY, -30000);
    CHECK(in.take_menu_presses() == (1u << B::BTN_UP));
  }
  { Config c; CHECK(std::string(Input::stick_face_default()) == "none"); }
  CHECK(std::string(Input::stick_dpad_default()) == "left");
}

// pad.stick_dpad names a stick like stick_face does; true/false still read as
// left/none, so an old config keeps its meaning.
void test_stick_dpad_names_a_stick() {
  constexpr u32 kUp = 1u << B::BTN_UP, kDpad = kUp | (1u << B::BTN_DOWN) | (1u << B::BTN_LEFT) | (1u << B::BTN_RIGHT);
  { Rig r; Input& in = r.go();                                           // default: left
    T::axis(in, SDL_CONTROLLER_AXIS_LEFTY, -30000); CHECK((in.frame().buttons & kDpad) == kUp); }
  { Rig r; r.set("pad.stick_dpad", "true"); Input& in = r.go();
    T::axis(in, SDL_CONTROLLER_AXIS_LEFTY, -30000); CHECK((in.frame().buttons & kDpad) == kUp); }
  { Rig r; r.set("pad.stick_dpad", "false"); Input& in = r.go();
    T::axis(in, SDL_CONTROLLER_AXIS_LEFTY, -30000); CHECK((in.frame().buttons & kDpad) == 0); }
  { Rig r; r.set("pad.stick_dpad", "right"); r.set("pad.stylus_axis", "none"); Input& in = r.go();
    T::axis(in, SDL_CONTROLLER_AXIS_RIGHTY, -30000); CHECK((in.frame().buttons & kDpad) == kUp);
    T::axis(in, SDL_CONTROLLER_AXIS_LEFTY, -30000);  CHECK((in.frame().buttons & kDpad) == kUp); }   // left no longer
  { Rig r; r.set("pad.stick_dpad", "right"); Input& in = r.go();          // the pen has the right stick
    T::axis(in, SDL_CONTROLLER_AXIS_RIGHTY, -30000); CHECK((in.frame().buttons & kDpad) == 0); }
}

// Under the pad.stylus_dpad chord the d-pad stick is the pen's, as the d-pad
// is: the game lets go of it for as long as the chord is held, and a stick
// still tilted on release is the d-pad again without having to move.
void test_stick_dpad_moves_the_pen_under_the_chord() {
  constexpr u32 kRight = 1u << B::BTN_RIGHT, kDpad = kRight | (1u << B::BTN_LEFT) | (1u << B::BTN_UP) | (1u << B::BTN_DOWN);
  Rig r; r.set("pad.stylus_dpad", "leftshoulder"); Input& in = r.go();
  T::axis(in, SDL_CONTROLLER_AXIS_LEFTX, 30000);
  CHECK((in.frame().buttons & kDpad) == kRight);
  const int x0 = in.stylus_x();
  in.update_stylus(); CHECK(in.stylus_x() == x0);                          // no chord: not the pen's
  CHECK(T::pad(in, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, true));
  CHECK((in.frame().buttons & kDpad) == 0);                                // held across the press, and let go
  for (int i = 0; i < 8; ++i) in.update_stylus();
  CHECK(in.stylus_x() > x0);
  T::axis(in, SDL_CONTROLLER_AXIS_LEFTX, 30001);
  CHECK((in.frame().buttons & kDpad) == 0);
  CHECK(T::pad(in, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, false));
  CHECK((in.frame().buttons & kDpad) == kRight);
  const int x1 = in.stylus_x();
  in.update_stylus(); CHECK(in.stylus_x() == x1);
}

// pad.axis_<name>: a pad whose left stick SDL reports rotated (X on lefty,
// Y on leftx backwards) is put right by naming the physical axis per
// remapped one; everything downstream sees the corrected stick.
void test_axis_remap() {
  constexpr u32 kUp = 1u << B::BTN_UP, kDown = 1u << B::BTN_DOWN, kLeft = 1u << B::BTN_LEFT, kRight = 1u << B::BTN_RIGHT;
  constexpr u32 kDpad = kUp | kDown | kLeft | kRight;
  { Rig r; Input& in = r.go();                                              // identity by default
    T::physical_axis(in, SDL_CONTROLLER_AXIS_LEFTX, 30000); CHECK((in.frame().buttons & kDpad) == kRight); }
  { Rig r; r.set("pad.axis_leftx", "lefty").set("pad.axis_lefty", "-leftx"); Input& in = r.go();
    T::physical_axis(in, SDL_CONTROLLER_AXIS_LEFTY, 30000);  CHECK((in.frame().buttons & kDpad) == kRight);
    T::physical_axis(in, SDL_CONTROLLER_AXIS_LEFTY, 0);
    T::physical_axis(in, SDL_CONTROLLER_AXIS_LEFTX, 30000);  CHECK((in.frame().buttons & kDpad) == kUp);    // inverted
    T::physical_axis(in, SDL_CONTROLLER_AXIS_LEFTX, -32768); CHECK((in.frame().buttons & kDpad) == kDown);  // no overflow
  }
  { Rig r; r.set("pad.axis_leftx", "none"); Input& in = r.go();
    T::physical_axis(in, SDL_CONTROLLER_AXIS_LEFTX, 30000); CHECK((in.frame().buttons & kDpad) == 0); }
  { Rig r; r.set("pad.axis_leftx", "sideways"); Input& in = r.go();          // unparseable: stays identity
    T::physical_axis(in, SDL_CONTROLLER_AXIS_LEFTX, 30000); CHECK((in.frame().buttons & kDpad) == kRight); }
  {
    // A capture names the remapped axis for a binding and keeps the physical
    // one for an axis row.
    Rig r; r.set("pad.axis_righttrigger", "-righty").set("pad.axis_righty", "none"); Input& in = r.go();
    in.begin_capture(true);
    T::capture_axis(in, SDL_CONTROLLER_AXIS_RIGHTY, -30000);
    CHECK(in.captured_raw_axis() == "-righty");
    CHECK(in.take_capture() == "+righttrigger");
  }
  CHECK(Input::axis_remap_key(SDL_CONTROLLER_AXIS_LEFTY) == "pad.axis_lefty");
  CHECK(Input::axis_remap_default(SDL_CONTROLLER_AXIS_RIGHTX) == "rightx");
}

} // namespace
} // namespace ds::sdl

int main() {
  ds::sdl::test_fallback_is_inert_by_default();
  ds::sdl::test_fallback_rescues_an_orphaned_button();
  ds::sdl::test_fallback_yields_to_a_real_binding();
  ds::sdl::test_fallback_press_does_not_outlive_the_frame();
  ds::sdl::test_escape_is_still_quit();
  ds::sdl::test_escape_cancels_once_quit_moves();
  ds::sdl::test_pad_capture_swallows_keys_and_escape_cancels();
  ds::sdl::test_capture_composes_a_chord_with_the_modifier();
  ds::sdl::test_reachable_counts_every_way_in();
  ds::sdl::test_hotkey_cannot_hide_a_from_the_menu();
  ds::sdl::test_menu_faces_ignore_bindings();
  ds::sdl::test_modifier_alone_is_a_hotkey();
  ds::sdl::test_dual_screens_parse();
  ds::sdl::test_face_positions();
  ds::sdl::test_keyboard_capture_backs_out_on_pad_input();
  ds::sdl::test_stylus_axis_reads_the_stick();
  ds::sdl::test_extra_defaults_match_configure();
  ds::sdl::test_pad_labels();
  ds::sdl::test_stylus_tap_alt_binding();
  ds::sdl::test_stick_face_buttons();
  ds::sdl::test_stick_dpad_names_a_stick();
  ds::sdl::test_stick_dpad_moves_the_pen_under_the_chord();
  ds::sdl::test_axis_remap();
  std::printf("input tests passed\n");
  return 0;
}
