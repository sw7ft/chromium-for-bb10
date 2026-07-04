// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/shell/browser/shell_fixed_location_provider.h"

#include "base/time/time.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_QNX)
#include "base/qnx_trace.h"
#include "content/shell/browser/berry_geolocation_qnx.h"
#endif
#include "services/device/public/cpp/geolocation/geoposition.h"
#include "services/device/public/mojom/geoposition.mojom.h"

namespace content {
namespace {

constexpr double kDefaultLatitude = content::kBerryDefaultLatitude;
constexpr double kDefaultLongitude = content::kBerryDefaultLongitude;

class ShellFixedLocationProvider : public device::LocationProvider {
 public:
  ShellFixedLocationProvider() = default;
  ~ShellFixedLocationProvider() override = default;

  void FillDiagnostics(
      device::mojom::GeolocationDiagnostics& diagnostics) override {
    diagnostics.provider_state =
        started_
            ? device::mojom::GeolocationDiagnostics::ProviderState::
                  kHighAccuracy
            : device::mojom::GeolocationDiagnostics::ProviderState::kStopped;
  }

  void SetUpdateCallback(
      const LocationProviderUpdateCallback& callback) override {
    callback_ = callback;
    MaybeEmitPosition();
  }

  void StartProvider(bool high_accuracy) override {
    started_ = true;
    // content_shell grants geolocation immediately; emit without waiting for
    // InformProvidersPermissionGranted (can race Maps' first getCurrentPosition).
    permission_granted_ = true;
    MaybeEmitPosition();
  }

  void StopProvider() override { started_ = false; }

  const device::mojom::GeopositionResult* GetPosition() override {
    return result_.get();
  }

  void OnPermissionGranted() override {
    permission_granted_ = true;
    MaybeEmitPosition();
  }

 private:
  void MaybeEmitPosition() {
    if (!started_ || !permission_granted_ || callback_.is_null())
      return;

    auto position = device::mojom::Geoposition::New();
    position->latitude = kDefaultLatitude;
    position->longitude = kDefaultLongitude;
    position->altitude = 0.0;
    position->accuracy = 100.0;
    position->altitude_accuracy = 0.0;
    position->heading = 0.0;
    position->speed = 0.0;
    position->timestamp = base::Time::Now();
    result_ = device::mojom::GeopositionResult::NewPosition(
        std::move(position));
#if BUILDFLAG(IS_QNX)
    QNX_TRACE_MSG("QNX:Geo:FixedPosition lat=49.26 lon=-123.12\n");
#endif
    callback_.Run(this, result_.Clone());
  }

  LocationProviderUpdateCallback callback_;
  device::mojom::GeopositionResultPtr result_;
  bool started_ = false;
  // content_shell auto-grants geolocation; the device service opt-in path can
  // race StartProvider, so do not block synthetic fixes on OnPermissionGranted.
  bool permission_granted_ = true;
};

}  // namespace

std::unique_ptr<device::LocationProvider> CreateShellFixedLocationProvider() {
  return std::make_unique<ShellFixedLocationProvider>();
}

}  // namespace content
