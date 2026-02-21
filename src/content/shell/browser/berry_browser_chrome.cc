// Copyright 2025 SW7FT. All rights reserved.
// BerryBrowserNative chrome UI -- Skia-rendered toolbar

#include "content/shell/browser/berry_browser_chrome.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkRect.h"
#include "third_party/skia/include/core/SkTypeface.h"

namespace content {

BerryBrowserChrome::BerryBrowserChrome(int width, int height)
    : width_(width), height_(height) {
  int x = 4;
  int y = 6;
  int bh = kToolbarHeight - 12;
  back_rect_ = gfx::Rect(x, y, kButtonWidth, bh);
  x += kButtonWidth + 2;
  forward_rect_ = gfx::Rect(x, y, kButtonWidth, bh);
  x += kButtonWidth + 2;
  reload_rect_ = gfx::Rect(x, y, kButtonWidth, bh);
  x += kButtonWidth + 8;
  url_rect_ = gfx::Rect(x, y, width_ - x - 8, bh);
}

BerryBrowserChrome::~BerryBrowserChrome() = default;

void BerryBrowserChrome::SetUrl(const std::string& url) {
  url_ = url;
  if (!editing_) edit_text_ = url;
}

void BerryBrowserChrome::SetLoading(bool loading) { loading_ = loading; }
void BerryBrowserChrome::SetCanGoBack(bool c) { can_go_back_ = c; }
void BerryBrowserChrome::SetCanGoForward(bool c) { can_go_forward_ = c; }

void BerryBrowserChrome::OnChar(char c) {
  editing_ = true;
  edit_text_ += c;
}

void BerryBrowserChrome::OnBackspace() {
  editing_ = true;
  if (!edit_text_.empty()) edit_text_.pop_back();
}

void BerryBrowserChrome::OnEnter() {
  committed_url_ = edit_text_;
  url_committed_ = true;
  editing_ = false;
}

gfx::Rect BerryBrowserChrome::GetWebContentBounds() const {
  return gfx::Rect(0, kToolbarHeight, width_, height_ - kToolbarHeight);
}

BerryBrowserChrome::HitResult BerryBrowserChrome::HitTest(int x, int y) const {
  gfx::Point p(x, y);
  if (back_rect_.Contains(p)) return HitResult::kBack;
  if (forward_rect_.Contains(p)) return HitResult::kForward;
  if (reload_rect_.Contains(p)) return HitResult::kReload;
  if (url_rect_.Contains(p)) return HitResult::kUrlBar;
  return HitResult::kNone;
}

void BerryBrowserChrome::Paint(SkCanvas* canvas) {
  // Toolbar background
  SkPaint bg;
  bg.setColor(SkColorSetRGB(33, 33, 33));
  canvas->drawRect(SkRect::MakeXYWH(0, 0, width_, kToolbarHeight), bg);

  // Separator line
  SkPaint sep;
  sep.setColor(SkColorSetRGB(60, 60, 60));
  canvas->drawRect(SkRect::MakeXYWH(0, kToolbarHeight - 1, width_, 1), sep);

  PaintButton(canvas, back_rect_, "<", can_go_back_);
  PaintButton(canvas, forward_rect_, ">", can_go_forward_);
  PaintButton(canvas, reload_rect_, loading_ ? "X" : "R", true);

  // URL bar
  SkPaint url_bg;
  url_bg.setColor(SkColorSetRGB(48, 48, 48));
  url_bg.setStyle(SkPaint::kFill_Style);
  canvas->drawRoundRect(
      SkRect::MakeXYWH(url_rect_.x(), url_rect_.y(),
                        url_rect_.width(), url_rect_.height()),
      4, 4, url_bg);

  SkPaint url_border;
  url_border.setColor(editing_ ? SkColorSetRGB(100, 160, 255) :
                                  SkColorSetRGB(80, 80, 80));
  url_border.setStyle(SkPaint::kStroke_Style);
  url_border.setStrokeWidth(editing_ ? 2 : 1);
  canvas->drawRoundRect(
      SkRect::MakeXYWH(url_rect_.x(), url_rect_.y(),
                        url_rect_.width(), url_rect_.height()),
      4, 4, url_border);

  // URL text
  SkFont font;
  font.setSize(16);
  SkPaint text_paint;
  text_paint.setColor(SkColorSetRGB(220, 220, 220));
  const std::string& display = editing_ ? edit_text_ : url_;
  canvas->drawString(display.c_str(),
                     url_rect_.x() + 8, url_rect_.y() + 28,
                     font, text_paint);

  if (editing_) {
    // Cursor
    float tw = font.measureText(display.c_str(), display.size(),
                                 SkTextEncoding::kUTF8);
    SkPaint cursor_paint;
    cursor_paint.setColor(SkColorSetRGB(100, 160, 255));
    canvas->drawRect(SkRect::MakeXYWH(url_rect_.x() + 8 + tw,
                                       url_rect_.y() + 8,
                                       2, url_rect_.height() - 16),
                     cursor_paint);
  }
}

void BerryBrowserChrome::PaintButton(SkCanvas* canvas, const gfx::Rect& r,
                                     const char* label, bool enabled) {
  SkPaint bg;
  bg.setColor(enabled ? SkColorSetRGB(55, 55, 55) : SkColorSetRGB(40, 40, 40));
  canvas->drawRoundRect(
      SkRect::MakeXYWH(r.x(), r.y(), r.width(), r.height()), 4, 4, bg);

  SkFont font;
  font.setSize(20);
  SkPaint tp;
  tp.setColor(enabled ? SkColorSetRGB(220, 220, 220) :
                        SkColorSetRGB(100, 100, 100));
  canvas->drawString(label, r.x() + r.width() / 2 - 6,
                     r.y() + r.height() / 2 + 7, font, tp);
}

}  // namespace content
