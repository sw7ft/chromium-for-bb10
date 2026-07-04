// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_SHELL_BROWSER_SHELL_FIXED_LOCATION_PROVIDER_H_
#define CONTENT_SHELL_BROWSER_SHELL_FIXED_LOCATION_PROVIDER_H_

#include <memory>

#include "services/device/public/cpp/geolocation/location_provider.h"

namespace content {

// Returns a LocationProvider that reports a fixed position. QNX/BB10 has no
// native geolocation stack; without this, Maps Lite loads UI but never fetches
// raster tiles (location stays "disabled").
std::unique_ptr<device::LocationProvider> CreateShellFixedLocationProvider();

}  // namespace content

#endif  // CONTENT_SHELL_BROWSER_SHELL_FIXED_LOCATION_PROVIDER_H_
