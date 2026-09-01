// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_SHELL_BROWSER_BERRY_MAPS_QNX_H_
#define CONTENT_SHELL_BROWSER_BERRY_MAPS_QNX_H_

#include "build/build_config.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_QNX)

#include <optional>

namespace content {

class Shell;

// Returns a corrected Maps URL when the camera segment contains lat=-90 or NaN.
std::optional<GURL> BerryFixMapsCameraUrl(const GURL& url);

// Redirect away from a broken Maps camera if needed.
void BerryMaybeRedirectBrokenMapsCamera(Shell* shell,
                                        const GURL& url,
                                        bool same_document);

}  // namespace content

#endif  // BUILDFLAG(IS_QNX)

#endif  // CONTENT_SHELL_BROWSER_BERRY_MAPS_QNX_H_
