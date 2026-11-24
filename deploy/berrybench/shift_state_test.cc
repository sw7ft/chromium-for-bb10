// Host-side check of the on-screen Shift logic.
//
//   g++ -std=c++17 -I../.. shift_state_test.cc -o /tmp/shift_state_test && /tmp/shift_state_test
//
// Each case is a key sequence as the keyboard could deliver it and the text it
// must produce. What a real Passport sends for its on-screen Shift has not
// been traced yet, so the cases cover the delivery models consistent with
// what has been observed on the device. One is knowingly left out: see "Not
// handled" in qnx_shift_state.h.
#include <cstdio>
#include <string>
#include <vector>

#include "ui/ozone/platform/qnx_screen/qnx_shift_state.h"

namespace {

using ui::QnxShiftState;

const int kShift = ui::kQnxSymLeftShift;
const int kModS = ui::kQnxModShift;
const int kModLock = ui::kQnxModCapsLock;

struct Key {
  int sym;
  int mods = 0;
  bool tap = true;      // down followed by up
  bool down = true;     // used when !tap
  bool repeat = false;
};

Key Down(int sym, int mods = 0) { return {sym, mods, false, true, false}; }
Key Up(int sym, int mods = 0) { return {sym, mods, false, false, false}; }
Key Repeat(int sym, int mods = 0) { return {sym, mods, false, true, true}; }

std::string Type(const std::vector<Key>& keys, std::string* flags = nullptr) {
  QnxShiftState s;
  std::string out;
  auto feed = [&](int sym, int mods, bool down, bool repeat) {
    QnxShiftState::Result r = s.Apply(sym, mods, down, repeat);
    if (down && r.sym >= 0x20 && r.sym < 0x7f) {
      out += static_cast<char>(r.sym);
      if (flags)
        *flags += r.shifted ? 'S' : (r.caps_lock ? 'L' : '-');
    }
  };
  for (const Key& k : keys) {
    if (k.tap) {
      feed(k.sym, k.mods, true, false);
      feed(k.sym, k.mods, false, false);
    } else {
      feed(k.sym, k.mods, k.down, k.repeat);
    }
  }
  return out;
}

int g_failed = 0;

void Expect(const char* name, const std::vector<Key>& keys, const char* want) {
  const std::string got = Type(keys);
  const bool ok = got == want;
  g_failed += !ok;
  std::printf("%-4s %-52s %s%s%s\n", ok ? "ok" : "FAIL", name, got.c_str(),
              ok ? "" : "  want ", ok ? "" : want);
}

}  // namespace

int main() {
  // --- Keyboard sends plain lower-case letters, no modifiers ---------------
  Expect("plain typing", {{'a'}, {'b'}}, "ab");
  Expect("shift, letter, letter", {{kShift}, {'a'}, {'b'}}, "Ab");
  // The reported bug: Shift switched on and off again without typing.
  Expect("shift on, shift off, letter", {{kShift}, {kShift}, {'a'}, {'b'}},
         "ab");
  Expect("shift x3, letter", {{kShift}, {kShift}, {kShift}, {'a'}, {'b'}},
         "Ab");
  Expect("shift, digit, letter", {{kShift}, {'1'}, {'a'}}, "1a");
  Expect("shift, space, letter", {{kShift}, {' '}, {'a'}}, " a");
  Expect("two capitals", {{kShift}, {'a'}, {kShift}, {'b'}, {'c'}}, "ABc");
  Expect("shift held with key repeat",
         {Down(kShift), Repeat(kShift), Repeat(kShift), Up(kShift), {'a'},
          {'b'}},
         "Ab");
  Expect("letter auto-repeat keeps its case",
         {{kShift}, Down('a'), Repeat('a'), Repeat('a'), Up('a'), {'b'}},
         "AAAb");

  // --- Keyboard sends the capital itself -----------------------------------
  // The old code left Shift armed here, so the *next* letter doubled up.
  Expect("shift, 'A' from keyboard, letter", {{kShift}, {'A'}, {'b'}}, "Ab");
  Expect("capital with no shift key at all", {{'A'}, {'b'}}, "Ab");
  Expect("caps lock as capitals, then off",
         {{kShift}, {kShift}, {'A'}, {'B'}, {kShift}, {'c'}, {'d'}}, "ABcd");
  Expect("caps lock as capitals, off, then one shift",
         {{kShift}, {kShift}, {'A'}, {kShift}, {kShift}, {'b'}, {'c'}}, "ABc");
  Expect("caps lock key, then a shift tap",
         {{ui::kQnxSymCapsLock}, {'A'}, {ui::kQnxSymCapsLock}, {kShift}, {'b'},
          {'c'}},
         "ABc");

  // --- Keyboard reports Shift in the modifiers -----------------------------
  Expect("one-shot via modifiers", {{kShift}, {'a', kModS}, {'b'}}, "Ab");
  Expect("held shift (physical keyboard)",
         {Down(kShift, kModS), {'a', kModS}, {'b', kModS}, Up(kShift), {'c'}},
         "ABc");
  Expect("sticky shift via modifiers, then off",
         {{kShift}, {'a', kModS}, {'b', kModS}, {kShift}, {'c'}, {'d'}},
         "ABcd");
  Expect("caps lock via modifiers, then off",
         {{kShift}, {kShift}, {'a', kModLock}, {'b', kModLock}, {kShift}, {'c'},
          {'d'}},
         "ABcd");
  Expect("caps lock via modifiers, single tap on",
         {{kShift}, {'a', kModLock}, {'b', kModLock}, {kShift}, {'c'}}, "ABc");
  Expect("capital after a capital, modifiers",
         {{kShift}, {'a', kModS}, {kShift}, {'b', kModS}, {'c'}}, "ABc");

  // --- Modifier flags handed to the page -----------------------------------
  {
    std::string flags;
    const std::string got =
        Type({{kShift}, {'a'}, {'b'}, {'c', kModLock}, {'D'}}, &flags);
    const bool ok = got == "AbCD" && flags == "S-L-";
    g_failed += !ok;
    std::printf("%-4s %-52s %s %s\n", ok ? "ok" : "FAIL", "modifier flags",
                got.c_str(), flags.c_str());
  }

  std::printf("shift state: %s\n", g_failed ? "FAILED" : "all passed");
  return g_failed ? 1 : 0;
}
