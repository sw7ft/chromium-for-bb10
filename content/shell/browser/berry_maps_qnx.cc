// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/shell/browser/berry_maps_qnx.h"

#if BUILDFLAG(IS_QNX)

#include "base/functional/callback_helpers.h"
#include "base/qnx_trace.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/shell/browser/berry_geolocation_qnx.h"
#include "content/shell/browser/shell.h"
#include "url/gurl.h"

namespace content {

namespace {

bool BerryMapsCameraLooksBroken(const std::string& spec) {
  if (spec.find("@-90,") != std::string::npos ||
      spec.find(",NaN") != std::string::npos ||
      spec.find("NaNz") != std::string::npos ||
      spec.find("NaNm") != std::string::npos)
    return true;
  // Maps sometimes flips latitude sign (e.g. @-39 when Vancouver is +49).
  const size_t at = spec.find("/@");
  if (at != std::string::npos && at + 3 < spec.size() && spec[at + 2] == '-')
    return true;
  return false;
}

bool BerryParseMapsPlaceCoords(const std::string& spec,
                               double* out_lat,
                               double* out_lng) {
  const size_t lat_pos = spec.find("!3d");
  const size_t lng_pos = spec.find("!4d");
  if (lat_pos == std::string::npos || lng_pos == std::string::npos)
    return false;
  const std::string lat_str = spec.substr(lat_pos + 3);
  const std::string lng_str = spec.substr(lng_pos + 3);
  double lat = 0.0;
  double lng = 0.0;
  if (!base::StringToDouble(lat_str, &lat) ||
      !base::StringToDouble(lng_str, &lng)) {
    return false;
  }
  if (lat < -90.0 || lat > 90.0 || lng < -180.0 || lng > 180.0)
    return false;
  *out_lat = lat;
  *out_lng = lng;
  return true;
}

size_t BerryFindMapsCameraAt(const std::string& spec) {
  const size_t slash_at = spec.find("/@");
  if (slash_at != std::string::npos)
    return slash_at + 1;
  const size_t bare_at = spec.find("@");
  if (bare_at != std::string::npos &&
      spec.find("google.com/maps") != std::string::npos)
    return bare_at;
  return std::string::npos;
}

std::string BerryEscapeForSingleQuotedJs(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\\' || c == '\'')
      out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

void BerryReplaceMapsHistoryUrl(WebContents* web_contents, const GURL& fixed) {
  if (!web_contents)
    return;
  RenderFrameHost* rfh = web_contents->GetPrimaryMainFrame();
  if (!rfh || !rfh->IsRenderFrameLive())
    return;
  const std::string escaped = BerryEscapeForSingleQuotedJs(fixed.spec());
  const std::string js = "try{history.replaceState(history.state,'','" +
                         escaped + "');}catch(e){}";
  rfh->ExecuteJavaScriptForTests(base::UTF8ToUTF16(js), base::DoNothing());
  QNX_NAV_LOG_FMT("BerryNav: MapsCameraReplaceState \"%s\"\n",
                  fixed.spec().substr(0, 100).c_str());
}

}  // namespace

std::optional<GURL> BerryFixMapsCameraUrl(const GURL& url) {
  if (!BerryIsGoogleMapsUrl(url))
    return std::nullopt;
  const std::string spec = url.spec();
  if (!BerryMapsCameraLooksBroken(spec))
    return std::nullopt;

  double lat = kBerryDefaultLatitude;
  double lng = kBerryDefaultLongitude;
  double zoom = 12.0;
  double place_lat = 0.0;
  double place_lng = 0.0;
  if (BerryParseMapsPlaceCoords(spec, &place_lat, &place_lng)) {
    lat = place_lat;
    lng = place_lng;
    zoom = 14.0;
  }

  const size_t camera_at = BerryFindMapsCameraAt(spec);
  if (camera_at == std::string::npos)
    return std::nullopt;

  size_t camera_end = spec.find('/', camera_at + 1);
  if (camera_end == std::string::npos)
    camera_end = spec.size();
  const size_t query_in_camera = spec.find('?', camera_at + 1);
  if (query_in_camera != std::string::npos && query_in_camera < camera_end)
    camera_end = query_in_camera;

  std::string fixed = spec;
  const std::string replacement =
      base::StringPrintf("@%.7f,%.7f,%.2fz", lat, lng, zoom);
  fixed.replace(camera_at, camera_end - camera_at, replacement);

  GURL fixed_url(fixed);
  if (!fixed_url.is_valid() || fixed_url == url)
    return std::nullopt;
  QNX_NAV_LOG_FMT("BerryNav: MapsCameraFix \"%s\" -> \"%s\"\n",
                  url.spec().substr(0, 100).c_str(),
                  fixed_url.spec().substr(0, 100).c_str());
  return fixed_url;
}

void BerryMaybeRedirectBrokenMapsCamera(Shell* shell,
                                        const GURL& url,
                                        bool same_document) {
  if (!shell)
    return;
  std::optional<GURL> fixed = BerryFixMapsCameraUrl(url);
  if (!fixed.has_value())
    return;

  WebContents* web_contents = shell->web_contents();
  if (same_document) {
    // Avoid LoadURL loops: patch the history entry in-place.
    BerryReplaceMapsHistoryUrl(web_contents, fixed.value());
    return;
  }

  static base::TimeTicks last_full_redirect;
  const base::TimeTicks now = base::TimeTicks::Now();
  if (!last_full_redirect.is_null() &&
      now - last_full_redirect < base::Seconds(3)) {
    BerryReplaceMapsHistoryUrl(web_contents, fixed.value());
    return;
  }
  last_full_redirect = now;

  Shell* shell_ptr = shell;
  GURL target = fixed.value();
  GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE, base::BindOnce(
                     [](Shell* s, const GURL& u) {
                       if (s)
                         s->LoadURL(u);
                     },
                     shell_ptr, target));
}

}  // namespace content

#endif  // BUILDFLAG(IS_QNX)
