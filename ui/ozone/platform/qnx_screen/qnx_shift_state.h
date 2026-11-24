// Copyright 2026 SW7FT. All rights reserved.
// Shift handling for BB10 keyboards whose Shift is an on-screen key.

#ifndef UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SHIFT_STATE_H_
#define UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SHIFT_STATE_H_

namespace ui {

// Values from <sys/keycodes.h>, repeated here so this header has no QNX
// dependency and its logic can be exercised on the build host
// (deploy/berrybench/shift_state_test.cc). qnx_screen_event_source.cc
// static_asserts that they match the SDK.
inline constexpr int kQnxSymLeftShift = 0xf0e1;
inline constexpr int kQnxSymRightShift = 0xf0e2;
inline constexpr int kQnxSymCapsLock = 0xf0e5;
inline constexpr int kQnxModShift = 1 << 0;
inline constexpr int kQnxModShiftLock = 1 << 8;
inline constexpr int kQnxModCapsLock = 1 << 16;

// Decides whether a letter must be upper-cased.
//
// A touch keyboard's Shift is tapped, not held, so the letter that follows
// can arrive as a plain lower-case symbol with no Shift modifier. A tap is
// therefore remembered ("armed") until the next character.
//
// Three rules keep that memory from going stale, which is what made a letter
// come out capital after Shift had already been switched off:
//   * A second tap disarms. Shift on, then off, leaves it off.
//   * Any character uses it up, not only one that needed it. If the keyboard
//     sends the capital itself, the tap must not carry over to the letter
//     after it.
//   * When the previous character shows the keyboard was applying Shift or a
//     lock on its own (in the modifiers, or by sending a capital), a tap on
//     Shift is the user turning that off, so it does not arm.
//
// Not handled: a keyboard that reports nothing in the modifiers and signals
// "Shift on" with a key-down and "Shift off" with the matching key-up some
// time later. Switching Shift on and off again without typing would then
// still leave it armed. Telling that apart from an ordinary tap needs a trace
// from such a device (QNX_KEY_TRACE).
class QnxShiftState {
 public:
  struct Result {
    int sym = 0;             // Symbol to deliver.
    bool shifted = false;    // Report the Shift modifier with it.
    bool caps_lock = false;  // Report the Caps Lock modifier with it.
  };

  Result Apply(int sym, int mods, bool down, bool repeat) {
    Result r;
    r.sym = sym;
    r.shifted = (mods & kQnxModShift) != 0;
    r.caps_lock = (mods & (kQnxModShiftLock | kQnxModCapsLock)) != 0;

    if (sym == kQnxSymLeftShift || sym == kQnxSymRightShift) {
      if (down && !repeat) {
        armed_ = keyboard_shift_active_ ? false : !armed_;
        keyboard_shift_active_ = false;
      }
      return r;
    }
    if (sym == kQnxSymCapsLock) {
      if (down && !repeat) {
        armed_ = false;
        keyboard_shift_active_ = false;
      }
      return r;
    }

    if (!down || repeat) {
      // Release and auto-repeat follow whatever the press was given.
      if (sym == pressed_sym_) {
        r.sym = pressed_out_;
        r.shifted = r.shifted || pressed_shifted_;
        if (!down)
          pressed_sym_ = 0;
      }
      return r;
    }

    const bool printable = sym >= 0x20 && sym < 0xE000;
    if (sym >= 'a' && sym <= 'z' && (r.shifted || r.caps_lock || armed_)) {
      r.sym = sym - 'a' + 'A';
      r.shifted = r.shifted || !r.caps_lock;
    }
    if (printable) {
      keyboard_shift_active_ =
          (mods & (kQnxModShift | kQnxModShiftLock | kQnxModCapsLock)) != 0 ||
          (sym >= 'A' && sym <= 'Z');
      armed_ = false;
    }

    pressed_sym_ = sym;
    pressed_out_ = r.sym;
    pressed_shifted_ = r.shifted;
    return r;
  }

  // The keyboard went away; nothing typed later belongs to an earlier tap.
  void Reset() {
    armed_ = false;
    keyboard_shift_active_ = false;
    pressed_sym_ = 0;
  }

  bool armed() const { return armed_; }

 private:
  bool armed_ = false;
  // The last character was shifted by the keyboard itself: Shift or a lock
  // was set in its modifiers, or it arrived as a capital.
  bool keyboard_shift_active_ = false;
  int pressed_sym_ = 0;
  int pressed_out_ = 0;
  bool pressed_shifted_ = false;
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SHIFT_STATE_H_
