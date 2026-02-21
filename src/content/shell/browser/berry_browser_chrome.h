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
  static constexpr int kToolbarHeight = 56;
  static constexpr int kButtonWidth = 44;

  BerryBrowserChrome(int width, int height);
  ~BerryBrowserChrome();

  void SetUrl(const std::string& url);
  const std::string& url() const { return url_; }
  void SetLoading(bool loading);
  void SetCanGoBack(bool can);
  void SetCanGoForward(bool can);

  void OnChar(char c);
  void OnBackspace();
  void OnEnter();

  void Paint(SkCanvas* canvas);
  gfx::Rect GetWebContentBounds() const;

  bool url_committed() const { return url_committed_; }
  void ClearCommit() { url_committed_ = false; }
  const std::string& committed_url() const { return committed_url_; }

  enum class HitResult { kNone, kBack, kForward, kReload, kUrlBar };
  HitResult HitTest(int x, int y) const;

 private:
  void PaintButton(SkCanvas* canvas, const gfx::Rect& r,
                   const char* label, bool enabled);

  int width_, height_;
  std::string url_;
  std::string edit_text_;
  std::string committed_url_;
  bool editing_ = false;
  bool loading_ = false;
  bool can_go_back_ = false;
  bool can_go_forward_ = false;
  bool url_committed_ = false;

  gfx::Rect back_rect_, forward_rect_, reload_rect_, url_rect_;
};

}  // namespace content

#endif
