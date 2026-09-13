// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_RENDERER_HOST_QNX_MP_REPLAY_LOCK_H_
#define CONTENT_BROWSER_RENDERER_HOST_QNX_MP_REPLAY_LOCK_H_

#include "url/gurl.h"

namespace content {

// While a QNX HTTPS body-replay commit is in flight, drop renderer-initiated
// navigations to the same host so they cannot abort the commit (white screen).
void BerryQnxReplayLock(const GURL& url);
void BerryQnxReplayUnlock();
bool BerryQnxReplayShouldDrop(const GURL& url);

}  // namespace content

#endif  // CONTENT_BROWSER_RENDERER_HOST_QNX_MP_REPLAY_LOCK_H_
