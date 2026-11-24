// Copyright 2026 SW7FT. All rights reserved.

#include "content/shell/browser/berry_edit_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/qnx_trace.h"
#include "base/task/sequenced_task_runner.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkFontMetrics.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "ui/aura/client/screen_position_client.h"
#include "ui/aura/window.h"
#include "ui/aura/window_delegate.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/hit_test.h"
#include "ui/base/pointer/touch_editing_controller.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/paint_recorder.h"
#include "ui/events/event.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/image/image_skia.h"

namespace content {

namespace {

// Commands offered, in display order.
struct Command {
  int id;
  const char* label;
};
constexpr Command kCommands[] = {
    {ui::TouchEditable::kCut, "Cut"},
    {ui::TouchEditable::kCopy, "Copy"},
    {ui::TouchEditable::kPaste, "Paste"},
    {ui::TouchEditable::kSelectAll, "Select all"},
};

// Sizes for a 540-pixel-wide window (the Passport's render size; each of
// those is 2.67 screen pixels, about 0.15 mm), scaled with the window for
// other devices. A button is 50 high, 7.5 mm: a comfortable thumb target.
constexpr int kReferenceWidth = 540;
constexpr float kItemHeight = 50;
constexpr float kItemPadding = 16;
constexpr float kFontSize = 18;
constexpr float kCornerRadius = 8;
// Gap between the menu and the text (or the handles under it).
constexpr float kAnchorGap = 8;
constexpr float kEdgeMargin = 4;

constexpr SkColor kBackground = SkColorSetRGB(0x2b, 0x2b, 0x2f);
constexpr SkColor kBorder = SkColorSetRGB(0x6a, 0x6a, 0x72);
constexpr SkColor kPressed = SkColorSetRGB(0x00, 0x7a, 0xc9);
constexpr SkColor kText = SK_ColorWHITE;

}  // namespace

class BerryEditMenuRunner::Menu : public aura::WindowDelegate {
 public:
  struct Item {
    int command;
    const char* label;
    gfx::Rect rect;  // In the menu's own coordinates.
  };

  using ChosenCallback = base::RepeatingCallback<void(int command)>;

  Menu(const std::vector<Command>& commands, float scale, ChosenCallback chosen)
      : scale_(scale), chosen_(std::move(chosen)) {
    font_.setSize(kFontSize * scale_);
    font_.setEdging(SkFont::Edging::kAntiAlias);
    const int height = Scaled(kItemHeight);
    int x = 0;
    for (const Command& c : commands) {
      const float text_width =
          font_.measureText(c.label, strlen(c.label), SkTextEncoding::kUTF8);
      const int width =
          static_cast<int>(text_width + 2 * kItemPadding * scale_ + 0.5f);
      items_.push_back({c.id, c.label, gfx::Rect(x, 0, width, height)});
      x += width;
    }
    size_ = gfx::Size(x, height);
    Render();
  }

  Menu(const Menu&) = delete;
  Menu& operator=(const Menu&) = delete;

  ~Menu() override {
    // The window calls back into this delegate while it is torn down.
    window_.reset();
  }

  const gfx::Size& size() const { return size_; }
  const std::vector<Item>& items() const { return items_; }

  void Show(aura::Window* root, const gfx::Rect& bounds) {
    window_ = std::make_unique<aura::Window>(this);
    window_->SetName("BerryEditMenu");
    window_->SetTransparent(true);
    window_->Init(ui::LAYER_TEXTURED);
    window_->set_owned_by_parent(false);
    root->AddChild(window_.get());
    // After AddChild: the root's layout stretches a new child to fill it.
    window_->SetBounds(bounds);
    root->StackChildAtTop(window_.get());
    window_->Show();
  }

  // Writes the rendered menu as a PNG; lets a test look at the pixels, which
  // the DevTools screenshot (page content only) does not include.
  void DumpPng(const char* path) const {
    std::vector<unsigned char> png;
    if (!gfx::PNGCodec::EncodeBGRASkBitmap(bitmap_, false, &png))
      return;
    if (FILE* f = fopen(path, "wb")) {
      fwrite(png.data(), 1, png.size(), f);
      fclose(f);
    }
  }

 private:
  int Scaled(float v) const { return static_cast<int>(v * scale_ + 0.5f); }

  int ItemAt(const gfx::Point& p) const {
    for (size_t i = 0; i < items_.size(); ++i) {
      if (items_[i].rect.Contains(p))
        return static_cast<int>(i);
    }
    return -1;
  }

  // Draws the menu into |bitmap_|. Text is rasterised here with Skia directly
  // (as the toolbar does) rather than through gfx::RenderText, which needs a
  // font stack this platform does not have.
  void Render() {
    SkBitmap bitmap;
    bitmap.allocN32Pixels(size_.width(), size_.height());
    bitmap.eraseColor(SK_ColorTRANSPARENT);
    SkCanvas canvas(bitmap);

    const SkRect bounds = SkRect::MakeWH(size_.width(), size_.height());
    const float radius = kCornerRadius * scale_;
    const SkRRect outline = SkRRect::MakeRectXY(bounds, radius, radius);

    SkPaint fill;
    fill.setAntiAlias(true);
    fill.setColor(kBackground);
    canvas.drawRRect(outline, fill);

    if (pressed_ >= 0) {
      canvas.save();
      canvas.clipRRect(outline, true);
      SkPaint pressed;
      pressed.setColor(kPressed);
      canvas.drawRect(gfx::RectToSkRect(items_[pressed_].rect), pressed);
      canvas.restore();
    }

    // A light edge keeps the menu readable on a dark page.
    SkPaint border;
    border.setAntiAlias(true);
    border.setStyle(SkPaint::kStroke_Style);
    border.setStrokeWidth(1);
    border.setColor(kBorder);
    canvas.drawRRect(
        SkRRect::MakeRectXY(bounds.makeInset(0.5f, 0.5f), radius, radius),
        border);

    SkPaint line;
    line.setColor(kBorder);
    for (size_t i = 1; i < items_.size(); ++i) {
      const int x = items_[i].rect.x();
      canvas.drawRect(SkRect::MakeLTRB(x, Scaled(11), x + 1,
                                       size_.height() - Scaled(11)),
                      line);
    }

    SkFontMetrics metrics;
    font_.getMetrics(&metrics);
    const float baseline =
        (size_.height() - (metrics.fAscent + metrics.fDescent)) / 2;
    SkPaint text;
    text.setAntiAlias(true);
    text.setColor(kText);
    for (const Item& item : items_) {
      canvas.drawString(item.label, item.rect.x() + kItemPadding * scale_,
                        baseline, font_, text);
    }

    bitmap.setImmutable();
    bitmap_ = bitmap;
    image_ = gfx::ImageSkia::CreateFrom1xBitmap(bitmap_);
  }

  void SetPressed(int index) {
    if (index == pressed_)
      return;
    pressed_ = index;
    Render();
    if (window_)
      window_->SchedulePaintInRect(gfx::Rect(size_));
  }

  void Press(const gfx::Point& p) { SetPressed(ItemAt(p)); }

  // A finger that slides off the item it went down on is a change of mind.
  void Move(const gfx::Point& p) {
    if (pressed_ >= 0 && ItemAt(p) != pressed_)
      SetPressed(-1);
  }

  void Release(const gfx::Point& p) {
    const int index = pressed_;
    SetPressed(-1);
    if (index >= 0 && ItemAt(p) == index)
      chosen_.Run(items_[index].command);
  }

  // ui::EventHandler:
  void OnTouchEvent(ui::TouchEvent* event) override {
    switch (event->type()) {
      case ui::ET_TOUCH_PRESSED:
        Press(event->location());
        break;
      case ui::ET_TOUCH_MOVED:
        Move(event->location());
        break;
      case ui::ET_TOUCH_RELEASED:
        Release(event->location());
        break;
      case ui::ET_TOUCH_CANCELLED:
        SetPressed(-1);
        break;
      default:
        break;
    }
    // Taken here in full; no gestures are needed on top of it.
    event->StopPropagation();
  }

  void OnGestureEvent(ui::GestureEvent* event) override {
    event->StopPropagation();
  }

  void OnMouseEvent(ui::MouseEvent* event) override {
    switch (event->type()) {
      case ui::ET_MOUSE_PRESSED:
        Press(event->location());
        break;
      case ui::ET_MOUSE_DRAGGED:
        Move(event->location());
        break;
      case ui::ET_MOUSE_RELEASED:
        Release(event->location());
        break;
      default:
        break;
    }
    event->StopPropagation();
  }

  // aura::WindowDelegate:
  gfx::Size GetMinimumSize() const override { return size_; }
  gfx::Size GetMaximumSize() const override { return size_; }
  void OnBoundsChanged(const gfx::Rect& old_bounds,
                       const gfx::Rect& new_bounds) override {}
  gfx::NativeCursor GetCursor(const gfx::Point& point) override {
    return gfx::NativeCursor();
  }
  int GetNonClientComponent(const gfx::Point& point) const override {
    return HTCLIENT;
  }
  bool ShouldDescendIntoChildForEventHandling(
      aura::Window* child,
      const gfx::Point& location) override {
    return false;
  }
  // Must not take focus: that would blur the text field being edited and
  // close the keyboard.
  bool CanFocus() override { return false; }
  void OnCaptureLost() override { SetPressed(-1); }
  void OnPaint(const ui::PaintContext& context) override {
    ui::PaintRecorder recorder(context, size_);
    recorder.canvas()->DrawImageInt(image_, 0, 0);
  }
  void OnDeviceScaleFactorChanged(float old_device_scale_factor,
                                  float new_device_scale_factor) override {}
  void OnWindowDestroying(aura::Window* window) override {}
  void OnWindowDestroyed(aura::Window* window) override {}
  void OnWindowTargetVisibilityChanged(bool visible) override {}
  bool HasHitTestMask() const override { return false; }
  void GetHitTestMask(SkPath* mask) const override {}

  const float scale_;
  ChosenCallback chosen_;
  SkFont font_;
  std::vector<Item> items_;
  gfx::Size size_;
  int pressed_ = -1;
  SkBitmap bitmap_;
  gfx::ImageSkia image_;
  std::unique_ptr<aura::Window> window_;
};

BerryEditMenuRunner::BerryEditMenuRunner() = default;

BerryEditMenuRunner::~BerryEditMenuRunner() = default;

bool BerryEditMenuRunner::IsMenuAvailable(
    const ui::TouchSelectionMenuClient* client) const {
  if (!client)
    return false;
  // Paste is asked about last: answering it means reading the clipboard.
  return client->IsCommandIdEnabled(ui::TouchEditable::kCopy) ||
         client->IsCommandIdEnabled(ui::TouchEditable::kCut) ||
         client->IsCommandIdEnabled(ui::TouchEditable::kSelectAll) ||
         client->IsCommandIdEnabled(ui::TouchEditable::kPaste);
}

void BerryEditMenuRunner::OpenMenu(
    base::WeakPtr<ui::TouchSelectionMenuClient> client,
    const gfx::Rect& anchor_rect,
    const gfx::Size& handle_image_size,
    aura::Window* context) {
  CloseMenu();
  if (!client || !context || !context->GetRootWindow())
    return;
  aura::Window* root = context->GetRootWindow();

  std::vector<Command> commands;
  for (const Command& c : kCommands) {
    if (client->IsCommandIdEnabled(c.id))
      commands.push_back(c);
  }
  if (commands.empty())
    return;

  const gfx::Rect area(root->bounds().size());
  const float scale = std::clamp(
      static_cast<float>(area.width()) / kReferenceWidth, 1.0f, 3.0f);
  auto menu = std::make_unique<Menu>(
      commands, scale,
      base::BindRepeating(
          [](base::WeakPtr<BerryEditMenuRunner> runner, int command) {
            // Not from inside the touch that chose it: running the command
            // closes the menu, and that destroys the window the touch is
            // still being delivered to.
            base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                FROM_HERE, base::BindOnce(&BerryEditMenuRunner::Execute,
                                          runner, command));
          },
          weak_factory_.GetWeakPtr()));

  // |anchor_rect| is the selection, in screen coordinates. Without a screen
  // position client (content_shell has none) those are the coordinates of
  // the window showing the page.
  gfx::Rect anchor = anchor_rect;
  gfx::Point origin = anchor.origin();
  if (auto* screen = aura::client::GetScreenPositionClient(root))
    screen->ConvertPointFromScreen(root, &origin);
  else
    aura::Window::ConvertPointToTarget(context, root, &origin);
  anchor.set_origin(origin);

  const gfx::Size size = menu->size();
  const int gap = static_cast<int>(kAnchorGap * scale + 0.5f);
  const int margin = static_cast<int>(kEdgeMargin * scale + 0.5f);
  const int top = area.y() + std::max(top_inset_, margin);
  const int bottom = area.bottom() - margin;

  // Above the selection if it fits; otherwise below it, clear of the handles
  // that hang under the text; otherwise (the selection fills the window) at
  // the top of what is visible.
  int y = anchor.y() - gap - size.height();
  if (y < top) {
    y = anchor.bottom() + handle_image_size.height() + gap;
    if (y + size.height() > bottom)
      y = top;
  }
  int x = anchor.CenterPoint().x() - size.width() / 2;
  x = std::clamp(x, area.x() + margin,
                 std::max(area.x() + margin,
                          area.right() - margin - size.width()));
  const gfx::Rect bounds(x, y, size.width(), size.height());

  if (base::QnxNavLogEnabled()) {
    // Geometry only, for diagnosis and for the bench test to know where to
    // tap; never the selected text.
    std::string line = "BerryEdit: open";
    char buf[96];
    snprintf(buf, sizeof(buf), " bounds=%d,%d,%d,%d anchor=%d,%d,%d,%d items=",
             bounds.x(), bounds.y(), bounds.width(), bounds.height(),
             anchor.x(), anchor.y(), anchor.width(), anchor.height());
    line += buf;
    for (const Menu::Item& item : menu->items()) {
      const gfx::Point c = item.rect.CenterPoint();
      snprintf(buf, sizeof(buf), "%d@%d,%d;", item.command, bounds.x() + c.x(),
               bounds.y() + c.y());
      line += buf;
    }
    QNX_NAV_LOG_FMT("%s\n", line.c_str());
  }
  if (const char* dump = getenv("BERRY_EDIT_MENU_DUMP"))
    menu->DumpPng(dump);

  client_ = std::move(client);
  menu_ = std::move(menu);
  menu_->Show(root, bounds);
}

void BerryEditMenuRunner::CloseMenu() {
  if (!menu_)
    return;
  QNX_NAV_LOG("BerryEdit: close\n");
  menu_.reset();
  client_.reset();
}

bool BerryEditMenuRunner::IsRunning() const {
  return menu_ != nullptr;
}

void BerryEditMenuRunner::Execute(int command) {
  // The menu may have been closed, or replaced by one for another selection,
  // between the tap and now.
  if (!menu_ || !client_)
    return;
  base::WeakPtr<ui::TouchSelectionMenuClient> client = client_;
  CloseMenu();
  QNX_NAV_LOG_FMT("BerryEdit: command %d\n", command);
  if (client && client->IsCommandIdEnabled(command))
    client->ExecuteCommand(command, 0);
}

}  // namespace content
