// Copyright 2026 SW7FT. All rights reserved.
// Input injection for the on-device bench tests (deploy/berrybench).

#ifndef CONTENT_SHELL_BROWSER_BERRY_TEST_HOOKS_H_
#define CONTENT_SHELL_BROWSER_BERRY_TEST_HOOKS_H_

namespace aura {
class WindowTreeHost;
}

namespace content {

// Does nothing unless the environment variable BERRY_TEST_CMD names a file;
// the launcher never sets it, so an installed browser never runs this.
//
// A bench engine has no touch screen and no Navigator, and DevTools cannot
// stand in for them: its synthetic touches are delivered straight to the page
// and skip the gesture recogniser, so a long press never selects anything,
// and it has no notion of an invocation from another app. With the variable
// set, the engine polls that file and performs the commands written to it
// (one per line), delivering them exactly where the real ones enter:
//
//   touch X Y HOLD_MS    finger down at X,Y (window coordinates), up after
//                        HOLD_MS; a long hold is a long press
//   invoke TEXT          an invocation carrying TEXT ("\n" for a newline)
//
// The file is deleted once its commands have been started.
void BerryStartTestHooks(aura::WindowTreeHost* host);

}  // namespace content

#endif  // CONTENT_SHELL_BROWSER_BERRY_TEST_HOOKS_H_
