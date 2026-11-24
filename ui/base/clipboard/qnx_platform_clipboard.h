// Copyright 2026 SW7FT. All rights reserved.
// BlackBerry 10 system clipboard for ClipboardOzone.

#ifndef UI_BASE_CLIPBOARD_QNX_PLATFORM_CLIPBOARD_H_
#define UI_BASE_CLIPBOARD_QNX_PLATFORM_CLIPBOARD_H_

#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "ui/ozone/public/platform_clipboard.h"

namespace ui {

// Shares plain text with the rest of the phone through libclipboard, the
// clipboard every BB10 app uses, so text copied in a page can be pasted into
// another app and the other way round.
//
// Only plain text crosses that boundary. Everything a page copies (HTML,
// images, custom types) stays available inside the browser for as long as the
// browser's copy is still the newest thing on the system clipboard:
// ClipboardOzone keeps the full set and serves it while IsSelectionOwner() is
// true. BB10 does not notify apps about clipboard changes, so ownership is
// decided by looking: once the system text is no longer what was left there
// by our last copy, somebody else has copied and their text wins.
//
// When libclipboard or its directory is unavailable (the bench engines, which
// do not run as an installed app) this degrades to a clipboard private to the
// process, which is what content_shell had before.
class QnxPlatformClipboard : public PlatformClipboard {
 public:
  QnxPlatformClipboard();
  QnxPlatformClipboard(const QnxPlatformClipboard&) = delete;
  QnxPlatformClipboard& operator=(const QnxPlatformClipboard&) = delete;
  ~QnxPlatformClipboard() override;

  // PlatformClipboard:
  void OfferClipboardData(ClipboardBuffer buffer,
                          const DataMap& data_map,
                          OfferDataClosure callback) override;
  void RequestClipboardData(ClipboardBuffer buffer,
                            const std::string& mime_type,
                            RequestDataClosure callback) override;
  void GetAvailableMimeTypes(ClipboardBuffer buffer,
                             GetMimeTypesClosure callback) override;
  bool IsSelectionOwner(ClipboardBuffer buffer) override;
  void SetClipboardDataChangedCallback(
      ClipboardDataChangedCallback callback) override;
  bool IsSelectionBufferAvailable() const override;

 private:
  struct Api;

  // The system clipboard's text; nullopt when it holds none.
  absl::optional<std::string> ReadSystemText();
  // Tells ClipboardOzone the contents changed. Posted, never re-entrant.
  void NotifyChangedSoon();
  void NotifyChanged();

  // Null when the system clipboard cannot be used.
  const Api* api_ = nullptr;

  bool owner_ = false;
  // What the system clipboard held right after our last copy.
  absl::optional<std::string> text_after_offer_;
  // What it held the last time we looked, to notice another app's copy.
  absl::optional<std::string> text_last_seen_;

  ClipboardDataChangedCallback changed_callback_;
  bool notify_pending_ = false;
  base::WeakPtrFactory<QnxPlatformClipboard> weak_factory_{this};
};

}  // namespace ui

#endif  // UI_BASE_CLIPBOARD_QNX_PLATFORM_CLIPBOARD_H_
