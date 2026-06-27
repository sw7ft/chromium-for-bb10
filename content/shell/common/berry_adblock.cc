// Copyright 2026 SW7FT. All rights reserved.

#include "content/shell/common/berry_adblock.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <string>
#include <string_view>
#include <vector>

#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "url/gurl.h"

namespace content {
namespace {

// Curated, high-traffic ad / tracker / analytics domains (suffix match). A
// compact hand-picked core distilled from EasyList/EasyPrivacy -- enough to cut
// the bulk of third-party JS on typical sites without paying to parse full
// filter lists on the Krait every launch. Deliberately EXCLUDES functional and
// auth-critical hosts: Google sign-in / APIs (accounts.google.com,
// apis.google.com, *.googleapis.com, *.gstatic.com), social-login SDKs, and
// generic CDNs. Blocking is host-suffix based, so "doubleclick.net" also covers
// "stats.g.doubleclick.net", etc.
constexpr std::string_view kBlockedDomains[] = {
    "doubleclick.net",       "googlesyndication.com", "googleadservices.com",
    "google-analytics.com",  "googletagmanager.com",  "googletagservices.com",
    "adservice.google.com",  "2mdn.net",              "app-measurement.com",
    "scorecardresearch.com", "adnxs.com",             "adsrvr.org",
    "amazon-adsystem.com",   "criteo.com",            "criteo.net",
    "taboola.com",           "outbrain.com",          "pubmatic.com",
    "rubiconproject.com",    "openx.net",             "casalemedia.com",
    "moatads.com",           "adform.net",            "bidswitch.net",
    "sharethrough.com",      "smartadserver.com",     "33across.com",
    "quantserve.com",        "quantcount.com",        "chartbeat.com",
    "chartbeat.net",         "nr-data.net",           "newrelic.com",
    "segment.com",           "segment.io",            "mixpanel.com",
    "hotjar.com",            "fullstory.com",         "demdex.net",
    "omtrdc.net",            "2o7.net",               "everesttech.net",
    "bluekai.com",           "krxd.net",              "rlcdn.com",
    "crwdcntrl.net",         "serving-sys.com",       "yieldmo.com",
    "teads.tv",              "indexww.com",           "contextweb.com",
    "gumgum.com",            "3lift.com",             "adsafeprotected.com",
    "doubleverify.com",      "branch.io",             "appsflyer.com",
    "adjust.com",            "ad-delivery.net",
};

bool HostIsBlocked(std::string_view host) {
  for (std::string_view dom : kBlockedDomains) {
    if (host == dom)
      return true;
    // Suffix match on a label boundary: host ends with "." + dom.
    if (host.size() > dom.size() &&
        host[host.size() - dom.size() - 1] == '.' &&
        host.substr(host.size() - dom.size()) == dom) {
      return true;
    }
  }
  return false;
}

class BerryAdblockThrottle : public blink::URLLoaderThrottle {
 public:
  BerryAdblockThrottle() = default;
  ~BerryAdblockThrottle() override = default;

  BerryAdblockThrottle(const BerryAdblockThrottle&) = delete;
  BerryAdblockThrottle& operator=(const BerryAdblockThrottle&) = delete;

  void WillStartRequest(network::ResourceRequest* request,
                        bool* /*defer*/) override {
    MaybeBlock(request->url);
  }

  void WillRedirectRequest(
      net::RedirectInfo* redirect_info,
      const network::mojom::URLResponseHead& /*response_head*/,
      bool* /*defer*/,
      std::vector<std::string>* /*to_be_removed_request_headers*/,
      net::HttpRequestHeaders* /*modified_request_headers*/,
      net::HttpRequestHeaders* /*modified_cors_exempt_request_headers*/)
      override {
    MaybeBlock(redirect_info->new_url);
  }

 private:
  void MaybeBlock(const GURL& url) {
    if (!delegate_ || !BerryAdblockShouldBlock(url))
      return;
    static const bool log = getenv("BERRY_ADBLOCK_LOG") != nullptr;
    if (log) {
      const std::string& spec = url.possibly_invalid_spec();
      fprintf(stderr, "BerryAdblock: blocked %.*s\n",
              static_cast<int>(spec.size()), spec.data());
    }
    delegate_->CancelWithError(net::ERR_BLOCKED_BY_CLIENT, "BerryAdblock");
  }
};

}  // namespace

bool BerryAdblockEnabled() {
  static const bool enabled = [] {
    if (const char* e = getenv("BERRY_ADBLOCK"); e && e[0] == '0')
      return false;
    struct stat st;
    if (::stat("/accounts/1000/shared/misc/berry-adblock.disable", &st) == 0)
      return false;
    return true;
  }();
  return enabled;
}

bool BerryAdblockShouldBlock(const GURL& url) {
  if (!url.SchemeIsHTTPOrHTTPS())
    return false;
  return HostIsBlocked(url.host_piece());
}

std::unique_ptr<blink::URLLoaderThrottle> MaybeCreateBerryAdblockThrottle() {
  if (!BerryAdblockEnabled())
    return nullptr;
  return std::make_unique<BerryAdblockThrottle>();
}

}  // namespace content
