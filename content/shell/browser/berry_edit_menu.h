// Copyright 2026 SW7FT. All rights reserved.
// Cut / Copy / Paste / Select all menu for touch text selection.

#ifndef CONTENT_SHELL_BROWSER_BERRY_EDIT_MENU_H_
#define CONTENT_SHELL_BROWSER_BERRY_EDIT_MENU_H_

#include <memory>

#include "base/memory/weak_ptr.h"
#include "ui/touch_selection/touch_selection_menu_runner.h"

namespace content {

// The engine already does touch selection: a long press selects a word and
// shows drag handles, and it knows how to cut, copy and paste. What it asks
// the embedder for is the small menu that offers those commands, and without
// one (content_shell has no UI toolkit) a selection could be made but never
// used. This is that menu.
//
// It is a compositor layer of its own stacked above the page, the same way
// the selection handles are drawn, so it appears whether the page is rendered
// by the GPU or in software.
class BerryEditMenuRunner : public ui::TouchSelectionMenuRunner {
 public:
  BerryEditMenuRunner();
  BerryEditMenuRunner(const BerryEditMenuRunner&) = delete;
  BerryEditMenuRunner& operator=(const BerryEditMenuRunner&) = delete;
  ~BerryEditMenuRunner() override;

  // Height of a strip along the top edge that the menu must stay out of,
  // because touches there never reach it (the browser toolbar takes them).
  void set_top_inset(int inset) { top_inset_ = inset; }

  // ui::TouchSelectionMenuRunner:
  bool IsMenuAvailable(
      const ui::TouchSelectionMenuClient* client) const override;
  void OpenMenu(base::WeakPtr<ui::TouchSelectionMenuClient> client,
                const gfx::Rect& anchor_rect,
                const gfx::Size& handle_image_size,
                aura::Window* context) override;
  void CloseMenu() override;
  bool IsRunning() const override;

 private:
  class Menu;

  // An item was tapped. Runs after the touch has been fully dispatched.
  void Execute(int command);

  std::unique_ptr<Menu> menu_;
  base::WeakPtr<ui::TouchSelectionMenuClient> client_;
  int top_inset_ = 0;
  base::WeakPtrFactory<BerryEditMenuRunner> weak_factory_{this};
};

}  // namespace content

#endif  // CONTENT_SHELL_BROWSER_BERRY_EDIT_MENU_H_
