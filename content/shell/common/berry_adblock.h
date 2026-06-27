// Copyright 2026 SW7FT. All rights reserved.
// Lightweight network-level ad/tracker blocking for the BB10/QNX browser.
//
// Heavy general-browsing pages spend most of their JS budget on third-party
// ads/analytics/tags. Cancelling those requests before they load means the
// Krait never downloads, parses, or executes that code -- the cheapest possible
// "make heavy JS sites faster" lever, with no allocator/scheduler changes.

#ifndef CONTENT_SHELL_COMMON_BERRY_ADBLOCK_H_
#define CONTENT_SHELL_COMMON_BERRY_ADBLOCK_H_

#include <memory>

#include "third_party/blink/public/common/loader/url_loader_throttle.h"

class GURL;

namespace content {

// Resolved once per process. Blocking is ON by default; disable with the
// BERRY_ADBLOCK=0 env var or a berry-adblock.disable marker file (so it can be
// toggled on-device without a rebuild).
bool BerryAdblockEnabled();

// True if |url| targets a known ad/tracker/analytics host. Reads only immutable
// static data, so it is safe to call from any thread.
bool BerryAdblockShouldBlock(const GURL& url);

// Returns a throttle that cancels blocked requests, or null when blocking is
// disabled. The throttle is stateless; one is created per request.
std::unique_ptr<blink::URLLoaderThrottle> MaybeCreateBerryAdblockThrottle();

}  // namespace content

#endif  // CONTENT_SHELL_COMMON_BERRY_ADBLOCK_H_
