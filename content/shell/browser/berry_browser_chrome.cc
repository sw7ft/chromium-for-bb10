// Copyright 2025 SW7FT. All rights reserved.
// BerryBrowserNative chrome UI -- Skia-rendered toolbar

#include "content/shell/browser/berry_browser_chrome.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkRect.h"
#include "third_party/skia/include/core/SkTypeface.h"

namespace content {

namespace {

// Scale a reference metric to this render surface, clamped so it stays both
// tappable on a 384x640 Z10 and sane on a 1440x1440 Passport.
int ScaleMetric(int at_reference, int short_axis, int lo, int hi) {
  const int v = at_reference * short_axis /
                BerryBrowserChrome::kReferenceShortAxis;
  return std::clamp(v, lo, hi);
}

}  // namespace

BerryBrowserChrome::BerryBrowserChrome(int width, int height)
    : width_(width), height_(height) {
  const int short_axis = std::max(1, std::min(width_, height_));
  toolbar_height_ =
      ScaleMetric(kToolbarHeightAtReference, short_axis, 40, 160);
  button_width_ = ScaleMetric(kButtonWidthAtReference, short_axis, 32, 128);
  url_font_size_ = ScaleMetric(16, short_axis, 12, 48);
  button_font_size_ = ScaleMetric(20, short_axis, 14, 56);

  const int pad = std::max(2, toolbar_height_ / 9);
  const int gap = std::max(1, toolbar_height_ / 28);
  const int bh = toolbar_height_ - 2 * pad;
  int x = std::max(2, toolbar_height_ / 14);
  const int y = pad;

  back_rect_ = gfx::Rect(x, y, button_width_, bh);
  x += button_width_ + gap;
  forward_rect_ = gfx::Rect(x, y, button_width_, bh);
  x += button_width_ + gap;
  reload_rect_ = gfx::Rect(x, y, button_width_, bh);
  x += button_width_ + 4 * gap;
  windows_rect_ =
      gfx::Rect(width_ - 4 * gap - button_width_, y, button_width_, bh);
  share_rect_ = gfx::Rect(windows_rect_.x() - gap - button_width_, y,
                          button_width_, bh);
  url_rect_ = gfx::Rect(x, y, std::max(bh, share_rect_.x() - x - 3 * gap), bh);
}

BerryBrowserChrome::~BerryBrowserChrome() = default;

void BerryBrowserChrome::SetUrl(const std::string& url) {
  url_ = url;
  if (!editing_) edit_text_ = url;
}

void BerryBrowserChrome::SetLoading(bool loading) { loading_ = loading; }
void BerryBrowserChrome::SetCanGoBack(bool c) { can_go_back_ = c; }
void BerryBrowserChrome::SetCanGoForward(bool c) { can_go_forward_ = c; }
void BerryBrowserChrome::SetWindowCount(int count) {
  window_count_ = count > 0 ? count : 1;
}

void BerryBrowserChrome::StartEditing() {
  editing_ = true;
  edit_text_.clear();
  replace_all_on_next_input_ = false;
}

void BerryBrowserChrome::CancelEditing() {
  editing_ = false;
  edit_text_ = url_;
  replace_all_on_next_input_ = false;
}

void BerryBrowserChrome::OnChar(char c) {
  editing_ = true;
  if (replace_all_on_next_input_) {
    edit_text_.clear();
    replace_all_on_next_input_ = false;
  }
  edit_text_ += c;
}

void BerryBrowserChrome::OnBackspace() {
  editing_ = true;
  replace_all_on_next_input_ = false;
  if (!edit_text_.empty())
    edit_text_.pop_back();
}

void BerryBrowserChrome::OnEnter() {
  if (edit_text_.empty()) {
    url_committed_ = false;
    editing_ = false;
    edit_text_ = url_;
    return;
  }
  committed_url_ = edit_text_;
  url_committed_ = true;
  editing_ = false;
}

gfx::Rect BerryBrowserChrome::GetWebContentBounds() const {
  return gfx::Rect(0, toolbar_height_, width_, height_ - toolbar_height_);
}

BerryBrowserChrome::HitResult BerryBrowserChrome::HitTest(int x, int y) const {
  gfx::Point p(x, y);
  if (back_rect_.Contains(p)) return HitResult::kBack;
  if (forward_rect_.Contains(p)) return HitResult::kForward;
  if (reload_rect_.Contains(p)) return HitResult::kReload;
  if (share_rect_.Contains(p)) return HitResult::kShare;
  if (windows_rect_.Contains(p)) return HitResult::kWindows;
  if (url_rect_.Contains(p)) return HitResult::kUrlBar;
  return HitResult::kNone;
}

void BerryBrowserChrome::Paint(SkCanvas* canvas) {
  const float radius = std::max(2, toolbar_height_ / 14);
  const float text_inset = std::max(4, url_font_size_ / 2);

  // Toolbar background — Ubuntu aubergine
  SkPaint bg;
  bg.setColor(SkColorSetRGB(44, 0, 30));
  canvas->drawRect(SkRect::MakeXYWH(0, 0, width_, toolbar_height_), bg);

  // Separator line
  SkPaint sep;
  sep.setColor(SkColorSetRGB(94, 39, 80));
  canvas->drawRect(SkRect::MakeXYWH(0, toolbar_height_ - 1, width_, 1), sep);

  PaintButton(canvas, back_rect_, "<", can_go_back_);
  PaintButton(canvas, forward_rect_, ">", can_go_forward_);
  PaintButton(canvas, reload_rect_, loading_ ? "X" : "R", true);
  PaintButton(canvas, share_rect_, "S", true);
  char wlabel[8];
  snprintf(wlabel, sizeof(wlabel), "%d", window_count_);
  PaintButton(canvas, windows_rect_, wlabel, window_count_ > 1);

  // URL bar
  SkPaint url_bg;
  url_bg.setColor(SkColorSetRGB(26, 0, 18));
  url_bg.setStyle(SkPaint::kFill_Style);
  canvas->drawRoundRect(
      SkRect::MakeXYWH(url_rect_.x(), url_rect_.y(),
                        url_rect_.width(), url_rect_.height()),
      radius, radius, url_bg);

  SkPaint url_border;
  url_border.setColor(editing_ ? SkColorSetRGB(233, 84, 32) :
                                  SkColorSetRGB(94, 39, 80));
  url_border.setStyle(SkPaint::kStroke_Style);
  url_border.setStrokeWidth(editing_ ? 2 : 1);
  canvas->drawRoundRect(
      SkRect::MakeXYWH(url_rect_.x(), url_rect_.y(),
                        url_rect_.width(), url_rect_.height()),
      radius, radius, url_border);

  // URL text — snapshot members so concurrent SetUrl (UI thread) cannot
  // invalidate c_str() while drawString runs during PresentCanvas (Navigator
  // path repaints at ~60fps while DDG same-doc navigations update the bar).
  SkFont font;
  font.setSize(url_font_size_);
  SkPaint text_paint;
  text_paint.setColor(SkColorSetRGB(243, 230, 239));
  const std::string display = editing_ ? edit_text_ : url_;
  const char* draw_text = display.c_str();
  if (editing_ && edit_text_.empty()) {
    text_paint.setColor(SkColorSetRGB(196, 160, 184));
    draw_text = "Search or enter address";
  }
  const float baseline =
      url_rect_.y() + url_rect_.height() / 2.0f + url_font_size_ * 0.36f;
  canvas->drawString(draw_text, url_rect_.x() + text_inset, baseline, font,
                     text_paint);

  if (editing_) {
    // Cursor
    float tw = editing_ && !edit_text_.empty()
                   ? font.measureText(display.c_str(), display.size(),
                                      SkTextEncoding::kUTF8)
                   : 0.f;
    const float cursor_inset = std::max(3, url_rect_.height() / 6);
    SkPaint cursor_paint;
    cursor_paint.setColor(SkColorSetRGB(233, 84, 32));
    canvas->drawRect(
        SkRect::MakeXYWH(url_rect_.x() + text_inset + tw,
                         url_rect_.y() + cursor_inset, 2,
                         url_rect_.height() - 2 * cursor_inset),
        cursor_paint);
  }
}

void BerryBrowserChrome::PaintButton(SkCanvas* canvas, const gfx::Rect& r,
                                     const char* label, bool enabled) {
  const float radius = std::max(2, toolbar_height_ / 14);
  SkPaint bg;
  bg.setColor(enabled ? SkColorSetRGB(94, 39, 80) : SkColorSetRGB(60, 8, 54));
  canvas->drawRoundRect(
      SkRect::MakeXYWH(r.x(), r.y(), r.width(), r.height()), radius, radius,
      bg);

  SkFont font;
  font.setSize(button_font_size_);
  SkPaint tp;
  tp.setColor(enabled ? SkColorSetRGB(243, 230, 239) :
                        SkColorSetRGB(138, 106, 128));
  const float tw =
      font.measureText(label, strlen(label), SkTextEncoding::kUTF8);
  canvas->drawString(label, r.x() + (r.width() - tw) / 2.0f,
                     r.y() + r.height() / 2.0f + button_font_size_ * 0.36f,
                     font, tp);
}

}  // namespace content
