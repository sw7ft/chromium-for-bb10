// Copyright 2025 SW7FT. All rights reserved.
// BerryBrowserNative chrome UI for QNX Screen

#ifndef CONTENT_SHELL_BROWSER_BERRY_BROWSER_CHROME_H_
#define CONTENT_SHELL_BROWSER_BERRY_BROWSER_CHROME_H_

#include <string>
#include "third_party/skia/include/core/SkCanvas.h"
#include "ui/gfx/geometry/rect.h"

namespace content {

class BerryBrowserChrome {
 public:
  // The metrics below were tuned against a 540px short axis (the launcher's
  // default resolution tier) and are scaled to the actual render surface. A
  // fixed pixel height does not survive the tier picker: at the 1440 tier a
  // 56px bar is about 3mm tall and impossible to hit, at the 420 tier it is
  // about 11mm and eats the screen.
  static constexpr int kReferenceShortAxis = 540;
  static constexpr int kToolbarHeightAtReference = 56;
  static constexpr int kButtonWidthAtReference = 44;

  BerryBrowserChrome(int width, int height);
  ~BerryBrowserChrome();

  int toolbar_height() const { return toolbar_height_; }

  void SetUrl(const std::string& url);
  const std::string& url() const { return url_; }
  void SetLoading(bool loading);
  bool loading() const { return loading_; }
  void SetCanGoBack(bool can);
  void SetCanGoForward(bool can);

  void StartEditing();
  void CancelEditing();
  void OnChar(char c);
  void OnBackspace();
  void OnEnter();

  // True after StartEditing(): first printable key replaces the whole field
  // (standard omnibox behavior — avoids appending a query to https://.../ ).

  void Paint(SkCanvas* canvas);
  gfx::Rect GetWebContentBounds() const;

  bool url_committed() const { return url_committed_; }
  void ClearCommit() { url_committed_ = false; }
  const std::string& committed_url() const { return committed_url_; }

  void SetWindowCount(int count);
  int window_count() const { return window_count_; }

  enum class HitResult {
    kNone,
    kBack,
    kForward,
    kReload,
    kUrlBar,
    kShare,
    kWindows
  };
  HitResult HitTest(int x, int y) const;

 private:
  void PaintButton(SkCanvas* canvas, const gfx::Rect& r,
                   const char* label, bool enabled);

  int width_, height_;
  int toolbar_height_;
  int button_width_;
  int url_font_size_;
  int button_font_size_;
  std::string url_;
  std::string edit_text_;
  std::string committed_url_;
  bool editing_ = false;
  bool loading_ = false;
  bool can_go_back_ = false;
  bool can_go_forward_ = false;
  bool url_committed_ = false;
  bool replace_all_on_next_input_ = false;

  int window_count_ = 1;
  gfx::Rect back_rect_, forward_rect_, reload_rect_, url_rect_, share_rect_,
      windows_rect_;
};

}  // namespace content

#endif
