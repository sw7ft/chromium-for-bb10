// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/shell/browser/berry_geolocation_qnx.h"

#if BUILDFLAG(IS_QNX)

#include "base/functional/bind.h"
#include "base/qnx_trace.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/device_service.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/device/public/cpp/geolocation/geoposition.h"
#include "services/device/public/mojom/geolocation_control.mojom.h"
#include "services/device/public/mojom/geoposition.mojom.h"

namespace content {

namespace {

device::mojom::GeopositionResultPtr BerryMakeVancouverPosition() {
  auto position = device::mojom::Geoposition::New();
  position->latitude = kBerryDefaultLatitude;
  position->longitude = kBerryDefaultLongitude;
  position->altitude = 0.0;
  position->accuracy = 100.0;
  position->altitude_accuracy = 0.0;
  position->heading = 0.0;
  position->speed = 0.0;
  position->timestamp = base::Time::Now();
  return device::mojom::GeopositionResult::NewPosition(std::move(position));
}

void BerryInjectFixedGeolocationOnContext(WebContents* web_contents) {
  if (!web_contents)
    return;
  auto* context =
      static_cast<WebContentsImpl*>(web_contents)->GetGeolocationContext();
  if (!context)
    return;
  auto result = BerryMakeVancouverPosition();
  if (!result->is_position() ||
      !device::ValidateGeoposition(*result->get_position())) {
    return;
  }
  context->SetOverride(std::move(result));
  QNX_NAV_LOG_FMT(
      "BerryNav: GeoContextOverride lat=%.5f lon=%.5f\n",
      kBerryDefaultLatitude, kBerryDefaultLongitude);
}

void BerryOptIntoGeolocationServicesOnce() {
  static bool opted_in = false;
  if (opted_in)
    return;
  opted_in = true;

  mojo::Remote<device::mojom::GeolocationControl> geolocation_control;
  GetDeviceService().BindGeolocationControl(
      geolocation_control.BindNewPipeAndPassReceiver());
  geolocation_control->UserDidOptIntoLocationServices();
  QNX_NAV_LOG_FMT("%s", "BerryNav: GeoOptIn UserDidOptIntoLocationServices\n");
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

void BerryPreseedGeolocationContext(WebContents* web_contents) {
  BerryOptIntoGeolocationServices();
  BerryInjectFixedGeolocationOnContext(web_contents);
}

void BerryApplyFixedGeolocationOverride(WebContents* web_contents) {
  BerryPreseedGeolocationContext(web_contents);
}

void BerryScheduleMapsGeolocationRetries(WebContents* web_contents) {
  static constexpr int kRetryDelaysMs[] = {50,  100,  250,  500,
                                           1000, 2000, 5000};
  for (int delay_ms : kRetryDelaysMs) {
    GetUIThreadTaskRunner({})->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<WebContents> wc) {
              if (!wc)
                return;
              BerryOptIntoGeolocationServicesOnce();
              BerryInjectFixedGeolocationOnContext(wc.get());
            },
            web_contents->GetWeakPtr()),
        base::Milliseconds(delay_ms));
  }
}

}  // namespace content

#endif  // BUILDFLAG(IS_QNX)
