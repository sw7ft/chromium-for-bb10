// Copyright 2026 SW7FT. All rights reserved.

#include "ui/base/clipboard/qnx_platform_clipboard.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/qnx_trace.h"
#include "base/task/sequenced_task_runner.h"
#include "ui/base/clipboard/clipboard_constants.h"

namespace ui {

namespace {

// libclipboard's own name for plain text (it stores one file per type).
const char kSystemTextType[] = "text/plain";

// A page can put megabytes on the clipboard. Other apps get at most this
// much of it; the full text stays available inside the browser.
constexpr size_t kMaxSystemTextBytes = 1024 * 1024;

// The names ClipboardOzone uses for UTF-8 text on this platform.
bool IsTextMimeType(const std::string& mime_type) {
  return mime_type == kMimeTypeText || mime_type == kMimeTypeTextUtf8;
}

// The UTF-8 text in |data_map|, if it has any.
bool FindText(const PlatformClipboard::DataMap& data_map, std::string* text) {
  for (const char* type : {kMimeTypeText, kMimeTypeTextUtf8}) {
    auto it = data_map.find(type);
    if (it == data_map.end() || !it->second)
      continue;
    text->assign(reinterpret_cast<const char*>(it->second->front()),
                 it->second->size());
    return true;
  }
  return false;
}

// Cuts |text| to at most |max| bytes without splitting a UTF-8 sequence.
void TruncateUtf8(std::string* text, size_t max) {
  if (text->size() <= max)
    return;
  size_t cut = max;
  while (cut > 0 && (static_cast<unsigned char>((*text)[cut]) & 0xC0) == 0x80)
    --cut;
  text->resize(cut);
}

}  // namespace

// <clipboard/clipboard.h>, resolved at run time: the library is part of every
// BB10 release, but an engine that will not start without it helps nobody.
struct QnxPlatformClipboard::Api {
  int (*set_clipboard_path)(const char*, unsigned int) = nullptr;
  int (*get_clipboard_path)(char*, unsigned int) = nullptr;
  int (*get_clipboard_can_write)() = nullptr;
  int (*empty_clipboard)() = nullptr;
  int (*set_clipboard_data)(const char*, unsigned int, const char*) = nullptr;
  int (*get_clipboard_data_no_time_update)(const char*, char**) = nullptr;
  unsigned int (*get_clipboard_data)(const char*, char**) = nullptr;

  // Returns the API bound to a usable clipboard directory, or null.
  static const Api* Load() {
    void* lib = dlopen("libclipboard.so.1", RTLD_NOW);
    if (!lib) {
      QNX_NAV_LOG("BerryClip: no libclipboard, clipboard is private\n");
      return nullptr;
    }
    static Api api;
#define BERRY_CLIP_SYM(name)                                       \
  api.name = reinterpret_cast<decltype(api.name)>(dlsym(lib, #name)); \
  if (!api.name)                                                   \
    return nullptr;
    BERRY_CLIP_SYM(set_clipboard_path)
    BERRY_CLIP_SYM(get_clipboard_path)
    BERRY_CLIP_SYM(get_clipboard_can_write)
    BERRY_CLIP_SYM(empty_clipboard)
    BERRY_CLIP_SYM(set_clipboard_data)
    BERRY_CLIP_SYM(get_clipboard_data_no_time_update)
    BERRY_CLIP_SYM(get_clipboard_data)
#undef BERRY_CLIP_SYM

    // Tests point the engine at a directory of their own so they never read
    // or replace what the user has copied. The directory must hold nothing
    // else: emptying the clipboard deletes every file in it.
    if (const char* dir = getenv("BERRY_CLIPBOARD_DIR")) {
      if (!dir[0] || api.set_clipboard_path(dir, strlen(dir)) != 0)
        return nullptr;
    }
    // The default directory is derived from the app's sandbox. Outside one
    // there is none, and so no system clipboard to share.
    char path[512] = "";
    if (api.get_clipboard_path(path, sizeof(path)) != 0 || !path[0]) {
      QNX_NAV_LOG("BerryClip: no clipboard directory, clipboard is private\n");
      return nullptr;
    }
    QNX_NAV_LOG_FMT("BerryClip: system clipboard at %s writable=%d\n", path,
                    api.get_clipboard_can_write() == 0 ? 1 : 0);
    return &api;
  }
};

QnxPlatformClipboard::QnxPlatformClipboard() : api_(Api::Load()) {}

QnxPlatformClipboard::~QnxPlatformClipboard() = default;

void QnxPlatformClipboard::OfferClipboardData(ClipboardBuffer buffer,
                                              const DataMap& data_map,
                                              OfferDataClosure callback) {
  if (buffer == ClipboardBuffer::kCopyPaste) {
    owner_ = true;
    if (api_) {
      // A copy replaces whatever was there, also when it has no text in it
      // (an image): leaving the previous text behind would have other apps
      // paste something the user copied earlier.
      api_->empty_clipboard();
      std::string text;
      if (FindText(data_map, &text) && !text.empty()) {
        TruncateUtf8(&text, kMaxSystemTextBytes);
        const int written =
            api_->set_clipboard_data(kSystemTextType, text.size(), text.data());
        QNX_NAV_LOG_FMT("BerryClip: copy %zu bytes -> %d\n", text.size(),
                        written);
      }
      // Read back rather than assume: if the write was refused, or cut short,
      // "what our copy left behind" is still exactly what is there now.
      text_after_offer_ = ReadSystemText();
      text_last_seen_ = text_after_offer_;
    }
    NotifyChangedSoon();
  }
  std::move(callback).Run();
}

void QnxPlatformClipboard::RequestClipboardData(ClipboardBuffer buffer,
                                                const std::string& mime_type,
                                                RequestDataClosure callback) {
  // Only reached when another app's copy is the newest one, and plain text is
  // all that is taken from other apps.
  Data data;
  if (buffer == ClipboardBuffer::kCopyPaste && api_ &&
      IsTextMimeType(mime_type)) {
    char* raw = nullptr;
    // This is a real paste, so let the system record the clipboard as read.
    const int size =
        static_cast<int>(api_->get_clipboard_data(kSystemTextType, &raw));
    if (size > 0 && raw) {
      size_t len = static_cast<size_t>(size);
      while (len > 0 && raw[len - 1] == '\0')
        --len;
      std::vector<uint8_t> bytes(raw, raw + len);
      data = base::RefCountedBytes::TakeVector(&bytes);
    }
    free(raw);
  }
  std::move(callback).Run(data);
}

void QnxPlatformClipboard::GetAvailableMimeTypes(ClipboardBuffer buffer,
                                                 GetMimeTypesClosure callback) {
  std::vector<std::string> types;
  if (buffer == ClipboardBuffer::kCopyPaste && api_) {
    const absl::optional<std::string> text = ReadSystemText();
    if (text && !text->empty()) {
      types.push_back(kMimeTypeText);
      types.push_back(kMimeTypeTextUtf8);
    }
  }
  std::move(callback).Run(types);
}

bool QnxPlatformClipboard::IsSelectionOwner(ClipboardBuffer buffer) {
  if (buffer != ClipboardBuffer::kCopyPaste || !owner_)
    return false;
  if (!api_)
    return true;
  if (ReadSystemText() == text_after_offer_)
    return true;
  // Another app has copied since. Its text is the clipboard now, and the
  // richer data from our own copy must not come back if it is later cleared.
  owner_ = false;
  return false;
}

void QnxPlatformClipboard::SetClipboardDataChangedCallback(
    ClipboardDataChangedCallback callback) {
  changed_callback_ = std::move(callback);
}

bool QnxPlatformClipboard::IsSelectionBufferAvailable() const {
  return false;
}

absl::optional<std::string> QnxPlatformClipboard::ReadSystemText() {
  absl::optional<std::string> text;
  char* raw = nullptr;
  // Looking is not pasting: leave the clipboard's "last read" time alone.
  const int size = api_->get_clipboard_data_no_time_update(kSystemTextType, &raw);
  if (size >= 0 && raw) {
    size_t len = static_cast<size_t>(size);
    while (len > 0 && raw[len - 1] == '\0')
      --len;
    text.emplace(raw, len);
  }
  free(raw);
  if (text != text_last_seen_) {
    text_last_seen_ = text;
    NotifyChangedSoon();
  }
  return text;
}

void QnxPlatformClipboard::NotifyChangedSoon() {
  if (notify_pending_ || !base::SequencedTaskRunner::HasCurrentDefault())
    return;
  notify_pending_ = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&QnxPlatformClipboard::NotifyChanged,
                                weak_factory_.GetWeakPtr()));
}

void QnxPlatformClipboard::NotifyChanged() {
  notify_pending_ = false;
  if (changed_callback_)
    changed_callback_.Run(ClipboardBuffer::kCopyPaste);
}

}  // namespace ui
