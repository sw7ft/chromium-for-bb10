// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/shell/browser/berry_geolocation_qnx.h"

#if BUILDFLAG(IS_QNX)

#include "base/functional/bind.h"
#include "base/qnx_trace.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_runner.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/device_service.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/device/public/mojom/geolocation_control.mojom.h"

namespace content {

namespace {

void BerryOptIntoGeolocationServicesOnce() {
  static bool opted_in = false;
  if (opted_in)
    return;
  opted_in = true;

  mojo::Remote<device::mojom::GeolocationControl> geolocation_control;
  GetDeviceService().BindGeolocationControl(
      geolocation_control.BindNewPipeAndPassReceiver());
  geolocation_control->UserDidOptIntoLocationServices();
  QNX_TRACE_MSG("QNX:Geo:UserDidOptIntoLocationServices\n");
}

}  // namespace

void BerryOptIntoGeolocationServices() {
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE, base::BindOnce(&BerryOptIntoGeolocationServicesOnce));
    return;
  }
  BerryOptIntoGeolocationServicesOnce();
}

bool BerryIsGoogleMapsUrl(const GURL& url) {
  if (!url.SchemeIsHTTPOrHTTPS())
    return false;
  const std::string host = url.host();
  if (host == "maps.google.com")
    return true;
  if ((host == "google.com" || host == "www.google.com") &&
      base::StartsWith(url.path(), "/maps", base::CompareCase::SENSITIVE)) {
    return true;
  }
  return false;
}

void BerryApplyFixedGeolocationOverride(WebContents* web_contents) {
  // Fixed position is supplied by ShellFixedLocationProvider +
  // UserDidOptIntoLocationServices. Do not use GeolocationContext::SetOverride:
  // it can deliver PositionUnavailable to an in-flight getCurrentPosition
  // before the override, which leaves Maps Lite stuck loading forever.
  BerryOptIntoGeolocationServices();
}

void BerryScheduleMapsGeolocationRetries(WebContents* web_contents) {
  for (int delay_ms : {500, 2000, 5000}) {
    GetUIThreadTaskRunner({})->PostDelayedTask(
        FROM_HERE, base::BindOnce(&BerryOptIntoGeolocationServicesOnce),
        base::Milliseconds(delay_ms));
  }
}

}  // namespace content

#endif  // BUILDFLAG(IS_QNX)
