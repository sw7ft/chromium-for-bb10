// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_SHELL_BROWSER_BERRY_GEOLOCATION_QNX_H_
#define CONTENT_SHELL_BROWSER_BERRY_GEOLOCATION_QNX_H_

#include "build/build_config.h"
#include "url/gurl.h"

namespace content {

class WebContents;

#if BUILDFLAG(IS_QNX)

// Vancouver default — matches typical device locale and prior Maps log coords.
constexpr double kBerryDefaultLatitude = 49.2577355;
constexpr double kBerryDefaultLongitude = -123.123904;

void BerryOptIntoGeolocationServices();
bool BerryIsGoogleMapsUrl(const GURL& url);
// Pre-seed GeolocationContext override before any page binds geolocation.
void BerryPreseedGeolocationContext(WebContents* web_contents);
void BerryApplyFixedGeolocationOverride(WebContents* web_contents);
void BerryScheduleMapsGeolocationRetries(WebContents* web_contents);

#endif  // BUILDFLAG(IS_QNX)

}  // namespace content

#endif  // CONTENT_SHELL_BROWSER_BERRY_GEOLOCATION_QNX_H_
