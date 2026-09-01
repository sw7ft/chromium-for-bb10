// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/url_loader.h"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#if defined(__QNX__) || defined(__QNXNTO__)
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <atomic>
#include "watch_shim_script.h"
#include "search_shim_script.h"
#endif

#include "base/command_line.h"
#include "base/containers/fixed_flat_set.h"
#include "base/files/file.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/histogram_macros.h"
#include "base/ranges/algorithm.h"
#include "base/sequence_checker.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/task/thread_pool.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "base/trace_event/typed_macros.h"
#include "build/build_config.h"
#include "base/qnx_trace.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/base/elements_upload_data_stream.h"
#include "net/base/isolation_info.h"
#include "net/base/load_flags.h"
#include "net/base/load_timing_info.h"
#include "net/base/net_errors.h"
#include "net/base/mime_sniffer.h"
#include "net/base/schemeful_site.h"
#include "net/base/url_util.h"
#include "net/base/transport_info.h"
#include "net/base/upload_bytes_element_reader.h"
#include "net/base/upload_file_element_reader.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_inclusion_status.h"
#include "net/cookies/cookie_setting_override.h"
#include "net/cookies/cookie_store.h"
#include "net/cookies/cookie_util.h"
#include "net/cookies/site_for_cookies.h"
#include "net/cookies/static_cookie_policy.h"
#include "net/dns/public/secure_dns_policy.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_util.h"
#include "net/log/net_log_source_type.h"
#include "net/log/net_log_with_source.h"
#include "net/ssl/client_cert_store.h"
#include "net/ssl/ssl_connection_status_flags.h"
#include "net/ssl/ssl_private_key.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/referrer_policy.h"
#include "net/url_request/redirect_info.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_getter.h"
#include "services/network/ad_heuristic_cookie_overrides.h"
#include "services/network/attribution/attribution_request_helper.h"
#include "services/network/chunked_data_pipe_upload_data_stream.h"
#include "services/network/data_pipe_element_reader.h"
#include "services/network/network_service_memory_cache_writer.h"
#include "services/network/public/cpp/client_hints.h"
#include "services/network/public/cpp/constants.h"
#include "services/network/public/cpp/corb/orb_impl.h"
#include "services/network/public/cpp/cors/cors.h"
#include "services/network/public/cpp/cors/origin_access_list.h"
#include "services/network/public/cpp/cross_origin_resource_policy.h"
#include "services/network/public/cpp/empty_url_loader_client.h"
#include "services/network/public/cpp/features.h"
#include "services/network/public/cpp/header_util.h"
#include "services/network/public/cpp/ip_address_space_util.h"
#include "services/network/public/cpp/net_adapters.h"
#include "services/network/public/cpp/network_switches.h"
#include "services/network/public/cpp/parsed_headers.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/client_security_state.mojom-forward.h"
#include "services/network/public/mojom/cookie_access_observer.mojom-forward.h"
#include "services/network/public/mojom/cookie_access_observer.mojom.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "services/network/public/mojom/devtools_observer.mojom.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "services/network/public/mojom/http_raw_headers.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/resource_scheduler/resource_scheduler_client.h"
#include "services/network/sec_header_helpers.h"
#include "services/network/shared_dictionary/shared_dictionary_access_checker.h"
#include "services/network/shared_storage/shared_storage_request_helper.h"
#include "services/network/throttling/scoped_throttling_token.h"
#include "services/network/trust_tokens/trust_token_request_helper.h"
#include "services/network/url_loader_factory.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "url/origin.h"

namespace network {

namespace {

// Cannot use 0, because this means "default" in
// mojo::core::Core::CreateDataPipe
constexpr size_t kBlockedBodyAllocationSize = 1;

// A subclass of net::UploadBytesElementReader which owns
// ResourceRequestBody.
class BytesElementReader : public net::UploadBytesElementReader {
 public:
  BytesElementReader(ResourceRequestBody* resource_request_body,
                     const DataElementBytes& element)
      : net::UploadBytesElementReader(element.AsStringPiece().data(),
                                      element.AsStringPiece().size()),
        resource_request_body_(resource_request_body) {}

  BytesElementReader(const BytesElementReader&) = delete;
  BytesElementReader& operator=(const BytesElementReader&) = delete;

  ~BytesElementReader() override {}

 private:
  scoped_refptr<ResourceRequestBody> resource_request_body_;
};

// A subclass of net::UploadFileElementReader which owns
// ResourceRequestBody.
// This class is necessary to ensure the BlobData and any attached shareable
// files survive until upload completion.
class FileElementReader : public net::UploadFileElementReader {
 public:
  FileElementReader(ResourceRequestBody* resource_request_body,
                    base::TaskRunner* task_runner,
                    const DataElementFile& element,
                    base::File&& file)
      : net::UploadFileElementReader(task_runner,
                                     std::move(file),
                                     element.path(),
                                     element.offset(),
                                     element.length(),
                                     element.expected_modification_time()),
        resource_request_body_(resource_request_body) {}

  FileElementReader(const FileElementReader&) = delete;
  FileElementReader& operator=(const FileElementReader&) = delete;

  ~FileElementReader() override {}

 private:
  scoped_refptr<ResourceRequestBody> resource_request_body_;
};

#if defined(__QNX__) || defined(__QNXNTO__)
// True when a googlevideo videoplayback URL looks like SABR garbage (bad expire,
// absurd duration, or sabr listed in sparams).
bool BerryGooglevideoUrlLooksSabrOrGarbage(const std::string& spec) {
  if (spec.find("googlevideo.com") == std::string::npos ||
      spec.find("videoplayback") == std::string::npos)
    return false;
  if (spec.find("sabr=") != std::string::npos ||
      spec.find("%2Csabr%2C") != std::string::npos ||
      spec.find("sabr%2C") != std::string::npos ||
      spec.find("%2Csabr") != std::string::npos)
    return false;  // Scrub on GET; do not block signed URLs outright.
  size_t exp = spec.find("expire=");
  if (exp != std::string::npos) {
    char* end = nullptr;
    const long long val = strtoll(spec.c_str() + exp + 7, &end, 10);
    if (val > 2100000000LL || val < 1400000000LL)
      return true;
  }
  size_t dur = spec.find("dur=");
  if (dur != std::string::npos) {
    char* end = nullptr;
    const long long val = strtoll(spec.c_str() + dur + 4, &end, 10);
    if (val > 86400 * 24)
      return true;
  }
  if (spec.find("mn=") != std::string::npos && spec.find("sn-") == std::string::npos)
    return true;
  return false;
}

// Progressive googlevideo URLs extracted from a player response; <video> fetches
// matching these pass through untouched (no scrub/block/POST logic).
std::vector<std::string> g_berry_googlevideo_allowlist;
std::string g_berry_youtube_visitor_data;

const char kBerryVisitorDataPath[] =
    "/accounts/1000/shared/misc/berry-youtube-visitor.dat";

void BerryClearGooglevideoAllowlist() {
  g_berry_googlevideo_allowlist.clear();
}

void BerryLoadPersistedVisitorData() {
  static bool loaded = false;
  if (loaded)
    return;
  loaded = true;
  FILE* f = fopen(kBerryVisitorDataPath, "r");
  if (!f)
    return;
  char buf[1024];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (n == 0)
    return;
  buf[n] = '\0';
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    buf[--n] = '\0';
  if (n > 0) {
    g_berry_youtube_visitor_data.assign(buf, n);
    QNX_NAV_LOG_FMT("BerryNav: VisitorDataLoad bytes=%zu\n", n);
  }
}

void BerryPersistVisitorData() {
  if (g_berry_youtube_visitor_data.empty())
    return;
  FILE* f = fopen(kBerryVisitorDataPath, "w");
  if (!f)
    return;
  fwrite(g_berry_youtube_visitor_data.data(), 1,
         g_berry_youtube_visitor_data.size(), f);
  fclose(f);
}

std::string BerryExtractVisitorDataFromJson(const std::string& body) {
  static const char* kKeys[] = {"\"visitorData\":\"", "\"visitorData\": \""};
  for (const char* key : kKeys) {
    size_t pos = body.find(key);
    if (pos == std::string::npos)
      continue;
    pos += strlen(key);
    size_t end = body.find('"', pos);
    if (end != std::string::npos && end > pos)
      return body.substr(pos, end - pos);
  }
  return std::string();
}

void BerryTryCacheVisitorDataFromWatchHtml(const std::string& html) {
  if (html.empty())
    return;
  std::string visitor = BerryExtractVisitorDataFromJson(html);
  if (visitor.empty()) {
    static const char* kMarkers[] = {"\"VISITOR_DATA\":\"", "VISITOR_DATA\":\"",
                                     "\\\"VISITOR_DATA\\\":\\\""};
    for (const char* marker : kMarkers) {
      size_t pos = html.find(marker);
      if (pos == std::string::npos)
        continue;
      pos += strlen(marker);
      size_t end = pos;
      while (end < html.size() && html[end] != '"' && html[end] != '\\' &&
             html[end] != '\'')
        ++end;
      if (end > pos) {
        visitor = html.substr(pos, end - pos);
        break;
      }
    }
  }
  if (visitor.empty() || visitor == g_berry_youtube_visitor_data)
    return;
  g_berry_youtube_visitor_data = visitor;
  BerryPersistVisitorData();
  QNX_NAV_LOG_FMT("BerryNav: VisitorDataWatch bytes=%zu\n", visitor.size());
}

void BerryCacheVisitorDataFromPlayerResponse(const std::string& body) {
  const std::string visitor = BerryExtractVisitorDataFromJson(body);
  if (visitor.empty())
    return;
  if (visitor == g_berry_youtube_visitor_data)
    return;
  g_berry_youtube_visitor_data = visitor;
  BerryPersistVisitorData();
  QNX_NAV_LOG_FMT("BerryNav: VisitorDataCache bytes=%zu\n", visitor.size());
}

bool BerryJsonClientHasVisitorData(const std::string& body) {
  return !BerryExtractVisitorDataFromJson(body).empty();
}

bool BerryInjectVisitorDataIntoPlayerJson(std::string* body) {
  if (!body || body->empty() || g_berry_youtube_visitor_data.empty())
    return false;
  if (BerryJsonClientHasVisitorData(*body))
    return false;

  size_t client = body->find("\"client\":");
  if (client == std::string::npos)
    client = body->find("\"client\" :");
  if (client == std::string::npos)
    return false;
  size_t brace = body->find('{', client);
  if (brace == std::string::npos)
    return false;

  const std::string insert =
      "\"visitorData\":\"" + g_berry_youtube_visitor_data + "\",";
  body->insert(brace + 1, insert);
  return true;
}

bool BerryGooglevideoUrlIsAllowlisted(const std::string& spec) {
  for (const std::string& allowed : g_berry_googlevideo_allowlist) {
    if (spec == allowed)
      return true;
  }
  return false;
}

void BerryPruneExpiredAllowlist() {
  if (g_berry_googlevideo_allowlist.empty())
    return;
  const time_t now = time(nullptr);
  std::vector<std::string> kept;
  kept.reserve(g_berry_googlevideo_allowlist.size());
  for (const std::string& url : g_berry_googlevideo_allowlist) {
    const size_t exp = url.find("expire=");
    if (exp == std::string::npos) {
      kept.push_back(url);
      continue;
    }
    char* end = nullptr;
    const long long val = strtoll(url.c_str() + exp + 7, &end, 10);
    if (val > static_cast<long long>(now) + 60)
      kept.push_back(url);
  }
  if (kept.size() == g_berry_googlevideo_allowlist.size())
    return;
  QNX_NAV_LOG_FMT("BerryNav: AllowlistPrune before=%zu after=%zu\n",
                  g_berry_googlevideo_allowlist.size(), kept.size());
  g_berry_googlevideo_allowlist = std::move(kept);
}

void BerryRegisterGooglevideoAllowlist(const std::string& body) {
  BerryPruneExpiredAllowlist();
  const size_t streaming = body.find("\"streamingData\"");
  if (streaming == std::string::npos)
    return;
  const size_t formats_key = body.find("\"formats\"", streaming);
  if (formats_key == std::string::npos)
    return;
  const size_t arr_start = body.find('[', formats_key);
  if (arr_start == std::string::npos)
    return;
  const size_t arr_end = body.find(']', arr_start);
  if (arr_end == std::string::npos || arr_end <= arr_start)
    return;
  const std::string formats_section =
      body.substr(arr_start, arr_end - arr_start + 1);

  size_t pos = 0;
  while ((pos = formats_section.find("\"url\":\"", pos)) != std::string::npos) {
    pos += 7;
    size_t url_end = pos;
    while (url_end < formats_section.size() && formats_section[url_end] != '"')
      ++url_end;
    if (url_end <= pos)
      continue;
    std::string url = formats_section.substr(pos, url_end - pos);
    pos = url_end + 1;
    if (url.rfind("https://", 0) != 0 ||
        url.find("googlevideo.com") == std::string::npos)
      continue;
    if (BerryGooglevideoUrlIsAllowlisted(url))
      continue;
    g_berry_googlevideo_allowlist.push_back(url);
    QNX_NAV_LOG_FMT("BerryNav: WatchShim allowlist url=\"%.120s\"\n",
                    url.c_str());
  }
}

void BerryScrubGooglevideoUrl(std::string* spec) {
  if (!spec || spec->empty())
    return;
  base::ReplaceSubstringsAfterOffset(spec, 0, "c=WEB", "c=ANDROID");
  base::ReplaceSubstringsAfterOffset(spec, 0, "%2Csabr%2C", "%2C");
  base::ReplaceSubstringsAfterOffset(spec, 0, "sabr%2C", "");
  base::ReplaceSubstringsAfterOffset(spec, 0, "%2Csabr", "");
  base::ReplaceSubstringsAfterOffset(spec, 0, "sabr=1&", "");
  base::ReplaceSubstringsAfterOffset(spec, 0, "&sabr=1", "");
  base::ReplaceSubstringsAfterOffset(spec, 0, "?sabr=1&", "?");
  base::ReplaceSubstringsAfterOffset(spec, 0, "?sabr=1", "?");
  base::ReplaceSubstringsAfterOffset(spec, 0, "&keepalive=yes", "");
}

// Scrub googlevideo URLs embedded in youtubei player JSON (WEB+SABR → ANDROID).
bool BerryScrubGooglevideoUrlsInPlayerBody(std::string* body) {
  if (!body || body->empty())
    return false;
  bool changed = false;
  const char* needle = "https://";
  size_t pos = 0;
  while ((pos = body->find(needle, pos)) != std::string::npos) {
    size_t end = body->find('"', pos);
    if (end == std::string::npos)
      break;
    if (body->substr(pos, end - pos).find("googlevideo.com") ==
        std::string::npos) {
      pos = end + 1;
      continue;
    }
    std::string url = body->substr(pos, end - pos);
    BerryScrubGooglevideoUrl(&url);
    if (BerryGooglevideoUrlLooksSabrOrGarbage(url)) {
      body->erase(pos, end - pos);
      changed = true;
      continue;
    }
    if (url != body->substr(pos, end - pos)) {
      body->replace(pos, end - pos, url);
      changed = true;
    }
    pos += url.size();
  }
  return changed;
}

// Returns true when the request body should be dropped (legacy; unused for POST).
bool BerryPrepareYoutubeUrlAndMethod(
    GURL* url,
    std::string* method,
    const ResourceRequestBody* /*request_body*/) {
  if (!url || !method || !url->is_valid() || !url->SchemeIsHTTPOrHTTPS())
    return false;

  const std::string host = url->host();
  const bool is_googlevideo = host.find("googlevideo.com") != std::string::npos;
  if (!is_googlevideo)
    return false;

  std::string spec = url->spec();
  if (spec.find("videoplayback") == std::string::npos)
    return false;

  if (BerryGooglevideoUrlIsAllowlisted(spec)) {
    QNX_NAV_LOG_FMT("BerryNav: GooglevideoAllowlist pass url=\"%.100s\"\n",
                    spec.c_str());
    return false;
  }

  const std::string before = spec;
  const bool is_post = *method == "POST";

  if (BerryGooglevideoUrlLooksSabrOrGarbage(spec)) {
    if (!g_berry_googlevideo_allowlist.empty() &&
        !BerryGooglevideoUrlIsAllowlisted(spec)) {
      QNX_NAV_LOG_FMT(
          "BerryNav: GooglevideoNotAllowlisted %s url=\"%.100s\"\n",
          is_post ? "POST" : "GET", spec.c_str());
    }
    QNX_NAV_LOG_FMT(
        "BerryNav: BlockGarbageGooglevideo %s url=\"%.100s\"\n",
        is_post ? "POST" : "GET", spec.c_str());
    *url = GURL("https://www.youtube.com/generate_204");
    *method = "GET";
    return false;
  }

  if (is_post) {
    // Keep POST + body intact — POST→GET drops signed body bytes and yields 403.
    BerryScrubGooglevideoUrl(&spec);
    if (BerryGooglevideoUrlLooksSabrOrGarbage(spec)) {
      QNX_NAV_LOG_FMT(
          "BerryNav: BlockGarbageGooglevideo POST url=\"%.100s\"\n",
          spec.c_str());
      *url = GURL("https://www.youtube.com/generate_204");
      *method = "GET";
      return false;
    }
    if (spec != before) {
      *url = GURL(spec);
      QNX_NAV_LOG_FMT("BerryNav: GooglevideoPostScrub url=\"%.100s\"\n",
                      spec.c_str());
    } else {
      QNX_NAV_LOG_FMT("BerryNav: GooglevideoPostPass url=\"%.100s\"\n",
                      spec.c_str());
    }
    return false;
  }

  BerryScrubGooglevideoUrl(&spec);
  if (spec != before) {
    *url = GURL(spec);
    QNX_NAV_LOG_FMT("BerryNav: GooglevideoScrub url=\"%.100s\"\n",
                    spec.c_str());
  }
  return false;
}

// Referer / Origin fixes for YouTube. Embed flows need a third-party Referer
// (error 153); watch-page googlevideo needs youtube.com (reddit → HTTP 403).
void BerryMaybeFixYoutubeEmbedReferer(net::URLRequest* url_request) {
  if (!url_request)
    return;
  const GURL& url = url_request->url();
  if (!url.SchemeIsHTTPOrHTTPS())
    return;
  const std::string spec = url.spec();
  const std::string host = url.host();
  const bool is_yt_host =
      host.find("youtube.com") != std::string::npos ||
      host.find("youtube-nocookie.com") != std::string::npos;
  const bool is_embed_page = is_yt_host && spec.find("/embed/") != std::string::npos;
  const bool is_youtubei = is_yt_host && spec.find("/youtubei/") != std::string::npos;
  const bool is_googlevideo = host.find("googlevideo.com") != std::string::npos;
  if (!is_embed_page && !is_youtubei && !is_googlevideo)
    return;

  const std::string& ref = url_request->referrer();
  const bool ref_is_file = ref.rfind("file:", 0) == 0;
  const bool ref_is_empty = ref.empty();
  const bool ref_is_yt_watch =
      !ref_is_empty && ref.find("youtube.com") != std::string::npos &&
      ref.find("/embed/") == std::string::npos;
  const bool ref_is_embed_ctx =
      ref_is_file || ref_is_empty ||
      (!ref_is_empty && ref.find("/embed/") != std::string::npos) ||
      (!ref_is_empty && ref.find("reddit.com") != std::string::npos);

  if (is_googlevideo) {
    if (BerryGooglevideoUrlIsAllowlisted(spec)) {
      static const char kAndroidVrUA[] =
          "com.google.android.apps.youtube.vr.oculus/1.65.10 (Linux; U; Android "
          "12L; eureka-user Build/SQ3A.220605.009.A1) gzip";
      url_request->SetExtraRequestHeaderByName("User-Agent", kAndroidVrUA, true);
      url_request->RemoveRequestHeaderByName("Origin");
      url_request->SetReferrer(std::string());
      url_request->set_referrer_policy(net::ReferrerPolicy::NEVER_CLEAR);
      QNX_NAV_LOG_FMT("BerryNav: GooglevideoAllowlist UA url=\"%.100s\"\n",
                      spec.c_str());
      return;
    }
    static const char kWatchReferer[] = "https://www.youtube.com/";
    url_request->SetReferrer(kWatchReferer);
    url_request->set_referrer_policy(net::ReferrerPolicy::NEVER_CLEAR);
    url_request->SetExtraRequestHeaderByName("Origin", "https://www.youtube.com",
                                             true);
    QNX_NAV_LOG_FMT(
        "BerryNav: WatchReferer url=\"%.100s\" old_ref=\"%.60s\"\n",
        spec.c_str(), ref.c_str());
    return;
  }

  if (!is_embed_page && !ref_is_embed_ctx)
    return;
  if (!ref_is_file && !ref_is_empty && ref_is_yt_watch)
    return;

  static const char kEmbedReferer[] = "https://www.reddit.com/";
  url_request->SetReferrer(kEmbedReferer);
  url_request->set_referrer_policy(net::ReferrerPolicy::NEVER_CLEAR);
  QNX_NAV_LOG_FMT(
      "BerryNav: EmbedReferer fix url=\"%.100s\" old_ref=\"%.60s\"\n",
      spec.c_str(), ref.c_str());
}

// Rewrite youtubei/v1/player POST bodies from WEB_EMBEDDED_PLAYER (SABR-only,
// 403 on content_shell) to ANDROID_VR 1.65.10 which still serves progressive
// HTTPS formats (fmt=18) without PO tokens per yt-dlp client table.
bool BerryRewriteYoutubePlayerJson(std::string* body) {
  if (!body)
    return false;

  const bool use_ios =
      access("/accounts/1000/shared/misc/berry-youtube-ios.enable", F_OK) == 0;
  const char* kClient = use_ios ? "IOS" : "ANDROID_VR";
  const char* kVersion = use_ios ? "19.45.4" : "1.65.10";
  const char* kDeviceFields = use_ios
      ? ",\"deviceMake\":\"Apple\",\"deviceModel\":\"iPhone14,3\","
        "\"osName\":\"iPhone\",\"osVersion\":\"17.0\""
      : ",\"deviceMake\":\"Oculus\",\"deviceModel\":\"Quest "
        "3\",\"androidSdkVersion\":32,\"osName\":\"Android\","
        "\"osVersion\":\"12L\"";

  bool rewritten = false;
  if (body->find("WEB_EMBEDDED") != std::string::npos) {
    base::ReplaceSubstringsAfterOffset(body, 0, "WEB_EMBEDDED_PLAYER", kClient);
    rewritten = true;
  }

  static const char* kWebClients[] = {
      "\"clientName\":\"WEB\"",
      "\"clientName\":\"MWEB\"",
      "\"clientName\":\"WEB_REMIX\"",
      "\"clientName\":\"WEB_CREATOR\"",
      "\"clientName\": \"WEB\"",
      "\"clientName\": \"MWEB\"",
  };
  const std::string kClientJson = std::string("\"clientName\":\"") + kClient + "\"";
  for (const char* from : kWebClients) {
    if (body->find(from) != std::string::npos) {
      base::ReplaceSubstringsAfterOffset(body, 0, from, kClientJson);
      rewritten = true;
    }
  }

  if (!rewritten && body->find(kClient) == std::string::npos)
    return false;

  const std::string kName = kClientJson;
  size_t pos = body->find(kName);
  if (pos == std::string::npos)
    return rewritten;

  size_t ver_key = body->find("\"clientVersion\":", pos);
  if (ver_key != std::string::npos) {
    size_t val_start = body->find('"', ver_key + 16);
    if (val_start != std::string::npos) {
      size_t val_end = body->find('"', val_start + 1);
      if (val_end != std::string::npos)
        body->replace(val_start + 1, val_end - val_start - 1, kVersion);
    }
  }

  if (body->find("\"deviceMake\":", pos) == std::string::npos) {
    size_t insert_at = body->find("\"clientVersion\":", pos);
    if (insert_at != std::string::npos) {
      insert_at = body->find('"', insert_at + 16);
      if (insert_at != std::string::npos) {
        insert_at = body->find('"', insert_at + 1);
        if (insert_at != std::string::npos)
          body->insert(insert_at + 1, kDeviceFields);
      }
    }
  }

  return true;
}

bool BerryShouldBufferYoutubeResponse(const GURL& url) {
  if (!url.is_valid() || url.host().find("youtube.com") == std::string::npos)
    return false;
  return url.spec().find("/youtubei/v1/player") != std::string::npos;
}

// Strip SABR-only fields from youtubei/v1/player JSON so the JS player picks
// progressive formats (fmt=18). Do NOT run on watch-page HTML (breaks kevlar).
bool BerryStripSabrFromYoutubePlayerResponse(const GURL& url,
                                             std::string* body) {
  if (!body || body->empty() || body->size() > 512 * 1024)
    return false;
  if (url.spec().find("/youtubei/v1/player") == std::string::npos)
    return false;
  if (body->find("streamingData") == std::string::npos &&
      body->find("serverAbrStreamingUrl") == std::string::npos)
    return false;

  bool changed = false;
  for (const char* key :
       {"\"serverAbrStreamingUrl\"", "\"sabrContextUpdate\""}) {
    for (;;) {
      size_t pos = body->find(key);
      if (pos == std::string::npos)
        break;
      size_t start = pos;
      if (start > 0 && (*body)[start - 1] == ',')
        --start;
      size_t colon = body->find(':', pos);
      if (colon == std::string::npos)
        break;
      size_t val_start = colon + 1;
      while (val_start < body->size() &&
             ((*body)[val_start] == ' ' || (*body)[val_start] == '\t'))
        ++val_start;
      size_t end = val_start;
      if (val_start < body->size() && (*body)[val_start] == '"') {
        end = body->find('"', val_start + 1);
        if (end == std::string::npos)
          break;
        ++end;
      } else if (val_start < body->size() && (*body)[val_start] == '{') {
        int depth = 0;
        for (end = val_start; end < body->size(); ++end) {
          if ((*body)[end] == '{')
            ++depth;
          else if ((*body)[end] == '}') {
            --depth;
            if (depth == 0) {
              ++end;
              break;
            }
          }
        }
      } else {
        break;
      }
      if (end < body->size() && (*body)[end] == ',')
        ++end;
      body->erase(start, end - start);
      changed = true;
    }
  }

  const size_t before = body->size();
  base::ReplaceSubstringsAfterOffset(body, 0, "sabr=1&", "");
  base::ReplaceSubstringsAfterOffset(body, 0, "&sabr=1", "");
  base::ReplaceSubstringsAfterOffset(body, 0, "?sabr=1&", "?");
  base::ReplaceSubstringsAfterOffset(body, 0, "?sabr=1", "?");
  base::ReplaceSubstringsAfterOffset(body, 0, "&keepalive=yes", "");
  base::ReplaceSubstringsAfterOffset(body, 0, ",sabr", "");
  base::ReplaceSubstringsAfterOffset(body, 0, "sabr,", "");
  if (body->size() != before)
    changed = true;

  if (changed) {
    QNX_NAV_LOG_FMT(
        "BerryNav: PlayerResponseStripSabr bytes=%zu url=\"%.80s\"\n",
        body->size(), url.spec().c_str());
  }
  if (BerryScrubGooglevideoUrlsInPlayerBody(body))
    changed = true;
  return changed;
}

void BerryMaybeSpoofYoutubeInnertube(ResourceRequest* request,
                                     net::URLRequest* url_request) {
  if (!request || !url_request)
    return;
  if (access("/accounts/1000/shared/misc/berry-youtube-innertube.disable",
             F_OK) == 0)
    return;
  if (request->method != "POST")
    return;
  const GURL& url = request->url;
  if (!url.is_valid() || url.host().find("youtube.com") == std::string::npos)
    return;
  const std::string spec = url.spec();
  if (spec.find("/youtubei/v1/player") == std::string::npos &&
      spec.find("/youtubei/v1/search") == std::string::npos)
    return;
  if (!request->request_body)
    return;

  BerryLoadPersistedVisitorData();

  bool spoofed = false;
  size_t spoof_body_size = 0;
  bool visitor_injected = false;
  for (auto& element : *request->request_body->elements_mutable()) {
    if (element.type() != mojom::DataElementDataView::Tag::kBytes)
      continue;
    std::string json(element.As<DataElementBytes>().AsStringPiece());
    const bool rewrote = BerryRewriteYoutubePlayerJson(&json);
    const bool injected = BerryInjectVisitorDataIntoPlayerJson(&json);
    if (!rewrote && !injected)
      continue;
    if (injected)
      visitor_injected = true;
    spoof_body_size = json.size();
    element = DataElement(DataElementBytes(
        std::vector<uint8_t>(json.begin(), json.end())));
    spoofed = true;
    break;
  }

  if (!spoofed)
    return;

  const std::string visitor_header = g_berry_youtube_visitor_data;
  const bool use_ios =
      access("/accounts/1000/shared/misc/berry-youtube-ios.enable", F_OK) == 0;
  if (!use_ios) {
    static const char kAndroidVrUA[] =
        "com.google.android.apps.youtube.vr.oculus/1.65.10 (Linux; U; Android "
        "12L; eureka-user Build/SQ3A.220605.009.A1) gzip";
    url_request->SetExtraRequestHeaderByName("User-Agent", kAndroidVrUA, true);
    url_request->SetExtraRequestHeaderByName("X-Youtube-Client-Name", "28",
                                             true);
    url_request->SetExtraRequestHeaderByName("X-Youtube-Client-Version",
                                             "1.65.10", true);
    if (!visitor_header.empty()) {
      url_request->SetExtraRequestHeaderByName("X-Goog-Visitor-Id",
                                               visitor_header.c_str(), true);
    }
    QNX_NAV_LOG_FMT(
        "BerryNav: InnertubeSpoof ANDROID_VR/1.65.10 body=%zu visitor=%s "
        "hdrs=UA,ClientName,ClientVersion%s url=\"%.80s\"\n",
        spoof_body_size,
        visitor_injected ? "injected"
                         : (visitor_header.empty() ? "none" : "cached"),
        visitor_header.empty() ? "" : ",GoogVisitorId", spec.c_str());
  } else {
    static const char kIosUA[] =
        "com.google.ios.youtube/19.45.4 (iPhone14,3; U; CPU iOS 17_0 like Mac "
        "OS X) gzip";
    url_request->SetExtraRequestHeaderByName("User-Agent", kIosUA, true);
    url_request->SetExtraRequestHeaderByName("X-Youtube-Client-Name", "5",
                                             true);
    url_request->SetExtraRequestHeaderByName("X-Youtube-Client-Version",
                                             "19.45.4", true);
    if (!visitor_header.empty()) {
      url_request->SetExtraRequestHeaderByName("X-Goog-Visitor-Id",
                                               visitor_header.c_str(), true);
    }
    QNX_NAV_LOG_FMT(
        "BerryNav: InnertubeSpoof IOS/19.45.4 body=%zu visitor=%s "
        "hdrs=UA,ClientName,ClientVersion%s url=\"%.80s\"\n",
        spoof_body_size,
        visitor_injected ? "injected"
                         : (visitor_header.empty() ? "none" : "cached"),
        visitor_header.empty() ? "" : ",GoogVisitorId", spec.c_str());
  }
}

std::string BerryHtmlEscape(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 16);
  for (char c : input) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

std::string BerryJsStringEscape(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 16);
  for (char c : input) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

// youtube.com or the cookie-less embed domain (youtube-nocookie.com does NOT
// contain the substring "youtube.com", so it needs its own check).
bool BerryIsYoutubeHost(const GURL& url) {
  if (!url.is_valid())
    return false;
  const std::string& host = url.host();
  return host.find("youtube.com") != std::string::npos ||
         host.find("youtube-nocookie.com") != std::string::npos;
}

// Video id from a path-style URL: /embed/<id> (third-party iframes, e.g. Bing
// Videos), /shorts/<id> (shared links), /v/<id> (legacy embeds). Returns empty
// for playlist pseudo-ids ("videoseries") and implausible segments.
std::string BerryExtractYoutubeVideoIdFromPath(const GURL& url) {
  static const char* kPrefixes[] = {"/embed/", "/shorts/", "/v/"};
  const std::string& path = url.path();
  for (const char* prefix : kPrefixes) {
    if (!base::StartsWith(path, prefix, base::CompareCase::SENSITIVE))
      continue;
    std::string id = path.substr(strlen(prefix));
    const size_t slash = id.find('/');
    if (slash != std::string::npos)
      id = id.substr(0, slash);
    if (id.empty() || id.size() > 20 || id == "videoseries")
      return std::string();
    return id;
  }
  return std::string();
}

std::string BerryExtractYoutubeVideoId(const GURL& url) {
  if (!url.is_valid())
    return std::string();
  for (net::QueryIterator it(url); !it.IsAtEnd(); it.Advance()) {
    if (it.GetKey() == "v" && !it.GetValue().empty())
      return std::string(it.GetValue());
  }
  {
    const std::string path_id = BerryExtractYoutubeVideoIdFromPath(url);
    if (!path_id.empty())
      return path_id;
  }
  const std::string spec = url.spec();
  static const char* kNeedles[] = {"watch?v=", "?v=", "&v="};
  for (const char* needle : kNeedles) {
    size_t pos = spec.find(needle);
    if (pos == std::string::npos)
      continue;
    pos += strlen(needle);
    size_t end = pos;
    while (end < spec.size()) {
      const char c = spec[end];
      if (c == '&' || c == '?' || c == '#' || c == '"')
        break;
      ++end;
    }
    if (end <= pos)
      continue;
    std::string v = spec.substr(pos, end - pos);
    const size_t amp = v.find('&');
    if (amp != std::string::npos)
      v = v.substr(0, amp);
    if (!v.empty())
      return v;
  }
  return std::string();
}

GURL BerryCanonicalizeYoutubeWatchUrl(const GURL& url) {
  const std::string vid = BerryExtractYoutubeVideoId(url);
  if (vid.empty())
    return url;
  return GURL("https://www.youtube.com/watch?v=" + vid);
}

bool BerryWatchUrlRequestsFullPage(const GURL& url) {
  if (access("/accounts/1000/shared/misc/berry-youtube-fullwatch.enable",
             F_OK) == 0)
    return true;
  for (net::QueryIterator it(url); !it.IsAtEnd(); it.Advance()) {
    if (it.GetKey() == "berry_full" && it.GetValue() == "1")
      return true;
  }
  const std::string spec = url.spec();
  return spec.find("berry_full=1") != std::string::npos;
}

bool BerryShouldUseYoutubeWatchShim(const net::URLRequest* req,
                                    int resource_type) {
  // resource_type: 0 = main frame, 1 = subframe. /watch stays main-frame-only
  // (subframe watch loads are YouTube-internal). Path-style ids (/embed,
  // /shorts, /v) are shimmed in BOTH: third-party sites (Bing Videos, news,
  // blogs) load /embed/<id> iframes whose real YouTube player dies on this
  // browser ("An error occurred").
  if (!req || (resource_type != 0 && resource_type != 1))
    return false;
  if (access("/accounts/1000/shared/misc/berry-youtube-shim.disable", F_OK) ==
      0)
    return false;
  const GURL& url = req->url();
  if (!BerryIsYoutubeHost(url))
    return false;
  const bool is_watch_path =
      url.path() == "/watch" || url.path() == "/watch/";
  const bool is_path_id = !BerryExtractYoutubeVideoIdFromPath(url).empty();
  if (!is_path_id && (!is_watch_path || resource_type != 0))
    return false;
  if (BerryWatchUrlRequestsFullPage(url))
    return false;
  return !BerryExtractYoutubeVideoId(url).empty();
}

std::string BerryExtractYoutubeSearchQuery(const GURL& url) {
  if (!url.is_valid())
    return std::string();
  for (net::QueryIterator it(url); !it.IsAtEnd(); it.Advance()) {
    if (it.GetKey() == "search_query" && !it.GetValue().empty())
      return std::string(it.GetValue());
  }
  return std::string();
}

bool BerryShouldUseYoutubeSearchShim(const net::URLRequest* req,
                                     int resource_type) {
  if (resource_type != 0 || !req)
    return false;
  if (access("/accounts/1000/shared/misc/berry-youtube-shim.disable", F_OK) ==
      0)
    return false;
  const GURL& url = req->url();
  if (!url.is_valid() || url.host().find("youtube.com") == std::string::npos)
    return false;
  if (url.path() != "/results" && url.path() != "/results/")
    return false;
  return !BerryExtractYoutubeSearchQuery(url).empty();
}

bool BerryUrlIsSearchShimTarget(const GURL& url) {
  if (!url.is_valid() || url.host().find("youtube.com") == std::string::npos)
    return false;
  if (url.path() != "/results" && url.path() != "/results/")
    return false;
  if (access("/accounts/1000/shared/misc/berry-youtube-shim.disable", F_OK) == 0)
    return false;
  return !BerryExtractYoutubeSearchQuery(url).empty();
}

bool BerryUrlIsGoogleSorryPath(const GURL& url) {
  if (!url.is_valid())
    return false;
  if (url.host().find("google.") == std::string::npos)
    return false;
  const std::string& path = url.path();
  return path == "/sorry" || path == "/sorry/" ||
         base::StartsWith(path, "/sorry/", base::CompareCase::SENSITIVE);
}

bool BerryShouldUseGoogleSorryBounce(const net::URLRequest* req,
                                      int resource_type) {
  if (resource_type != 0 || !req)
    return false;
  if (access("/accounts/1000/shared/misc/berry-google-sorry-shim.disable",
              F_OK) == 0)
    return false;
  return BerryUrlIsGoogleSorryPath(req->url());
}

bool BerryUrlIsGoogleSorryBounceTarget(const GURL& url) {
  if (access("/accounts/1000/shared/misc/berry-google-sorry-shim.disable",
              F_OK) == 0)
    return false;
  return BerryUrlIsGoogleSorryPath(url);
}

// youtube.com/ home feed: the kevlar desktop app half-loads on this browser
// (broken skeleton / error). Body-swap a local landing page that feeds the
// working search shim instead. berry_full=1 escapes to the real site.
bool BerryUrlIsYoutubeHomeBounceTarget(const GURL& url) {
  if (!BerryIsYoutubeHost(url))
    return false;
  const std::string& path = url.path();
  if (path != "/" && !path.empty())
    return false;
  if (access("/accounts/1000/shared/misc/berry-youtube-shim.disable", F_OK) ==
      0)
    return false;
  if (BerryWatchUrlRequestsFullPage(url))
    return false;
  return true;
}

bool BerryShouldUseYoutubeHomeBounce(const net::URLRequest* req,
                                     int resource_type) {
  if (resource_type != 0 || !req)
    return false;
  return BerryUrlIsYoutubeHomeBounceTarget(req->url());
}

std::string BerryExtractGoogleSearchQueryFromSorryUrl(const GURL& url) {
  if (!url.is_valid())
    return std::string();
  for (net::QueryIterator it(url); !it.IsAtEnd(); it.Advance()) {
    if (it.GetKey() != "continue" || it.GetValue().empty())
      continue;
    std::string continue_str(it.GetValue());
    GURL continue_url(continue_str);
    if (!continue_url.is_valid()) {
      continue_str = base::UnescapeURLComponent(
          continue_str,
          base::UnescapeRule::SPACES | base::UnescapeRule::PATH_SEPARATORS |
              base::UnescapeRule::URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS);
      continue_url = GURL(continue_str);
    }
    if (!continue_url.is_valid())
      continue;
    for (net::QueryIterator qit(continue_url); !qit.IsAtEnd(); qit.Advance()) {
      if (qit.GetKey() == "q" && !qit.GetValue().empty())
        return std::string(qit.GetValue());
    }
  }
  return std::string();
}

std::string BerryWatchFullPageFallbackUrl(const std::string& watch_url_spec) {
  if (watch_url_spec.find('?') != std::string::npos)
    return watch_url_spec + "&berry_full=1";
  return watch_url_spec + "?berry_full=1";
}

// Tripwire: log if shim HTML was served but berry_js_alive never arrives.
std::atomic<uint64_t> g_berry_shim_flush_gen{0};
std::atomic<bool> g_berry_shim_js_alive{false};

void BerryCheckWatchShimJsAlive(uint64_t gen, std::string video_id) {
  if (gen != g_berry_shim_flush_gen.load(std::memory_order_relaxed))
    return;
  if (g_berry_shim_js_alive.load(std::memory_order_relaxed))
    return;
  QNX_NAV_LOG_FMT(
      "BerryNav: WatchShimJsAlive MISSING gen=%llu v=\"%.20s\" "
      "(no berry_js_alive within 5s — script dead?)\n",
      static_cast<unsigned long long>(gen), video_id.c_str());
}

void BerryOnWatchShimFlushed(const std::string& video_id) {
  const uint64_t gen =
      g_berry_shim_flush_gen.fetch_add(1, std::memory_order_relaxed) + 1;
  g_berry_shim_js_alive.store(false, std::memory_order_relaxed);
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&BerryCheckWatchShimJsAlive, gen, video_id),
      base::Seconds(5));
}

void BerryMaybeMarkWatchShimJsAlive(const GURL& url) {
  if (!url.is_valid())
    return;
  if (url.spec().find("berry_js_alive=1") == std::string::npos)
    return;
  g_berry_shim_js_alive.store(true, std::memory_order_relaxed);
  QNX_NAV_LOG_FMT("BerryNav: WatchShimJsAlive OK url=\"%.80s\"\n",
                  url.spec().c_str());
}

void BerryCancelWatchShimJsAliveTripwire() {
  g_berry_shim_flush_gen.fetch_add(1, std::memory_order_relaxed);
}

bool BerryUrlIsWatchShimTarget(const GURL& url) {
  if (!BerryIsYoutubeHost(url))
    return false;
  const bool is_watch_path =
      url.path() == "/watch" || url.path() == "/watch/";
  if (!is_watch_path && BerryExtractYoutubeVideoIdFromPath(url).empty())
    return false;
  if (access("/accounts/1000/shared/misc/berry-youtube-shim.disable", F_OK) == 0)
    return false;
  if (BerryWatchUrlRequestsFullPage(url))
    return false;
  return !BerryExtractYoutubeVideoId(url).empty();
}

void BerryScrubWatchShimResponseHeaders(mojom::URLResponseHead* response,
                                        size_t shim_body_size,
                                        const GURL& url) {
  if (!response || !response->headers)
    return;
  // Zero-C++ fallback if parsed-header regen ever fails: extract 'nonce-…' from
  // the real CSP header before removal and stamp <script nonce="…"> on the shim
  // body — strict-dynamic + a valid nonce lets inline script run under YouTube's
  // policy without weakening it.
  static const char* kRemove[] = {
      "Content-Security-Policy",
      "Content-Security-Policy-Report-Only",
      "Content-Encoding",
      "Transfer-Encoding",
      "Cross-Origin-Opener-Policy",
      "Cross-Origin-Embedder-Policy",
      // Shim HTML may be served into third-party iframes (/embed, /shorts);
      // YouTube's SAMEORIGIN on watch/shorts would blank the frame.
      "X-Frame-Options",
  };
  for (const char* name : kRemove)
    response->headers->RemoveHeader(name);
  response->headers->SetHeader("Content-Type", "text/html; charset=utf-8");
  response->headers->SetHeader("Content-Length",
                               base::NumberToString(shim_body_size));
  response->mime_type = "text/html";
  response->charset = "utf-8";
  response->content_length = static_cast<int64_t>(shim_body_size);
  // Blink reads CSP from parsed_headers, not the raw header list. Regenerate
  // after scrub so content_security_policy (and COOP/COEP we stripped) match.
  response->parsed_headers =
      network::PopulateParsedHeaders(response->headers.get(), url);
}

void BerryScrubSorryBounceResponseHeaders(mojom::URLResponseHead* response,
                                          size_t shim_body_size,
                                          const GURL& url) {
  BerryScrubWatchShimResponseHeaders(response, shim_body_size, url);
  if (response && response->headers)
    response->headers->ReplaceStatusLine("HTTP/1.1 200 OK");
}

std::string BerryBuildWatchShimHtml(const GURL& watch_url,
                                    const std::string& video_id) {
  (void)watch_url;
  std::string script = network::kBerryWatchShimScript;
  const std::string safe_vid = BerryHtmlEscape(video_id);
  base::ReplaceSubstringsAfterOffset(&script, 0, "__BERRY_VIDEO_ID__",
                                     safe_vid);
  return std::string(
             "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width\">"
             "<title>Loading...</title>"
             "<style>"
             "body{font-family:sans-serif;background:#111;color:#eee;margin:0;"
             "padding:12px}"
             "#title{font-size:1.2em;margin:0 0 8px}"
             "video{width:100%;max-width:960px;background:#000}"
             "#status{color:#888;font-size:0.9em;margin:8px 0}"
             "#status.tap{cursor:pointer;color:#6af;text-decoration:underline}"
             "</style></head><body>"
             "<h1 id=\"title\">Loading...</h1>"
             "<p id=\"status\">Fetching stream...</p>"
             "<video id=\"v\" controls autoplay playsinline></video>"
             "<script>") +
         script + "</script></body></html>";
}

std::string BerryBuildSearchShimHtml(const GURL& results_url,
                                     const std::string& search_query) {
  (void)results_url;
  std::string script = network::kBerrySearchShimScript;
  const std::string safe_query = BerryJsStringEscape(search_query);
  base::ReplaceSubstringsAfterOffset(&script, 0, "__BERRY_SEARCH_QUERY__",
                                     safe_query);
  const std::string title = BerryHtmlEscape(search_query);
  return std::string(
             "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width\">"
             "<title>") +
         title +
         std::string(
             " — YouTube Search</title>"
             "<style>"
             "body{font-family:sans-serif;background:#111;color:#eee;margin:0;"
             "padding:12px}"
             "form{display:flex;gap:8px;margin:0 0 12px}"
             "input{flex:1;font-size:18px;padding:10px;border-radius:8px;"
             "border:1px solid #333;background:#0b0f14;color:#fff}"
             "button{font-size:18px;padding:10px 16px;border:none;border-radius:8px;"
             "background:#cc0000;color:#fff;font-weight:700}"
             "#status{color:#888;font-size:0.9em;margin:8px 0}"
             "#status.tap{cursor:pointer;color:#6af;text-decoration:underline}"
             "#list{display:grid;gap:10px}"
             "a.row{display:flex;gap:10px;text-decoration:none;color:#eee;"
             "padding:8px;border-radius:8px;background:#1a1a1a}"
             "a.row:active{background:#252525}"
             "img{width:120px;height:68px;object-fit:cover;background:#000;"
             "border-radius:4px;flex-shrink:0}"
             ".meta{flex:1;min-width:0}"
             ".t{font-size:1em;font-weight:600;margin:0 0 4px}"
             ".c{font-size:0.85em;color:#aaa}"
             ".d{font-size:0.85em;color:#888;align-self:flex-start}"
             "</style></head><body>"
             "<form id=\"searchForm\"><input id=\"q\" type=\"search\" "
             "value=\"") +
         title +
         std::string(
             "\" autocomplete=\"off\"><button type=\"submit\">Search</button>"
             "</form>"
             "<p id=\"status\">Searching...</p>"
             "<div id=\"list\"></div>"
             "<script>") +
         script + "</script></body></html>";
}

std::string BerryBuildSorryBounceHtml(const GURL& sorry_url,
                                      const std::string& search_query) {
  (void)sorry_url;
  const std::string q_enc = base::EscapeQueryParamValue(search_query, false);
  const std::string ddg_url =
      "https://html.duckduckgo.com/html/?q=" + q_enc;
  const std::string google_url =
      "https://www.google.com/search?q=" + q_enc;
  const std::string q_label =
      search_query.empty() ? std::string("your search")
                           : BerryHtmlEscape(search_query);
  return std::string(
             "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width\">"
             "<title>Google blocked this search</title>"
             "<style>"
             "body{font-family:sans-serif;background:#111;color:#eee;margin:0;"
             "padding:16px;max-width:640px}"
             "h1{font-size:1.25em;margin:0 0 12px;line-height:1.3}"
             "p{color:#aaa;font-size:0.95em;margin:0 0 16px;line-height:1.4}"
             ".q{color:#fff;font-weight:600;word-break:break-word}"
             ".actions{display:flex;flex-direction:column;gap:10px}"
             "a.btn{display:block;text-align:center;padding:14px 16px;"
             "border-radius:10px;text-decoration:none;font-size:1.05em;"
             "font-weight:700}"
             "a.primary{background:#58a6ff;color:#111}"
             "a.secondary{background:#333;color:#eee;border:1px solid #555}"
             "a:active{opacity:0.85}"
             "</style></head><body>"
             "<h1>Google thinks we're a robot</h1>"
             "<p>Search blocked for: <span class=\"q\">") +
         q_label +
         std::string(
             "</span>. You can try Google again (it often works on retry) or "
             "search DuckDuckGo instead.</p>"
             "<div class=\"actions\">"
             "<a class=\"btn primary\" href=\"") +
         ddg_url +
         std::string("\">Search DuckDuckGo</a>"
                     "<a class=\"btn secondary\" href=\"") +
         google_url +
         std::string("\">Try Google again</a>"
                     "</div></body></html>");
}

std::string BerryBuildYoutubeHomeBounceHtml() {
  return std::string(
      "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width\">"
      "<title>YouTube</title>"
      "<style>"
      "body{font-family:sans-serif;background:#111;color:#eee;margin:0;"
      "padding:24px 16px;max-width:640px}"
      "h1{font-size:1.4em;margin:0 0 4px}"
      "h1 .red{color:#f33}"
      "p{color:#888;font-size:0.9em;margin:0 0 18px;line-height:1.4}"
      "form{display:flex;gap:8px;margin:0 0 14px}"
      "input{flex:1;padding:13px 14px;border-radius:10px;border:1px solid "
      "#444;background:#1c1c1c;color:#eee;font-size:1.05em}"
      "button{padding:13px 18px;border-radius:10px;border:0;background:#f33;"
      "color:#fff;font-size:1.05em;font-weight:700}"
      "a.full{color:#58a6ff;font-size:0.85em;text-decoration:none}"
      "</style></head><body>"
      "<h1><span class=\"red\">&#9654;</span> YouTube</h1>"
      "<p>Search videos below &mdash; results and playback use the fast "
      "built-in player. The full YouTube site doesn't work well on this "
      "browser.</p>"
      "<form action=\"https://www.youtube.com/results\" method=\"get\">"
      "<input name=\"search_query\" placeholder=\"Search YouTube\" "
      "autofocus autocomplete=\"off\">"
      "<button type=\"submit\">Go</button></form>"
      "<a class=\"full\" href=\"https://www.youtube.com/?berry_full=1\">"
      "Load full YouTube site anyway</a>"
      "</body></html>");
}
#endif

std::unique_ptr<net::UploadDataStream> CreateUploadDataStream(
    ResourceRequestBody* body,
    std::vector<base::File>& opened_files,
    base::SequencedTaskRunner* file_task_runner) {
  // In the case of a chunked upload, there will just be one element.
  if (body->elements()->size() == 1) {
    if (body->elements()->begin()->type() ==
        network::mojom::DataElementDataView::Tag::kChunkedDataPipe) {
      auto& element =
          body->elements_mutable()->at(0).As<DataElementChunkedDataPipe>();
      const bool has_null_source = element.read_only_once().value();
      auto upload_data_stream =
          std::make_unique<ChunkedDataPipeUploadDataStream>(
              body, element.ReleaseChunkedDataPipeGetter(), has_null_source);
      if (element.read_only_once()) {
        upload_data_stream->EnableCache();
      }
      return upload_data_stream;
    }
  }

  auto opened_file = opened_files.begin();
  std::vector<std::unique_ptr<net::UploadElementReader>> element_readers;
  for (const auto& element : *body->elements()) {
    switch (element.type()) {
      case network::mojom::DataElementDataView::Tag::kBytes:
        element_readers.push_back(std::make_unique<BytesElementReader>(
            body, element.As<DataElementBytes>()));
        break;
      case network::mojom::DataElementDataView::Tag::kFile:
        DCHECK(opened_file != opened_files.end());
        element_readers.push_back(std::make_unique<FileElementReader>(
            body, file_task_runner, element.As<network::DataElementFile>(),
            std::move(*opened_file++)));
        break;
      case network::mojom::DataElementDataView::Tag::kDataPipe: {
        element_readers.push_back(std::make_unique<DataPipeElementReader>(
            body,
            element.As<network::DataElementDataPipe>().CloneDataPipeGetter()));
        break;
      }
      case network::mojom::DataElementDataView::Tag::kChunkedDataPipe: {
        // This shouldn't happen, as the traits logic should ensure that if
        // there's a chunked pipe, there's one and only one element.
        NOTREACHED();
        break;
      }
    }
  }
  DCHECK(opened_file == opened_files.end());

  return std::make_unique<net::ElementsUploadDataStream>(
      std::move(element_readers), body->identifier());
}

class SSLPrivateKeyInternal : public net::SSLPrivateKey {
 public:
  SSLPrivateKeyInternal(
      const std::string& provider_name,
      const std::vector<uint16_t>& algorithm_preferences,
      mojo::PendingRemote<mojom::SSLPrivateKey> ssl_private_key)
      : provider_name_(provider_name),
        algorithm_preferences_(algorithm_preferences),
        ssl_private_key_(std::move(ssl_private_key)) {
    ssl_private_key_.set_disconnect_handler(
        base::BindOnce(&SSLPrivateKeyInternal::HandleSSLPrivateKeyError,
                       base::Unretained(this)));
  }

  SSLPrivateKeyInternal(const SSLPrivateKeyInternal&) = delete;
  SSLPrivateKeyInternal& operator=(const SSLPrivateKeyInternal&) = delete;

  // net::SSLPrivateKey:
  std::string GetProviderName() override { return provider_name_; }

  std::vector<uint16_t> GetAlgorithmPreferences() override {
    return algorithm_preferences_;
  }

  void Sign(uint16_t algorithm,
            base::span<const uint8_t> input,
            net::SSLPrivateKey::SignCallback callback) override {
    std::vector<uint8_t> input_vector(input.begin(), input.end());
    if (!ssl_private_key_ || !ssl_private_key_.is_connected()) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(std::move(callback),
                         net::ERR_SSL_CLIENT_AUTH_CERT_NO_PRIVATE_KEY,
                         input_vector));
      return;
    }

    ssl_private_key_->Sign(algorithm, input_vector,
                           base::BindOnce(&SSLPrivateKeyInternal::Callback,
                                          this, std::move(callback)));
  }

 private:
  ~SSLPrivateKeyInternal() override = default;

  void HandleSSLPrivateKeyError() { ssl_private_key_.reset(); }

  void Callback(net::SSLPrivateKey::SignCallback callback,
                int32_t net_error,
                const std::vector<uint8_t>& input) {
    DCHECK_LE(net_error, 0);
    DCHECK_NE(net_error, net::ERR_IO_PENDING);
    std::move(callback).Run(static_cast<net::Error>(net_error), input);
  }

  std::string provider_name_;
  std::vector<uint16_t> algorithm_preferences_;
  mojo::Remote<mojom::SSLPrivateKey> ssl_private_key_;
};

bool ShouldNotifyAboutCookie(net::CookieInclusionStatus status) {
  // Notify about cookies actually used, and those blocked by preferences ---
  // for purposes of cookie UI --- as well those carrying warnings pertaining to
  // SameSite features and cookies with non-ASCII domain attributes, in order to
  // issue a deprecation warning for them.

  // Filter out tentative secure source scheme warnings. They're used for netlog
  // debugging and not something we want to inform cookie observers about.
  status.RemoveWarningReason(
      net::CookieInclusionStatus::
          WARN_TENTATIVELY_ALLOWING_SECURE_SOURCE_SCHEME);

  return status.IsInclude() || status.ShouldWarn() ||
         status.HasExclusionReason(
             net::CookieInclusionStatus::EXCLUDE_USER_PREFERENCES) ||
         status.HasExclusionReason(
             net::CookieInclusionStatus::EXCLUDE_THIRD_PARTY_PHASEOUT) ||
         status.HasExclusionReason(
             net::CookieInclusionStatus::EXCLUDE_DOMAIN_NON_ASCII);
}

// Parses AcceptCHFrame and removes client hints already in the headers.
std::vector<mojom::WebClientHintsType> ComputeAcceptCHFrameHints(
    const std::string& accept_ch_frame,
    const net::HttpRequestHeaders& headers) {
  absl::optional<std::vector<mojom::WebClientHintsType>> maybe_hints =
      ParseClientHintsHeader(accept_ch_frame);

  if (!maybe_hints)
    return {};

  // Only look at/add headers that aren't already present.
  std::vector<mojom::WebClientHintsType> hints;
  for (auto hint : maybe_hints.value()) {
    // ResourceWidth is only for images, which won't trigger a restart.
    if (hint == mojom::WebClientHintsType::kResourceWidth ||
        hint == mojom::WebClientHintsType::kResourceWidth_DEPRECATED) {
      continue;
    }

    const std::string header = GetClientHintToNameMap().at(hint);
    if (!headers.HasHeader(header))
      hints.push_back(hint);
  }

  return hints;
}

// Returns true if the |credentials_mode| of the request allows sending
// credentials.
bool ShouldAllowCredentials(mojom::CredentialsMode credentials_mode) {
  switch (credentials_mode) {
    case mojom::CredentialsMode::kInclude:
    // TODO(crbug.com/943939): Make this work with CredentialsMode::kSameOrigin.
    case mojom::CredentialsMode::kSameOrigin:
      return true;

    case mojom::CredentialsMode::kOmit:
    case mojom::CredentialsMode::kOmitBug_775438_Workaround:
      return false;
  }
}

// Returns true when the |credentials_mode| of the request allows sending client
// certificates.
bool ShouldSendClientCertificates(mojom::CredentialsMode credentials_mode) {
  switch (credentials_mode) {
    case mojom::CredentialsMode::kInclude:
    case mojom::CredentialsMode::kSameOrigin:
      return true;

    // TODO(https://crbug.com/775438): Due to a bug, the default behavior does
    // not properly correspond to Fetch's "credentials mode", in that client
    // certificates will be sent if available, or the handshake will be aborted
    // to allow selecting a client cert.
    // With the feature kOmitCorsClientCert enabled, the correct
    // behavior is done; omit all client certs and continue the handshake
    // without sending one if requested.
    case mojom::CredentialsMode::kOmit:
      return !base::FeatureList::IsEnabled(features::kOmitCorsClientCert);

    case mojom::CredentialsMode::kOmitBug_775438_Workaround:
      return false;
  }
}

template <typename T>
T* PtrOrFallback(const mojo::Remote<T>& remote, T* fallback) {
  return remote.is_bound() ? remote.get() : fallback;
}

// Retrieves the Cookie header from either `cors_exempt_headers` or `headers`.
std::string GetCookiesFromHeaders(
    const net::HttpRequestHeaders& headers,
    const net::HttpRequestHeaders& cors_exempt_headers) {
  std::string cookies;
  if (!cors_exempt_headers.GetHeader(net::HttpRequestHeaders::kCookie,
                                     &cookies)) {
    headers.GetHeader(net::HttpRequestHeaders::kCookie, &cookies);
  }
  return cookies;
}

net::HttpRequestHeaders AttachCookies(const net::HttpRequestHeaders& headers,
                                      const std::string& cookies_from_browser) {
  DCHECK(!cookies_from_browser.empty());

  // Parse the existing cookie line.
  std::string old_cookies;
  headers.GetHeader(net::HttpRequestHeaders::kCookie, &old_cookies);
  net::cookie_util::ParsedRequestCookies parsed_cookies;

  net::cookie_util::ParseRequestCookieLine(old_cookies, &parsed_cookies);
  net::cookie_util::ParsedRequestCookies parsed_cookies_from_browser;
  net::cookie_util::ParseRequestCookieLine(cookies_from_browser,
                                           &parsed_cookies_from_browser);

  // Add the browser cookies to the request.
  for (auto cookie : parsed_cookies_from_browser) {
    DCHECK(!cookie.first.empty());

    // Ensure we're not adding duplicate cookies.
    auto it = std::find_if(
        parsed_cookies.begin(), parsed_cookies.end(),
        [&cookie](const net::cookie_util::ParsedRequestCookie& old_cookie) {
          return old_cookie.first == cookie.first;
        });
    if (it != parsed_cookies.end())
      continue;

    parsed_cookies.emplace_back(cookie.first, cookie.second);
  }

  net::HttpRequestHeaders updated_headers = headers;
  std::string updated_cookies =
      net::cookie_util::SerializeRequestCookieLine(parsed_cookies);
  updated_headers.SetHeader(net::HttpRequestHeaders::kCookie, updated_cookies);

  return updated_headers;
}

}  // namespace

URLLoader::MaybeSyncURLLoaderClient::MaybeSyncURLLoaderClient(
    mojo::PendingRemote<mojom::URLLoaderClient> mojo_client,
    base::WeakPtr<mojom::URLLoaderClient> sync_client)
    : mojo_client_(std::move(mojo_client)),
      sync_client_(std::move(sync_client)) {}

URLLoader::MaybeSyncURLLoaderClient::~MaybeSyncURLLoaderClient() = default;

void URLLoader::MaybeSyncURLLoaderClient::Reset() {
  mojo_client_.reset();
  sync_client_.reset();
}

mojo::PendingReceiver<mojom::URLLoaderClient>
URLLoader::MaybeSyncURLLoaderClient::BindNewPipeAndPassReceiver() {
  sync_client_.reset();
  return mojo_client_.BindNewPipeAndPassReceiver();
}

mojom::URLLoaderClient* URLLoader::MaybeSyncURLLoaderClient::Get() {
#if defined(__QNX__) || defined(__QNXNTO__)
  // Always use the Mojo remote on QNX. The sync in-process path re-enters the
  // network stack during OnReceiveResponse and can deadlock single-process mode
  // when many subresources arrive while socket reads are pending.
  if (mojo_client_)
    return mojo_client_.get();
  return nullptr;
#endif
  if (sync_client_)
    return sync_client_.get();
  if (mojo_client_)
    return mojo_client_.get();
  return nullptr;
}

URLLoader::PartialLoadInfo::PartialLoadInfo(net::LoadStateWithParam load_state,
                                            net::UploadProgress upload_progress)
    : load_state(std::move(load_state)),
      upload_progress(std::move(upload_progress)) {}

URLLoader::URLLoader(
    URLLoaderContext& context,
    DeleteCallback delete_callback,
    mojo::PendingReceiver<mojom::URLLoader> url_loader_receiver,
    int32_t options,
    const ResourceRequest& request,
    mojo::PendingRemote<mojom::URLLoaderClient> url_loader_client,
    base::WeakPtr<mojom::URLLoaderClient> sync_url_loader_client,
    const net::NetworkTrafficAnnotationTag& traffic_annotation,
    uint32_t request_id,
    int keepalive_request_size,
    base::WeakPtr<KeepaliveStatisticsRecorder> keepalive_statistics_recorder,
    std::unique_ptr<TrustTokenRequestHelperFactory> trust_token_helper_factory,
    std::unique_ptr<SharedDictionaryAccessChecker> shared_dictionary_checker,
    mojo::PendingRemote<mojom::CookieAccessObserver> cookie_observer,
    mojo::PendingRemote<mojom::TrustTokenAccessObserver> trust_token_observer,
    mojo::PendingRemote<mojom::URLLoaderNetworkServiceObserver>
        url_loader_network_observer,
    mojo::PendingRemote<mojom::DevToolsObserver> devtools_observer,
    mojo::PendingRemote<mojom::AcceptCHFrameObserver> accept_ch_frame_observer,
    net::CookieSettingOverrides cookie_setting_overrides,
    std::unique_ptr<AttributionRequestHelper> attribution_request_helper,
    bool shared_storage_writable_eligible)
    : url_request_context_(context.GetUrlRequestContext()),
      network_context_client_(context.GetNetworkContextClient()),
      delete_callback_(std::move(delete_callback)),
      options_(options),
      corb_detachable_(request.corb_detachable),
      resource_type_(request.resource_type),
      is_load_timing_enabled_(request.enable_load_timing),
      factory_params_(context.GetFactoryParams()),
      coep_reporter_(context.GetCoepReporter()),
      request_id_(request_id),
      keepalive_request_size_(keepalive_request_size),
      keepalive_(request.keepalive),
      do_not_prompt_for_login_(request.do_not_prompt_for_login),
      is_ad_tagged_(request.is_ad_tagged),
      receiver_(this, std::move(url_loader_receiver)),
      url_loader_client_(std::move(url_loader_client),
                         std::move(sync_url_loader_client)),
      writable_handle_watcher_(FROM_HERE,
                               mojo::SimpleWatcher::ArmingPolicy::MANUAL,
                               base::SequencedTaskRunner::GetCurrentDefault()),
      peer_closed_handle_watcher_(
          FROM_HERE,
          mojo::SimpleWatcher::ArmingPolicy::MANUAL,
          base::SequencedTaskRunner::GetCurrentDefault()),
      per_factory_corb_state_(context.GetMutableCorbState()),
      devtools_request_id_(request.devtools_request_id),
      request_mode_(request.mode),
      request_credentials_mode_(request.credentials_mode),
      request_destination_(request.destination),
      resource_scheduler_client_(context.GetResourceSchedulerClient()),
      keepalive_statistics_recorder_(std::move(keepalive_statistics_recorder)),
      custom_proxy_pre_cache_headers_(request.custom_proxy_pre_cache_headers),
      custom_proxy_post_cache_headers_(request.custom_proxy_post_cache_headers),
      fetch_window_id_(request.fetch_window_id),
      private_network_access_checker_(
          request,
          factory_params_->client_security_state.get(),
          options_),
      trust_token_helper_factory_(std::move(trust_token_helper_factory)),
      shared_dictionary_checker_(std::move(shared_dictionary_checker)),
      attribution_request_helper_(std::move(attribution_request_helper)),
      origin_access_list_(context.GetOriginAccessList()),
      cookie_observer_remote_(std::move(cookie_observer)),
      cookie_observer_(PtrOrFallback(cookie_observer_remote_,
                                     context.GetCookieAccessObserver())),
      trust_token_observer_remote_(std::move(trust_token_observer)),
      trust_token_observer_(
          PtrOrFallback(trust_token_observer_remote_,
                        context.GetTrustTokenAccessObserver())),
      url_loader_network_observer_remote_(
          std::move(url_loader_network_observer)),
      url_loader_network_observer_(
          PtrOrFallback(url_loader_network_observer_remote_,
                        context.GetURLLoaderNetworkServiceObserver())),
      devtools_observer_remote_(std::move(devtools_observer)),
      devtools_observer_(PtrOrFallback(devtools_observer_remote_,
                                       context.GetDevToolsObserver())),
      shared_storage_request_helper_(
          std::make_unique<SharedStorageRequestHelper>(
              shared_storage_writable_eligible,
              url_loader_network_observer_)),
      has_fetch_streaming_upload_body_(HasFetchStreamingUploadBody(&request)),
      allow_http1_for_streaming_upload_(
          request.request_body &&
          request.request_body->AllowHTTP1ForStreamingUpload()),
      accept_ch_frame_observer_(std::move(accept_ch_frame_observer)),
      provide_data_use_updates_(context.DataUseUpdatesEnabled()) {
  TRACE_EVENT("loading", "URLLoader::URLLoader",
              perfetto::Flow::FromPointer(this));
  QNX_TRACE_FMT("QNX:UL:ctor url=%s\n", request.url.spec().c_str());
  DCHECK(delete_callback_);

  mojom::TrustedURLLoaderHeaderClient* url_loader_header_client =
      context.GetUrlLoaderHeaderClient();
  if (url_loader_header_client &&
      (options_ & mojom::kURLLoadOptionUseHeaderClient)) {
    if (options_ & mojom::kURLLoadOptionAsCorsPreflight) {
      url_loader_header_client->OnLoaderForCorsPreflightCreated(
          request, header_client_.BindNewPipeAndPassReceiver());
    } else {
      url_loader_header_client->OnLoaderCreated(
          request_id_, header_client_.BindNewPipeAndPassReceiver());
    }
    // Make sure the loader dies if |header_client_| has an error, otherwise
    // requests can hang.
    header_client_.set_disconnect_handler(
        base::BindOnce(&URLLoader::OnMojoDisconnect, base::Unretained(this)));
  }
  if (devtools_request_id()) {
    options_ |= mojom::kURLLoadOptionSendSSLInfoWithResponse |
                mojom::kURLLoadOptionSendSSLInfoForCertificateError;
  }
  receiver_.set_disconnect_handler(
      base::BindOnce(&URLLoader::OnMojoDisconnect, base::Unretained(this)));
  GURL effective_url = request.url;
  std::string effective_method = request.method;
#if defined(__QNX__) || defined(__QNXNTO__)
  qnx_skip_request_body_ =
      BerryPrepareYoutubeUrlAndMethod(&effective_url, &effective_method,
                                      request.request_body.get());
#endif
  url_request_ = url_request_context_->CreateRequest(
      effective_url, request.priority, this, traffic_annotation,
      /*is_for_websockets=*/false, request.net_log_create_info);

  url_request_->set_method(effective_method);
  url_request_->set_site_for_cookies(request.site_for_cookies);
  if (ShouldForceIgnoreSiteForCookies(request))
    url_request_->set_force_ignore_site_for_cookies(true);
  if (!request.navigation_redirect_chain.empty()) {
    DCHECK_EQ(request.mode, mojom::RequestMode::kNavigate);
    url_request_->SetURLChain(request.navigation_redirect_chain);
  }
  url_request_->SetReferrer(request.referrer.GetAsReferrer().spec());
  url_request_->set_referrer_policy(request.referrer_policy);
  url_request_->set_upgrade_if_insecure(request.upgrade_if_insecure);

  auto isolation_info = GetIsolationInfo(
      factory_params_->isolation_info,
      factory_params_->automatically_assign_isolation_info, request);
  if (isolation_info)
    url_request_->set_isolation_info(isolation_info.value());

  if (context.ShouldRequireIsolationInfo()) {
    DCHECK(!url_request_->isolation_info().IsEmpty());
  }

  // When a service worker forwards a navigation request it uses the
  // service worker's IsolationInfo.  This causes the cookie code to fail
  // to send SameSite=Lax cookies for main-frame navigations passed through
  // a service worker.  To fix this we check to see if the original destination
  // of the request was a main frame document and then set a flag indicating
  // SameSite cookies should treat it as a main frame navigation.
  if (request.mode == mojom::RequestMode::kNavigate &&
      request.destination == mojom::RequestDestination::kEmpty &&
      request.original_destination == mojom::RequestDestination::kDocument) {
    url_request_->set_force_main_frame_for_same_site_cookies(true);
  }

  if (factory_params_->disable_secure_dns ||
      (request.trusted_params && request.trusted_params->disable_secure_dns)) {
    url_request_->SetSecureDnsPolicy(net::SecureDnsPolicy::kDisable);
  }

  // |cors_exempt_headers| must be merged here to avoid breaking CORS checks.
  // They are non-empty when the values are given by the UA code, therefore
  // they should be ignored by CORS checks.
  net::HttpRequestHeaders merged_headers = request.headers;
  merged_headers.MergeFrom(request.cors_exempt_headers);

  // This should be ensured by the CorsURLLoaderFactory(), which is called
  // before URLLoaders are created.
  DCHECK(AreRequestHeadersSafe(merged_headers));
  url_request_->SetExtraRequestHeaders(merged_headers);

#if defined(__QNX__) || defined(__QNXNTO__)
  BerryMaybeFixYoutubeEmbedReferer(url_request_.get());
#endif

  url_request_->SetUserData(kUserDataKey,
                            std::make_unique<UnownedPointer>(this));
  url_request_->set_accepted_stream_types(
      request.devtools_accepted_stream_types);

  if (request.trusted_params) {
    has_user_activation_ = request.trusted_params->has_user_activation;
    allow_cookies_from_browser_ =
        request.trusted_params->allow_cookies_from_browser;
  }

  // Store any cookies passed from the browser process to later attach them to
  // the request.
  if (allow_cookies_from_browser_) {
    cookies_from_browser_ =
        GetCookiesFromHeaders(request.headers, request.cors_exempt_headers);
  }

  throttling_token_ = network::ScopedThrottlingToken::MaybeCreate(
      url_request_->net_log().source().id, request.throttling_profile_id);

  url_request_->set_initiator(request.request_initiator);

  SetFetchMetadataHeaders(url_request_.get(), request_mode_,
                          has_user_activation_, request_destination_, nullptr,
                          *factory_params_, *origin_access_list_);

  SetAttributionReportingHeaders(*url_request_, request);

  if (request.update_first_party_url_on_redirect) {
    url_request_->set_first_party_url_policy(
        net::RedirectInfo::FirstPartyURLPolicy::UPDATE_URL_ON_REDIRECT);
  }

  int request_load_flags = request.load_flags;

  url_request_->SetLoadFlags(request_load_flags);
  url_request_->SetPriorityIncremental(request.priority_incremental);
  SetRequestCredentials(request.url);

  url_request_->SetRequestHeadersCallback(base::BindRepeating(
      &URLLoader::SetRawRequestHeadersAndNotify, base::Unretained(this)));
  if (shared_dictionary_checker_) {
    url_request_->SetIsSharedDictionaryReadAllowedCallback(base::BindRepeating(
        &URLLoader::IsSharedDictionaryReadAllowed, base::Unretained(this)));
  }

  if (devtools_request_id()) {
    url_request_->SetResponseHeadersCallback(base::BindRepeating(
        &URLLoader::SetRawResponseHeaders, base::Unretained(this)));
  }

  url_request_->SetEarlyResponseHeadersCallback(base::BindRepeating(
      &URLLoader::NotifyEarlyResponse, base::Unretained(this)));

  if (keepalive_ && keepalive_statistics_recorder_) {
    keepalive_statistics_recorder_->OnLoadStarted(
        *factory_params_->top_frame_id, keepalive_request_size_);
  }

  if (request.net_log_reference_info) {
    // Log source object that created the request, if avairable.
    url_request_->net_log().AddEventReferencingSource(
        net::NetLogEventType::CREATED_BY,
        request.net_log_reference_info.value());
  }

  url_request_->set_has_storage_access(request.has_storage_access);

  url_request_->cookie_setting_overrides().PutAll(cookie_setting_overrides);
  if (request.is_outermost_main_frame &&
      network::cors::IsCorsEnabledRequestMode(request_mode_)) {
    url_request_->cookie_setting_overrides().Put(
        net::CookieSettingOverride::kTopLevelStorageAccessGrantEligible);
  }

  AddAdsHeuristicCookieSettingOverrides(
      request.is_ad_tagged, url_request_->cookie_setting_overrides());

  // The `kStorageAccessGrantEligible` override will be applied (in-place) by
  // individual request jobs as appropriate, but should not be present
  // initially.
  DCHECK(!url_request_->cookie_setting_overrides().Has(
      net::CookieSettingOverride::kStorageAccessGrantEligible));

  // Resolve elements from request_body and prepare upload data.
  if (request.request_body.get() && !qnx_skip_request_body_) {
    OpenFilesForUpload(request);
    return;
  }

  ProcessOutboundTrustTokenInterceptor(request);
}

// This class is used to manage the queue of pending file upload operations
// initiated by the URLLoader::OpenFilesForUpload().
class URLLoader::FileOpenerForUpload {
 public:
  typedef base::OnceCallback<void(int, std::vector<base::File>)>
      SetUpUploadCallback;

  FileOpenerForUpload(std::vector<base::FilePath> paths,
                      URLLoader* url_loader,
                      int32_t process_id,
                      mojom::NetworkContextClient* const network_context_client,
                      SetUpUploadCallback set_up_upload_callback)
      : paths_(std::move(paths)),
        url_loader_(url_loader),
        process_id_(process_id),
        network_context_client_(network_context_client),
        set_up_upload_callback_(std::move(set_up_upload_callback)) {
    StartOpeningNextBatch();
  }

  FileOpenerForUpload(const FileOpenerForUpload&) = delete;
  FileOpenerForUpload& operator=(const FileOpenerForUpload&) = delete;

  ~FileOpenerForUpload() {
    if (!opened_files_.empty())
      PostCloseFiles(std::move(opened_files_));
  }

 private:
  static void OnFilesForUploadOpened(
      base::WeakPtr<FileOpenerForUpload> file_opener,
      size_t num_files_requested,
      int error_code,
      std::vector<base::File> opened_files) {
    if (!file_opener) {
      PostCloseFiles(std::move(opened_files));
      return;
    }

    if (error_code == net::OK && num_files_requested != opened_files.size())
      error_code = net::ERR_FAILED;

    if (error_code != net::OK) {
      PostCloseFiles(std::move(opened_files));
      file_opener->FilesForUploadOpenedDone(error_code);
      return;
    }

    for (base::File& file : opened_files)
      file_opener->opened_files_.push_back(std::move(file));

    if (file_opener->opened_files_.size() < file_opener->paths_.size()) {
      file_opener->StartOpeningNextBatch();
      return;
    }

    file_opener->FilesForUploadOpenedDone(net::OK);
  }

  // |opened_files| need to be closed on a blocking task runner, so move the
  // |opened_files| vector onto a sequence that can block so it gets destroyed
  // there.
  static void PostCloseFiles(std::vector<base::File> opened_files) {
    base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
        base::DoNothingWithBoundArgs(std::move(opened_files)));
  }

  void StartOpeningNextBatch() {
    size_t num_files_to_request = std::min(paths_.size() - opened_files_.size(),
                                           kMaxFileUploadRequestsPerBatch);
    std::vector<base::FilePath> batch_paths(
        paths_.begin() + opened_files_.size(),
        paths_.begin() + opened_files_.size() + num_files_to_request);

    network_context_client_->OnFileUploadRequested(
        process_id_, /*async=*/true, batch_paths,
        url_loader_->url_request_->url(),
        base::BindOnce(&FileOpenerForUpload::OnFilesForUploadOpened,
                       weak_ptr_factory_.GetWeakPtr(), num_files_to_request));
  }

  void FilesForUploadOpenedDone(int error_code) {
    url_loader_->url_request_->LogUnblocked();

    if (error_code == net::OK)
      std::move(set_up_upload_callback_).Run(net::OK, std::move(opened_files_));
    else
      std::move(set_up_upload_callback_).Run(error_code, {});
  }

  // The paths of files for upload
  const std::vector<base::FilePath> paths_;
  const raw_ptr<URLLoader> url_loader_;
  const int32_t process_id_;
  const raw_ptr<mojom::NetworkContextClient> network_context_client_;
  SetUpUploadCallback set_up_upload_callback_;
  // The files opened so far.
  std::vector<base::File> opened_files_;

  base::WeakPtrFactory<FileOpenerForUpload> weak_ptr_factory_{this};
};

void URLLoader::OpenFilesForUpload(const ResourceRequest& request) {
  ResourceRequest upload_request = request;
#if defined(__QNX__) || defined(__QNXNTO__)
  BerryMaybeSpoofYoutubeInnertube(&upload_request, url_request_.get());
#endif
  std::vector<base::FilePath> paths;
  for (const auto& element : *upload_request.request_body.get()->elements()) {
    if (element.type() == mojom::DataElementDataView::Tag::kFile) {
      paths.push_back(element.As<network::DataElementFile>().path());
    }
  }
  if (paths.empty()) {
    SetUpUpload(upload_request, net::OK, std::vector<base::File>());
    return;
  }
  if (!network_context_client_) {
    DLOG(ERROR) << "URLLoader couldn't upload a file because no "
                   "NetworkContextClient is set.";
    // Defer calling NotifyCompleted to make sure the URLLoader finishes
    // initializing before getting deleted.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&URLLoader::NotifyCompleted,
                       weak_ptr_factory_.GetWeakPtr(), net::ERR_ACCESS_DENIED));
    return;
  }
  url_request_->LogBlockedBy("Opening Files");
  file_opener_for_upload_ = std::make_unique<FileOpenerForUpload>(
      std::move(paths), this, factory_params_->process_id,
      network_context_client_,
      base::BindOnce(&URLLoader::SetUpUpload, base::Unretained(this),
                     upload_request));
}

void URLLoader::SetUpUpload(const ResourceRequest& request,
                            int error_code,
                            std::vector<base::File> opened_files) {
  if (error_code != net::OK) {
    DCHECK(opened_files.empty());
    // Defer calling NotifyCompleted to make sure the URLLoader finishes
    // initializing before getting deleted.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&URLLoader::NotifyCompleted,
                                  weak_ptr_factory_.GetWeakPtr(), error_code));
    return;
  }
  scoped_refptr<base::SequencedTaskRunner> task_runner =
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE});
  url_request_->set_upload(CreateUploadDataStream(
      request.request_body.get(), opened_files, task_runner.get()));

  if (request.enable_upload_progress) {
    upload_progress_tracker_ = std::make_unique<UploadProgressTracker>(
        FROM_HERE,
        base::BindRepeating(&URLLoader::SendUploadProgress,
                            base::Unretained(this)),
        url_request_.get());
  }
  ProcessOutboundTrustTokenInterceptor(request);
}

void URLLoader::ProcessOutboundSharedStorageInterceptor() {
  DCHECK(shared_storage_request_helper_);
  shared_storage_request_helper_->ProcessOutgoingRequest(*url_request_);
  ScheduleStart();
}

// TODO(https://crbug.com/1410256): Parallelize Private State Tokens and
// Attribution operations.
void URLLoader::ProcessOutboundAttributionInterceptor() {
  if (!attribution_request_helper_) {
    ProcessOutboundSharedStorageInterceptor();
    return;
  }

  attribution_request_helper_->Begin(
      *url_request_,
      base::BindOnce(&URLLoader::ProcessOutboundSharedStorageInterceptor,
                     weak_ptr_factory_.GetWeakPtr()));
}

void URLLoader::ProcessOutboundTrustTokenInterceptor(
    const ResourceRequest& request) {
  if (!request.trust_token_params) {
    ProcessOutboundAttributionInterceptor();
    return;
  }

  // Trust token operations other than signing cannot be served from cache
  // because it needs to send the server the Trust Tokens request header and
  // get the corresponding response header. It is okay to cache the results in
  // case subsequent requests are made to the same URL in non-trust-token
  // settings.
  if (request.trust_token_params->operation !=
      mojom::TrustTokenOperationType::kSigning) {
    url_request_->SetLoadFlags(url_request_->load_flags() |
                               net::LOAD_BYPASS_CACHE);
  }

  // Since the request has trust token parameters, |trust_token_helper_factory_|
  // is guaranteed to be non-null by URLLoader's constructor's contract.
  DCHECK(trust_token_helper_factory_);

  trust_token_helper_factory_->CreateTrustTokenHelperForRequest(
      url_request_->isolation_info().top_frame_origin().value_or(url::Origin()),
      url_request_->extra_request_headers(), request.trust_token_params.value(),
      url_request_->net_log(),
      base::BindOnce(&URLLoader::OnDoneConstructingTrustTokenHelper,
                     weak_ptr_factory_.GetWeakPtr(),
                     request.trust_token_params->operation));
}

void URLLoader::OnDoneConstructingTrustTokenHelper(
    mojom::TrustTokenOperationType operation,
    TrustTokenStatusOrRequestHelper status_or_helper) {
  if (trust_token_observer_) {
    const net::IsolationInfo& isolation_info = url_request_->isolation_info();
    url::Origin top_frame_origin;
    if (isolation_info.top_frame_origin()) {
      top_frame_origin = *isolation_info.top_frame_origin();
    }

    bool token_operation_unauthorized =
        status_or_helper.status() ==
        mojom::TrustTokenOperationStatus::kUnauthorized;
    switch (operation) {
      case mojom::TrustTokenOperationType::kIssuance:
        trust_token_observer_->OnTrustTokensAccessed(
            mojom::TrustTokenAccessDetails::NewIssuance(
                mojom::TrustTokenIssuanceDetails::New(
                    top_frame_origin, url::Origin::Create(url_request_->url()),
                    token_operation_unauthorized)));
        break;
      case mojom::TrustTokenOperationType::kRedemption:
        trust_token_observer_->OnTrustTokensAccessed(
            mojom::TrustTokenAccessDetails::NewRedemption(
                mojom::TrustTokenRedemptionDetails::New(
                    top_frame_origin, url::Origin::Create(url_request_->url()),
                    token_operation_unauthorized)));
        break;
      case mojom::TrustTokenOperationType::kSigning:
        trust_token_observer_->OnTrustTokensAccessed(
            mojom::TrustTokenAccessDetails::NewSigning(
                mojom::TrustTokenSigningDetails::New(
                    top_frame_origin, token_operation_unauthorized)));
        break;
    }
  }

  if (!status_or_helper.ok()) {
    trust_token_status_ = status_or_helper.status();

    // Defer calling NotifyCompleted to make sure the URLLoader
    // finishes initializing before getting deleted.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&URLLoader::NotifyCompleted,
                                  weak_ptr_factory_.GetWeakPtr(),
                                  net::ERR_TRUST_TOKEN_OPERATION_FAILED));

    if (devtools_observer_ && devtools_request_id()) {
      mojom::TrustTokenOperationResultPtr operation_result =
          mojom::TrustTokenOperationResult::New();
      operation_result->status = *trust_token_status_;
      operation_result->operation = operation;
      devtools_observer_->OnTrustTokenOperationDone(
          devtools_request_id().value(), std::move(operation_result));
    }
    return;
  }

  trust_token_helper_ = status_or_helper.TakeOrCrash();
  trust_token_helper_->Begin(
      url_request_->url(),
      base::BindOnce(&URLLoader::OnDoneBeginningTrustTokenOperation,
                     weak_ptr_factory_.GetWeakPtr()));
}

void URLLoader::OnDoneBeginningTrustTokenOperation(
    absl::optional<net::HttpRequestHeaders> headers,
    mojom::TrustTokenOperationStatus status) {
  trust_token_status_ = status;

  // In case the operation failed or it succeeded in a manner where the request
  // does not need to be sent onwards, the DevTools event is emitted from here.
  // Otherwise the DevTools event is always emitted from
  // |OnDoneFinalizingTrustTokenOperation|.
  if (status != mojom::TrustTokenOperationStatus::kOk) {
    DCHECK(!headers);
    MaybeSendTrustTokenOperationResultToDevTools();
  }

  if (status == mojom::TrustTokenOperationStatus::kOk) {
    DCHECK(headers);
    for (const auto& header_pair : headers->GetHeaderVector()) {
      url_request_->SetExtraRequestHeaderByName(
          header_pair.key, header_pair.value, /*overwrite=*/true);
    }

    ProcessOutboundAttributionInterceptor();
  } else if (status == mojom::TrustTokenOperationStatus::kAlreadyExists ||
             status == mojom::TrustTokenOperationStatus::
                           kOperationSuccessfullyFulfilledLocally) {
    // The Trust Tokens operation succeeded without needing to send the request;
    // we return early with an "error" representing this success.
    //
    // Here and below, defer calling NotifyCompleted to make sure the URLLoader
    // finishes initializing before getting deleted.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            &URLLoader::NotifyCompleted, weak_ptr_factory_.GetWeakPtr(),
            net::ERR_TRUST_TOKEN_OPERATION_SUCCESS_WITHOUT_SENDING_REQUEST));
  } else {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&URLLoader::NotifyCompleted,
                                  weak_ptr_factory_.GetWeakPtr(),
                                  net::ERR_TRUST_TOKEN_OPERATION_FAILED));
  }
}

void URLLoader::ScheduleStart() {
  QNX_TRACE_FMT("QNX:UL:SchedStart url=%s\n",
                 url_request_ ? url_request_->url().spec().c_str() : "null");
#if defined(__QNX__) || defined(__QNXNTO__)
  if (resource_type_ == 0) {
    if (url_request_) {
      BerryMaybeMarkWatchShimJsAlive(url_request_->url());
      if (!BerryUrlIsWatchShimTarget(url_request_->url()) &&
          !BerryUrlIsSearchShimTarget(url_request_->url()) &&
          !BerryUrlIsGoogleSorryBounceTarget(url_request_->url()))
        BerryCancelWatchShimJsAliveTripwire();
    }
    const char* url_log_ptr = "null";
    std::string url_log_storage;
    if (url_request_) {
      url_log_storage = url_request_->url().spec().substr(0, 120);
      url_log_ptr = url_log_storage.c_str();
    }
    QNX_NAV_LOG_FMT(
        "BerryNav: URLReqStart loader=%p url=\"%s\" ms=%lld abs=%lld\n",
        this, url_log_ptr,
        url_request_ && url_request_->creation_time().is_null() == false
            ? static_cast<long long>(
                  (base::TimeTicks::Now() - url_request_->creation_time())
                      .InMilliseconds())
            : -1LL,
        base::QnxNowMs());
  }
#endif
  bool defer = false;
  if (resource_scheduler_client_) {
    resource_scheduler_request_handle_ =
        resource_scheduler_client_->ScheduleRequest(
            !(options_ & mojom::kURLLoadOptionSynchronous), url_request_.get());
    resource_scheduler_request_handle_->set_resume_callback(
        base::BindOnce(&URLLoader::ResumeStart, base::Unretained(this)));
    resource_scheduler_request_handle_->WillStartRequest(&defer);
#if defined(__QNX__) || defined(__QNXNTO__)
    // Main-frame navigations must not sit behind subresource throttling on a
    // single-core-class device; deferral showed up as multi-second TTFB gaps.
    if (resource_type_ == 0)  // blink::mojom::ResourceType::kMainFrame
      defer = false;
#endif
  }
  if (defer) {
    url_request_->LogBlockedBy("ResourceScheduler");
    QNX_TRACE_MSG("QNX:UL:Deferred!\n");
  } else {
#if defined(__QNX__) || defined(__QNXNTO__)
    if (BerryShouldUseYoutubeWatchShim(url_request_.get(), resource_type_)) {
      QnxPrepareYoutubeWatchShim();
    } else if (BerryShouldUseYoutubeSearchShim(url_request_.get(),
                                               resource_type_)) {
      QnxPrepareYoutubeSearchShim();
    } else if (BerryShouldUseGoogleSorryBounce(url_request_.get(),
                                               resource_type_)) {
      QnxPrepareGoogleSorryBounce();
    } else if (BerryShouldUseYoutubeHomeBounce(url_request_.get(),
                                               resource_type_)) {
      QnxPrepareYoutubeHomeBounce();
    }
#endif
    QNX_TRACE_MSG("QNX:UL:Starting!\n");
    url_request_->Start();
  }
}

URLLoader::~URLLoader() {
  TRACE_EVENT("loading", "URLLoader::~URLLoader",
              perfetto::TerminatingFlow::FromPointer(this));
  if (keepalive_ && keepalive_statistics_recorder_) {
    keepalive_statistics_recorder_->OnLoadFinished(
        *factory_params_->top_frame_id, keepalive_request_size_);
  }

  if (!cookie_access_details_.empty()) {
    // In case the response wasn't received successfully sent the call now.
    // Note `cookie_observer_` is guaranteed non-null since
    // `cookie_access_details_` is only appended to when it is valid.
    cookie_observer_->OnCookiesAccessed(std::move(cookie_access_details_));
  }
}

// static
const void* const URLLoader::kUserDataKey = &URLLoader::kUserDataKey;

void URLLoader::FollowRedirect(
    const std::vector<std::string>& removed_headers,
    const net::HttpRequestHeaders& modified_headers,
    const net::HttpRequestHeaders& modified_cors_exempt_headers,
    const absl::optional<GURL>& new_url) {
  if (!deferred_redirect_url_) {
    NOTREACHED();
    return;
  }

  // Set seen_raw_request_headers_ to false in order to make sure this redirect
  // also calls the devtools observer.
  seen_raw_request_headers_ = false;

  memory_cache_writer_.reset();

  // Removing headers can't make the set of pre-existing headers unsafe, but
  // adding headers can.
  if (!AreRequestHeadersSafe(modified_headers) ||
      !AreRequestHeadersSafe(modified_cors_exempt_headers)) {
    NotifyCompleted(net::ERR_INVALID_ARGUMENT);
    // |this| may have been deleted.
    return;
  }

  // Store any cookies passed from the browser process to later attach them to
  // the request.
  if (allow_cookies_from_browser_) {
    cookies_from_browser_ =
        GetCookiesFromHeaders(modified_headers, modified_cors_exempt_headers);
  }

  // Reset the state of the PNA checker - redirects should be treated like new
  // requests by the same client.
  if (new_url.has_value()) {
    private_network_access_checker_.ResetForRedirect(*new_url);
  } else {
    private_network_access_checker_.ResetForRedirect(*deferred_redirect_url_);
  }

  // Propagate removal or restoration of shared storage eligiblity to the helper
  // if the "Sec-Shared-Storage-Writable" request header has been removed or
  // restored.
  DCHECK(shared_storage_request_helper_);
  shared_storage_request_helper_->UpdateSharedStorageWritableEligible(
      removed_headers, modified_headers);

  deferred_redirect_url_.reset();
  new_redirect_url_ = new_url;

  net::HttpRequestHeaders merged_modified_headers = modified_headers;
  merged_modified_headers.MergeFrom(modified_cors_exempt_headers);
  url_request_->FollowDeferredRedirect(removed_headers,
                                       merged_modified_headers);
  new_redirect_url_.reset();
}

void URLLoader::SetPriority(net::RequestPriority priority,
                            int32_t intra_priority_value) {
  if (url_request_ && resource_scheduler_client_) {
    resource_scheduler_client_->ReprioritizeRequest(
        url_request_.get(), priority, intra_priority_value);
  }
}

void URLLoader::PauseReadingBodyFromNet() {
  DVLOG(1) << "URLLoader pauses fetching response body for "
           << (url_request_ ? url_request_->original_url().spec()
                            : "a URL that has completed loading or failed.");

  // Please note that we pause reading body in all cases. Even if the URL
  // request indicates that the response was cached, there could still be
  // network activity involved. For example, the response was only partially
  // cached.
  should_pause_reading_body_ = true;
}

void URLLoader::ResumeReadingBodyFromNet() {
  DVLOG(1) << "URLLoader resumes fetching response body for "
           << (url_request_ ? url_request_->original_url().spec()
                            : "a URL that has completed loading or failed.");
  should_pause_reading_body_ = false;

  if (paused_reading_body_) {
    paused_reading_body_ = false;
    ReadMore();
  }
}

PrivateNetworkAccessCheckResult URLLoader::PrivateNetworkAccessCheck(
    const net::TransportInfo& transport_info) {
  PrivateNetworkAccessCheckResult result =
      private_network_access_checker_.Check(transport_info);

  mojom::IPAddressSpace response_address_space =
      *private_network_access_checker_.ResponseAddressSpace();

  url_request_->net_log().AddEvent(
      net::NetLogEventType::PRIVATE_NETWORK_ACCESS_CHECK, [&] {
        return base::Value::Dict()
            .Set("client_address_space",
                 IPAddressSpaceToStringPiece(
                     private_network_access_checker_.ClientAddressSpace()))
            .Set("resource_address_space",
                 IPAddressSpaceToStringPiece(response_address_space))
            .Set("result",
                 PrivateNetworkAccessCheckResultToStringPiece(result));
      });

  bool is_warning = false;
  switch (result) {
    case PrivateNetworkAccessCheckResult::kAllowedByPolicyWarn:
      is_warning = true;
      break;
    case PrivateNetworkAccessCheckResult::kBlockedByPolicyBlock:
      is_warning = false;
      break;
    default:
      // Do not report anything to DevTools in these cases.
      return result;
  }

  // If `security_state` was nullptr, then `result` should not have mentioned
  // the policy set in `security_state->private_network_request_policy`.
  const mojom::ClientSecurityState* security_state =
      private_network_access_checker_.client_security_state();
  DCHECK(security_state);

  if (devtools_observer_) {
    devtools_observer_->OnPrivateNetworkRequest(
        devtools_request_id(), url_request_->url(), is_warning,
        response_address_space, security_state->Clone());
  }

  return result;
}

int URLLoader::OnConnected(net::URLRequest* url_request,
                           const net::TransportInfo& info,
                           net::CompletionOnceCallback callback) {
  DCHECK_EQ(url_request, url_request_.get());
  transport_info_ = info;

  // Now that the request endpoint's address has been resolved, check if
  // this request should be blocked per Private Network Access.
  PrivateNetworkAccessCheckResult result = PrivateNetworkAccessCheck(info);
  absl::optional<mojom::CorsError> cors_error =
      PrivateNetworkAccessCheckResultToCorsError(result);
  if (cors_error.has_value()) {
    if (result == PrivateNetworkAccessCheckResult::kBlockedByPolicyBlock &&
        (info.type == net::TransportType::kCached ||
         info.type == net::TransportType::kCachedFromProxy)) {
      // If the cached entry was blocked by the private network access check
      // without a preflight, we'll start over and attempt to request from the
      // network, so resetting the checker.
      private_network_access_checker_.ResetForRetry();
      return net::
          ERR_CACHED_IP_ADDRESS_SPACE_BLOCKED_BY_PRIVATE_NETWORK_ACCESS_POLICY;
    }
    // Remember the CORS error so we can annotate the URLLoaderCompletionStatus
    // with it later, then fail the request with the same net error code as
    // other CORS errors.
    cors_error_status_ = CorsErrorStatus(
        *cors_error, private_network_access_checker_.TargetAddressSpace(),
        *private_network_access_checker_.ResponseAddressSpace());
    if (result == PrivateNetworkAccessCheckResult::
                      kBlockedByInconsistentIpAddressSpace ||
        result ==
            PrivateNetworkAccessCheckResult::kBlockedByTargetIpAddressSpace) {
      return net::ERR_INCONSISTENT_IP_ADDRESS_SPACE;
    }
    return net::ERR_FAILED;
  }

  if (!accept_ch_frame_observer_ || info.accept_ch_frame.empty() ||
      !base::FeatureList::IsEnabled(features::kAcceptCHFrame)) {
    return net::OK;
  }

  // Find client hints that are in the ACCEPT_CH frame that were not already
  // included in the request
  std::vector<mojom::WebClientHintsType> hints = ComputeAcceptCHFrameHints(
      info.accept_ch_frame, url_request->extra_request_headers());

  // If there are hints in the ACCEPT_CH frame that weren't included in the
  // original request, notify the observer. If those hints can be included,
  // this URLLoader will be destroyed and another with the correct hints
  // started. Otherwise, the callback to continue the network transaction will
  // be called and the URLLoader will continue as normal.
  if (!hints.empty()) {
    accept_ch_frame_observer_->OnAcceptCHFrameReceived(
        url::Origin::Create(url_request->url()), hints, std::move(callback));
    return net::ERR_IO_PENDING;
  }

  return net::OK;
}

mojom::URLResponseHeadPtr URLLoader::BuildResponseHead() const {
  auto response = mojom::URLResponseHead::New();

  response->request_time = url_request_->request_time();
  response->response_time = url_request_->response_time();
  response->headers = url_request_->response_headers();
  response->parsed_headers =
      PopulateParsedHeaders(response->headers.get(), url_request_->url());

  url_request_->GetCharset(&response->charset);
  response->content_length = url_request_->GetExpectedContentSize();
  url_request_->GetMimeType(&response->mime_type);
  net::HttpResponseInfo response_info = url_request_->response_info();
  response->was_fetched_via_spdy = response_info.was_fetched_via_spdy;
  response->was_alpn_negotiated = response_info.was_alpn_negotiated;
  response->alpn_negotiated_protocol = response_info.alpn_negotiated_protocol;
  response->alternate_protocol_usage = response_info.alternate_protocol_usage;
  response->connection_info = response_info.connection_info;
  response->remote_endpoint = response_info.remote_endpoint;
  response->was_fetched_via_cache = url_request_->was_cached();
  response->is_validated = (response_info.cache_entry_status ==
                            net::HttpResponseInfo::ENTRY_VALIDATED);
  response->proxy_server = url_request_->proxy_chain().proxy_server();
  response->network_accessed = response_info.network_accessed;
  response->async_revalidation_requested =
      response_info.async_revalidation_requested;
  response->was_in_prefetch_cache =
      !(url_request_->load_flags() & net::LOAD_PREFETCH) &&
      response_info.unused_since_prefetch;
  response->did_use_shared_dictionary = response_info.did_use_shared_dictionary;

  response->was_cookie_in_request = false;
  for (const auto& cookie_with_access_result :
       url_request_->maybe_sent_cookies()) {
    if (cookie_with_access_result.access_result.status.IsInclude()) {
      // IsInclude() true means the cookie was sent.
      response->was_cookie_in_request = true;
      break;
    }
  }

  if (is_load_timing_enabled_)
    url_request_->GetLoadTimingInfo(&response->load_timing);

  if (url_request_->ssl_info().cert.get()) {
    response->ct_policy_compliance =
        url_request_->ssl_info().ct_policy_compliance;
    response->cert_status = url_request_->ssl_info().cert_status;
    if ((options_ & mojom::kURLLoadOptionSendSSLInfoWithResponse) ||
        (net::IsCertStatusError(url_request_->ssl_info().cert_status) &&
         (options_ & mojom::kURLLoadOptionSendSSLInfoForCertificateError))) {
      response->ssl_info = url_request_->ssl_info();
    }
  }

  response->request_start = url_request_->creation_time();
  response->response_start = base::TimeTicks::Now();
  response->encoded_data_length = url_request_->GetTotalReceivedBytes();
  response->auth_challenge_info = url_request_->auth_challenge_info();
  response->has_range_requested =
      url_request_->extra_request_headers().HasHeader(
          net::HttpRequestHeaders::kRange);
  base::ranges::copy(url_request_->response_info().dns_aliases,
                     std::back_inserter(response->dns_aliases));
  // [spec]: https://fetch.spec.whatwg.org/#http-network-or-cache-fetch
  // 13. Set response’s request-includes-credentials to includeCredentials.
  response->request_include_credentials = url_request_->allow_credentials();

  response->response_address_space =
      private_network_access_checker_.ResponseAddressSpace().value_or(
          mojom::IPAddressSpace::kUnknown);
  response->client_address_space =
      private_network_access_checker_.ClientAddressSpace();

  return response;
}

void URLLoader::OnReceivedRedirect(net::URLRequest* url_request,
                                   const net::RedirectInfo& redirect_info,
                                   bool* defer_redirect) {
  DCHECK(url_request == url_request_.get());

  DCHECK(!deferred_redirect_url_);
  deferred_redirect_url_ = std::make_unique<GURL>(redirect_info.new_url);

  // Send the redirect response to the client, allowing them to inspect it and
  // optionally follow the redirect.
  *defer_redirect = true;

  mojom::URLResponseHeadPtr response = BuildResponseHead();
  DispatchOnRawResponse();
  ReportFlaggedResponseCookies(false);

  if (memory_cache_)
    memory_cache_->OnRedirect(url_request_.get(), request_destination_);

  const CrossOriginEmbedderPolicy kEmpty;
  // Enforce the Cross-Origin-Resource-Policy (CORP) header.
  const CrossOriginEmbedderPolicy& cross_origin_embedder_policy =
      factory_params_->client_security_state
          ? factory_params_->client_security_state->cross_origin_embedder_policy
          : kEmpty;

  if (absl::optional<mojom::BlockedByResponseReason> blocked_reason =
          CrossOriginResourcePolicy::IsBlocked(
              url_request_->url(), url_request_->original_url(),
              url_request_->initiator(), *response, request_mode_,
              request_destination_, cross_origin_embedder_policy,
              coep_reporter_)) {
    CompleteBlockedResponse(net::ERR_BLOCKED_BY_RESPONSE, false,
                            blocked_reason);
    // TODO(https://crbug.com/1154250):  Close the socket here.
    // For more details see https://crbug.com/1154250#c17.
    // Item 2 discusses redirect handling.
    //
    // "url_request_->AbortAndCloseConnection()" should ideally close the
    // socket, but unfortunately, URLRequestHttpJob caches redirects in a way
    // that ignores their response bodies, since they'll never be read. It does
    // this by calling HttpCache::Transaction::StopCaching(), which also has the
    // effect of detaching the HttpNetworkTransaction, which owns the socket,
    // from the HttpCache::Transaction. To fix this, we'd either need to call
    // StopCaching() later in the process, or make the HttpCache::Transaction
    // continue to hang onto the HttpNetworkTransaction after this call.
    DeleteSelf();
    return;
  }

  SetRequestCredentials(redirect_info.new_url);

  // Clear the Cookie header to ensure that cookies passed in through the
  // `ResourceRequest` do not persist across redirects.
  url_request_.get()->RemoveRequestHeaderByName(
      net::HttpRequestHeaders::kCookie);
  cookies_from_browser_.clear();

  // We may need to clear out old Sec- prefixed request headers. We'll attempt
  // to do this before we re-add any.
  MaybeRemoveSecHeaders(url_request_.get(), redirect_info.new_url);
  SetFetchMetadataHeaders(url_request_.get(), request_mode_,
                          has_user_activation_, request_destination_,
                          &redirect_info.new_url, *factory_params_,
                          *origin_access_list_);

  DCHECK_EQ(emitted_devtools_raw_request_, emitted_devtools_raw_response_);
  response->emitted_extra_info = emitted_devtools_raw_request_;

  ProcessInboundAttributionInterceptorOnReceivedRedirect(redirect_info,
                                                         std::move(response));
}

void URLLoader::ProcessInboundSharedStorageInterceptorOnReceivedRedirect(
    const ::net::RedirectInfo& redirect_info,
    mojom::URLResponseHeadPtr response) {
  DCHECK(shared_storage_request_helper_);
  uint64_t response_index = next_on_receive_redirect_response_index_++;
  on_receive_redirect_responses_[response_index] = std::move(response);
  if (!shared_storage_request_helper_->ProcessIncomingResponse(
          *url_request_, base::BindOnce(&URLLoader::ContinueOnReceiveRedirect,
                                        weak_ptr_factory_.GetWeakPtr(),
                                        redirect_info, response_index))) {
    ContinueOnReceiveRedirect(redirect_info, response_index);
  }
}

void URLLoader::ProcessInboundAttributionInterceptorOnReceivedRedirect(
    const net::RedirectInfo& redirect_info,
    mojom::URLResponseHeadPtr response) {
  if (!attribution_request_helper_) {
    ProcessInboundSharedStorageInterceptorOnReceivedRedirect(
        redirect_info, std::move(response));
    return;
  }

  attribution_request_helper_->OnReceiveRedirect(
      *url_request_, std::move(response), redirect_info,
      base::BindOnce(
          &URLLoader::ProcessInboundSharedStorageInterceptorOnReceivedRedirect,
          weak_ptr_factory_.GetWeakPtr(), redirect_info));
}

void URLLoader::ContinueOnReceiveRedirect(
    const net::RedirectInfo& redirect_info,
    uint64_t response_index) {
  auto iter = on_receive_redirect_responses_.find(response_index);
  DCHECK(iter != on_receive_redirect_responses_.end());
  mojom::URLResponseHeadPtr response = std::move(iter->second);
  DCHECK(response);
  on_receive_redirect_responses_.erase(iter);
  url_loader_client_.Get()->OnReceiveRedirect(redirect_info,
                                              std::move(response));
}

// static
bool URLLoader::HasFetchStreamingUploadBody(const ResourceRequest* request) {
  const ResourceRequestBody* request_body = request->request_body.get();
  if (!request_body)
    return false;
  const std::vector<DataElement>* elements = request_body->elements();
  if (elements->size() != 1u)
    return false;
  const auto& element = elements->front();
  return element.type() == mojom::DataElementDataView::Tag::kChunkedDataPipe &&
         element.As<network::DataElementChunkedDataPipe>().read_only_once();
}

// static
absl::optional<net::IsolationInfo> URLLoader::GetIsolationInfo(
    const net::IsolationInfo& factory_isolation_info,
    bool automatically_assign_isolation_info,
    const ResourceRequest& request) {
  if (!factory_isolation_info.IsEmpty())
    return factory_isolation_info;

  if (request.trusted_params &&
      !request.trusted_params->isolation_info.IsEmpty()) {
    if (request.credentials_mode != network::mojom::CredentialsMode::kOmit) {
      DCHECK(request.trusted_params->isolation_info.site_for_cookies()
                 .IsEquivalent(request.site_for_cookies));
    }
    return request.trusted_params->isolation_info;
  }

  if (automatically_assign_isolation_info) {
    url::Origin origin = url::Origin::Create(request.url);
    return net::IsolationInfo::Create(net::IsolationInfo::RequestType::kOther,
                                      origin, origin, net::SiteForCookies());
  }

  return absl::nullopt;
}

void URLLoader::OnAuthRequired(net::URLRequest* url_request,
                               const net::AuthChallengeInfo& auth_info) {
  if (has_fetch_streaming_upload_body_) {
    NotifyCompleted(net::ERR_FAILED);
    // |this| may have been deleted.
    return;
  }
  if (!url_loader_network_observer_) {
    OnAuthCredentials(absl::nullopt);
    return;
  }

  if (do_not_prompt_for_login_) {
    OnAuthCredentials(absl::nullopt);
    return;
  }

  DCHECK(!auth_challenge_responder_receiver_.is_bound());

  url_loader_network_observer_->OnAuthRequired(
      fetch_window_id_, request_id_, url_request_->url(), first_auth_attempt_,
      auth_info, url_request->response_headers(),
      auth_challenge_responder_receiver_.BindNewPipeAndPassRemote());

  auth_challenge_responder_receiver_.set_disconnect_handler(
      base::BindOnce(&URLLoader::DeleteSelf, base::Unretained(this)));

  first_auth_attempt_ = false;
}

void URLLoader::OnCertificateRequested(net::URLRequest* unused,
                                       net::SSLCertRequestInfo* cert_info) {
  DCHECK(!client_cert_responder_receiver_.is_bound());

  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kIgnoreUrlFetcherCertRequests) &&
      factory_params_->is_trusted) {
    ContinueWithoutCertificate();
    return;
  }

  if (!url_loader_network_observer_) {
    CancelRequest();
    return;
  }

  // Set up mojo endpoints for ClientCertificateResponder and bind to the
  // Receiver. This enables us to receive messages regarding the client
  // certificate selection.
  url_loader_network_observer_->OnCertificateRequested(
      fetch_window_id_, cert_info,
      client_cert_responder_receiver_.BindNewPipeAndPassRemote());
  client_cert_responder_receiver_.set_disconnect_handler(
      base::BindOnce(&URLLoader::CancelRequest, base::Unretained(this)));
}

void URLLoader::OnSSLCertificateError(net::URLRequest* request,
                                      int net_error,
                                      const net::SSLInfo& ssl_info,
                                      bool fatal) {
  if (!url_loader_network_observer_) {
    OnSSLCertificateErrorResponse(ssl_info, net_error);
    return;
  }
  url_loader_network_observer_->OnSSLCertificateError(
      url_request_->url(), net_error, ssl_info, fatal,
      base::BindOnce(&URLLoader::OnSSLCertificateErrorResponse,
                     weak_ptr_factory_.GetWeakPtr(), ssl_info));
}

void URLLoader::ProcessInboundSharedStorageInterceptorOnResponseStarted() {
  DCHECK(shared_storage_request_helper_);
  if (!shared_storage_request_helper_->ProcessIncomingResponse(
          *url_request_, base::BindOnce(&URLLoader::ContinueOnResponseStarted,
                                        weak_ptr_factory_.GetWeakPtr()))) {
    ContinueOnResponseStarted();
  }
}

void URLLoader::ProcessInboundAttributionInterceptorOnResponseStarted() {
  if (!attribution_request_helper_) {
    ProcessInboundSharedStorageInterceptorOnResponseStarted();
    return;
  }

  attribution_request_helper_->Finalize(
      *response_,
      base::BindOnce(
          &URLLoader::ProcessInboundSharedStorageInterceptorOnResponseStarted,
          weak_ptr_factory_.GetWeakPtr()));
}

void URLLoader::OnResponseStarted(net::URLRequest* url_request, int net_error) {
#if defined(__QNX__)
  QNX_TRACE_FMT("QNX:UL:OnRespStarted err=%d\n", net_error);
  if (resource_type_ == 0) {
    QNX_NAV_LOG_FMT("BerryNav: IOReqDone err=%d abs=%lld\n", net_error,
                    base::QnxNowMs());
  }
#endif
  DCHECK(url_request == url_request_.get());
  has_received_response_ = true;

  // Use `true` to force sending the cookie accessed update now. This is because
  // for navigations the CookieObserver might get torn down by the time the
  // request completes.
  ReportFlaggedResponseCookies(true);

  if (net_error != net::OK) {
    NotifyCompleted(net_error);
    // |this| may have been deleted.
    return;
  }

  response_ = BuildResponseHead();
#if defined(__QNX__) || defined(__QNXNTO__)
  if (qnx_youtube_watch_shim_buffer_ && response_ && response_->headers) {
    const int http_code = response_->headers->response_code();
    const bool code_ok =
        qnx_google_sorry_bounce_
            ? (http_code == 429 || http_code == 200 || http_code == 403)
            : (http_code == 200);
    if (code_ok && !qnx_youtube_watch_shim_html_.empty()) {
      if (qnx_google_sorry_bounce_) {
        BerryScrubSorryBounceResponseHeaders(response_.get(),
                                             qnx_youtube_watch_shim_html_.size(),
                                             url_request_->url());
        QNX_NAV_LOG_FMT(
            "BerryNav: SorryBounce head scrubbed loader=%p code=%d shim_bytes=%zu "
            "url=\"%.80s\"\n",
            this, http_code, qnx_youtube_watch_shim_html_.size(),
            url_request_->url().spec().c_str());
      } else {
        BerryScrubWatchShimResponseHeaders(response_.get(),
                                           qnx_youtube_watch_shim_html_.size(),
                                           url_request_->url());
        const size_t csp_count =
            response_->parsed_headers
                ? response_->parsed_headers->content_security_policy.size()
                : 0;
        QNX_NAV_LOG_FMT(
            "BerryNav: WatchShim head scrubbed loader=%p code=%d shim_bytes=%zu "
            "csp=%zu\n",
            this, http_code, qnx_youtube_watch_shim_html_.size(), csp_count);
      }
      qnx_watch_shim_scrubbed_ = true;
      qnx_watch_shim_response_ready_ = true;
    } else {
      QNX_NAV_LOG_FMT(
          "BerryNav: %s head skip loader=%p code=%d html=%zu\n",
          qnx_google_sorry_bounce_ ? "SorryBounce" : "WatchShim", this,
          http_code, qnx_youtube_watch_shim_html_.size());
      qnx_youtube_watch_shim_buffer_ = false;
      qnx_youtube_watch_shim_active_ = false;
      qnx_google_sorry_bounce_ = false;
      qnx_youtube_watch_shim_html_.clear();
    }
  }
#endif
  DispatchOnRawResponse();

  // Parse and remove the Trust Tokens response headers, if any are expected,
  // potentially failing the request if an error occurs.
  if (response_ && response_->headers && trust_token_helper_) {
    DCHECK(response_);
    trust_token_helper_->Finalize(
        *response_->headers.get(),
        base::BindOnce(&URLLoader::OnDoneFinalizingTrustTokenOperation,
                       weak_ptr_factory_.GetWeakPtr()));
    // |this| may have been deleted.
    return;
  }

  if (memory_cache_) {
    memory_cache_writer_ = memory_cache_->MaybeCreateWriter(
        url_request_.get(), request_destination_, transport_info_, response_);
  }

  ProcessInboundAttributionInterceptorOnResponseStarted();
}

void URLLoader::OnDoneFinalizingTrustTokenOperation(
    mojom::TrustTokenOperationStatus status) {
  trust_token_status_ = status;

  MaybeSendTrustTokenOperationResultToDevTools();

  if (status != mojom::TrustTokenOperationStatus::kOk) {
    NotifyCompleted(net::ERR_TRUST_TOKEN_OPERATION_FAILED);
    // |this| may have been deleted.
    return;
  }

  ProcessInboundAttributionInterceptorOnResponseStarted();
}

void URLLoader::MaybeSendTrustTokenOperationResultToDevTools() {
  CHECK(trust_token_helper_ && trust_token_status_);

  if (!devtools_observer_ || !devtools_request_id())
    return;

  mojom::TrustTokenOperationResultPtr operation_result =
      trust_token_helper_->CollectOperationResultWithStatus(
          *trust_token_status_);
  devtools_observer_->OnTrustTokenOperationDone(devtools_request_id().value(),
                                                std::move(operation_result));
}

bool BerryPlayerResponseLooksLikeError(const std::string& body) {
  if (body.size() < 4096)
    return true;
  if (body.find("streamingData") == std::string::npos &&
      body.find("playabilityStatus") == std::string::npos)
    return true;
  if (body.find("\"status\":\"ERROR\"") != std::string::npos ||
      body.find("\"status\":\"UNPLAYABLE\"") != std::string::npos ||
      body.find("\"status\":\"LOGIN_REQUIRED\"") != std::string::npos)
    return true;
  return false;
}

void BerryLogPlayerFormatSummary(const std::string& body) {
  if (body.empty())
    return;
  size_t itag18 = 0;
  size_t progressive = 0;
  size_t pos = 0;
  while ((pos = body.find("\"itag\":", pos)) != std::string::npos) {
    pos += 7;
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t'))
      ++pos;
    if (pos + 2 < body.size() && body.compare(pos, 2, "18") == 0 &&
        (body[pos + 2] == ',' || body[pos + 2] == '}'))
      ++itag18;
  }
  pos = 0;
  while ((pos = body.find("\"url\":\"https://", pos)) != std::string::npos) {
    ++progressive;
    pos += 6;
  }
  QNX_NAV_LOG_FMT(
      "BerryNav: PlayerFormats itag18=%zu progressive_urls=%zu bytes=%zu\n",
      itag18, progressive, body.size());
}

#if defined(__QNX__) || defined(__QNXNTO__)
bool URLLoader::QnxWriteBodyToNewDataPipe(const std::string& body,
                                          const char* log_tag) {
  if (body.empty())
    return false;

  MojoCreateDataPipeOptions options;
  options.struct_size = sizeof(MojoCreateDataPipeOptions);
  options.flags = MOJO_CREATE_DATA_PIPE_FLAG_NONE;
  options.element_num_bytes = 1;
  const size_t body_len = body.size();
  options.capacity_num_bytes = static_cast<uint32_t>(
      std::max(body_len + 4096,
               static_cast<size_t>(network::features::GetDataPipeDefaultAllocationSize(
                   features::DataPipeAllocationSize::kLargerSizeIfPossible))));
  if (mojo::CreateDataPipe(&options, response_body_stream_, consumer_handle_) !=
      MOJO_RESULT_OK) {
    QNX_NAV_LOG_FMT("BerryNav: %s pipe_fail loader=%p\n", log_tag, this);
    return false;
  }

  size_t offset = 0;
  while (offset < body.size() && response_body_stream_.is_valid()) {
    uint32_t num_bytes = static_cast<uint32_t>(body.size() - offset);
    MojoResult result = response_body_stream_->WriteData(
        body.data() + offset, &num_bytes, MOJO_WRITE_DATA_FLAG_NONE);
    if (result == MOJO_RESULT_OK) {
      offset += num_bytes;
      continue;
    }
    if (result == MOJO_RESULT_SHOULD_WAIT) {
      base::PlatformThread::Sleep(base::Milliseconds(2));
      continue;
    }
    QNX_NAV_LOG_FMT("BerryNav: %s partial loader=%p wrote=%zu/%zu\n", log_tag,
                    this, offset, body.size());
    break;
  }
  total_written_bytes_ = offset;
  if (response_)
    response_->content_length = static_cast<int64_t>(body.size());
  response_body_stream_.reset();
  const char* url_snip = url_request_ ? url_request_->url().spec().c_str()
                                      : "(null-req)";
  QNX_NAV_LOG_FMT("BerryNav: %s loader=%p bytes=%zu url=\"%.80s\"\n", log_tag,
                  this, body.size(), url_snip);
  return offset == body.size();
}

void URLLoader::QnxFlushBufferedYoutubePlayerBody() {
  if (qnx_youtube_player_body_.empty())
    return;
  BerryCacheVisitorDataFromPlayerResponse(qnx_youtube_player_body_);
  const std::string original = qnx_youtube_player_body_;
  if (BerryPlayerResponseLooksLikeError(original)) {
    QNX_NAV_LOG_FMT(
        "BerryNav: PlayerResponseError skip_strip bytes=%zu preview=\"%.200s\" "
        "url=\"%.80s\"\n",
        original.size(), original.c_str(), url_request_->url().spec().c_str());
  } else {
    BerryStripSabrFromYoutubePlayerResponse(url_request_->url(),
                                          &qnx_youtube_player_body_);
    if (BerryPlayerResponseLooksLikeError(qnx_youtube_player_body_)) {
      qnx_youtube_player_body_ = original;
      QNX_NAV_LOG_FMT(
          "BerryNav: PlayerResponseRevert bytes=%zu url=\"%.80s\"\n",
          original.size(), url_request_->url().spec().c_str());
    }
  }

  if (!QnxWriteBodyToNewDataPipe(qnx_youtube_player_body_, "PlayerResponseFlush"))
    return;
  BerryRegisterGooglevideoAllowlist(qnx_youtube_player_body_);
  BerryLogPlayerFormatSummary(qnx_youtube_player_body_);
}

void URLLoader::QnxPrepareYoutubeWatchShim() {
  qnx_google_sorry_bounce_ = false;
  qnx_youtube_watch_shim_active_ = true;
  qnx_youtube_watch_shim_buffer_ = true;
  qnx_watch_shim_response_ready_ = false;
  qnx_watch_shim_scrubbed_ = false;
  qnx_youtube_watch_page_sniff_.clear();
  const GURL canonical_watch =
      BerryCanonicalizeYoutubeWatchUrl(url_request_->url());
  qnx_youtube_video_id_ = BerryExtractYoutubeVideoId(canonical_watch);
  if (qnx_youtube_video_id_.empty()) {
    qnx_youtube_watch_shim_active_ = false;
    qnx_youtube_watch_shim_buffer_ = false;
    return;
  }

  BerryClearGooglevideoAllowlist();
  qnx_youtube_watch_shim_html_ =
      BerryBuildWatchShimHtml(canonical_watch, qnx_youtube_video_id_);
  QNX_NAV_LOG_FMT(
      "BerryNav: WatchShim start loader=%p v=%s url=\"%.80s\" canon=\"%.80s\" "
      "(body-swap)\n",
      this, qnx_youtube_video_id_.c_str(), url_request_->url().spec().c_str(),
      canonical_watch.spec().c_str());
  QNX_NAV_LOG_FMT("BerryNav: WatchShim html loader=%p bytes=%zu\n", this,
                  qnx_youtube_watch_shim_html_.size());
}

void URLLoader::QnxPrepareYoutubeSearchShim() {
  qnx_google_sorry_bounce_ = false;
  qnx_youtube_watch_shim_active_ = true;
  qnx_youtube_watch_shim_buffer_ = true;
  qnx_watch_shim_response_ready_ = false;
  qnx_watch_shim_scrubbed_ = false;
  qnx_youtube_watch_page_sniff_.clear();
  const std::string search_query =
      BerryExtractYoutubeSearchQuery(url_request_->url());
  qnx_youtube_video_id_ = search_query;
  if (search_query.empty()) {
    qnx_youtube_watch_shim_active_ = false;
    qnx_youtube_watch_shim_buffer_ = false;
    return;
  }

  qnx_youtube_watch_shim_html_ =
      BerryBuildSearchShimHtml(url_request_->url(), search_query);
  QNX_NAV_LOG_FMT(
      "BerryNav: SearchShim start loader=%p q=\"%.80s\" url=\"%.80s\" "
      "(body-swap)\n",
      this, search_query.c_str(), url_request_->url().spec().c_str());
  QNX_NAV_LOG_FMT("BerryNav: SearchShim html loader=%p bytes=%zu\n", this,
                  qnx_youtube_watch_shim_html_.size());
}

void URLLoader::QnxPrepareGoogleSorryBounce() {
  qnx_google_sorry_bounce_ = true;
  qnx_youtube_watch_shim_active_ = true;
  qnx_youtube_watch_shim_buffer_ = true;
  qnx_watch_shim_response_ready_ = false;
  qnx_watch_shim_scrubbed_ = false;
  qnx_youtube_watch_page_sniff_.clear();
  const std::string search_query =
      BerryExtractGoogleSearchQueryFromSorryUrl(url_request_->url());
  qnx_youtube_video_id_ = search_query;
  qnx_youtube_watch_shim_html_ =
      BerryBuildSorryBounceHtml(url_request_->url(), search_query);
  QNX_NAV_LOG_FMT(
      "BerryNav: SorryBounce start loader=%p q=\"%.80s\" url=\"%.80s\" "
      "(body-swap)\n",
      this, search_query.c_str(), url_request_->url().spec().c_str());
  QNX_NAV_LOG_FMT("BerryNav: SorryBounce html loader=%p bytes=%zu\n", this,
                  qnx_youtube_watch_shim_html_.size());
}

void URLLoader::QnxPrepareYoutubeHomeBounce() {
  // Reuses the sorry-bounce plumbing (buffer original body, force 200, swap
  // in local HTML) with a different page.
  qnx_google_sorry_bounce_ = true;
  qnx_youtube_watch_shim_active_ = true;
  qnx_youtube_watch_shim_buffer_ = true;
  qnx_watch_shim_response_ready_ = false;
  qnx_watch_shim_scrubbed_ = false;
  qnx_youtube_watch_page_sniff_.clear();
  qnx_youtube_video_id_.clear();
  qnx_youtube_watch_shim_html_ = BerryBuildYoutubeHomeBounceHtml();
  QNX_NAV_LOG_FMT(
      "BerryNav: YtHomeBounce start loader=%p url=\"%.80s\" (body-swap)\n",
      this, url_request_->url().spec().c_str());
}

void URLLoader::QnxFlushWatchShimBody() {
  if (qnx_youtube_watch_shim_html_.empty())
    return;
  const char* log_tag =
      qnx_google_sorry_bounce_ ? "SorryBounceFlush" : "WatchShimFlush";
  if (QnxWriteBodyToNewDataPipe(qnx_youtube_watch_shim_html_, log_tag) &&
      !qnx_google_sorry_bounce_) {
    BerryOnWatchShimFlushed(qnx_youtube_video_id_);
  }
}
#endif

void URLLoader::ContinueOnResponseStarted() {
#if defined(__QNX__)
  QNX_TRACE_MSG("QNX:UL:ContOnResp\n");
#endif
#if defined(__QNX__) || defined(__QNXNTO__)
  {
    const std::string spec = url_request_->url().spec();
    if (BerryShouldBufferYoutubeResponse(url_request_->url())) {
      qnx_youtube_player_buffer_ = true;
      QNX_NAV_LOG_FMT("BerryNav: PlayerResponseBuffer url=\"%.80s\"\n",
                      spec.c_str());
    }
  }
#endif
  MojoCreateDataPipeOptions options;
  options.struct_size = sizeof(MojoCreateDataPipeOptions);
  options.flags = MOJO_CREATE_DATA_PIPE_FLAG_NONE;
  options.element_num_bytes = 1;
  options.capacity_num_bytes =
      network::features::GetDataPipeDefaultAllocationSize(
          features::DataPipeAllocationSize::kLargerSizeIfPossible);
  if (!qnx_youtube_player_buffer_ && !qnx_youtube_watch_shim_buffer_) {
    MojoResult result =
        mojo::CreateDataPipe(&options, response_body_stream_, consumer_handle_);
    if (result != MOJO_RESULT_OK) {
#if defined(__QNX__)
      QNX_TRACE_MSG("QNX:UL:DataPipeFail!\n");
#endif
      NotifyCompleted(net::ERR_INSUFFICIENT_RESOURCES);
      return;
    }
    DCHECK(response_body_stream_.is_valid());
    DCHECK(consumer_handle_.is_valid());

    peer_closed_handle_watcher_.Watch(
        response_body_stream_.get(), MOJO_HANDLE_SIGNAL_PEER_CLOSED,
        base::BindRepeating(&URLLoader::OnResponseBodyStreamConsumerClosed,
                            base::Unretained(this)));
    peer_closed_handle_watcher_.ArmOrNotify();

    writable_handle_watcher_.Watch(
        response_body_stream_.get(), MOJO_HANDLE_SIGNAL_WRITABLE,
        base::BindRepeating(&URLLoader::OnResponseBodyStreamReady,
                            base::Unretained(this)));
  }

  // Do not account header bytes when reporting received body bytes to client.
  reported_total_encoded_bytes_ = url_request_->GetTotalReceivedBytes();

  if (upload_progress_tracker_) {
    upload_progress_tracker_->OnUploadCompleted();
    upload_progress_tracker_ = nullptr;
  }

  // Enforce the Cross-Origin-Resource-Policy (CORP) header.
  const CrossOriginEmbedderPolicy kEmpty;
  const CrossOriginEmbedderPolicy& cross_origin_embedder_policy =
      factory_params_->client_security_state
          ? factory_params_->client_security_state->cross_origin_embedder_policy
          : kEmpty;
  if (absl::optional<mojom::BlockedByResponseReason> blocked_reason =
          CrossOriginResourcePolicy::IsBlocked(
              url_request_->url(), url_request_->original_url(),
              url_request_->initiator(), *response_, request_mode_,
              request_destination_, cross_origin_embedder_policy,
              coep_reporter_)) {
    CompleteBlockedResponse(net::ERR_BLOCKED_BY_RESPONSE, false,
                            blocked_reason);
    // Close the socket associated with the request, to prevent leaking
    // information.
    url_request_->AbortAndCloseConnection();
    DeleteSelf();
    return;
  }

  // Enforce ad-auction-only signals -- the renderer process isn't allowed
  // to read auction-only signals for ad auctions; only the browser process
  // is allowed to read those, and only the browser process can issue trusted
  // requests.
  std::string auction_only;
  // TODO(crbug.com/1448564): Remove old names once API users have migrated to
  // new names.
  if (!factory_params_->is_trusted && response_->headers &&
      (response_->headers->GetNormalizedHeader("Ad-Auction-Only",
                                               &auction_only) ||
       response_->headers->GetNormalizedHeader("X-FLEDGE-Auction-Only",
                                               &auction_only)) &&
      base::EqualsCaseInsensitiveASCII(auction_only, "true")) {
    CompleteBlockedResponse(net::ERR_BLOCKED_BY_RESPONSE, false);
    url_request_->AbortAndCloseConnection();
    DeleteSelf();
    return;
  }

  // Figure out if we need to sniff (for MIME type detection or for Cross-Origin
  // Read Blocking / CORB).
  if (factory_params_->is_corb_enabled) {
    corb_analyzer_ = corb::ResponseAnalyzer::Create(*per_factory_corb_state_);
    is_more_corb_sniffing_needed_ = true;
    auto decision =
        corb_analyzer_->Init(url_request_->url(), url_request_->initiator(),
                             request_mode_, request_destination_, *response_);
    if (MaybeBlockResponseForCorb(decision))
      return;
  }

  if ((options_ & mojom::kURLLoadOptionSniffMimeType)) {
    if (ShouldSniffContent(url_request_->url(), *response_)) {
      // We're going to look at the data before deciding what the content type
      // is.  That means we need to delay sending the response started IPC.
      VLOG(1) << "Will sniff content for mime type: " << url_request_->url();
      is_more_mime_sniffing_needed_ = true;
    } else if (response_->mime_type.empty()) {
      // Ugg.  The server told us not to sniff the content but didn't give us
      // a mime type.  What's a browser to do?  Turns out, we're supposed to
      // treat the response as "text/plain".  This is the most secure option.
      response_->mime_type.assign("text/plain");
    }
  }

  StartReading();
}

void URLLoader::ReadMore() {
#if defined(__QNX__)
  QNX_TRACE_MSG("QNX:UL:ReadMore\n");
#endif
  DCHECK(!read_in_progress_);
#if defined(__QNX__) || defined(__QNXNTO__)
  if (qnx_youtube_player_buffer_ || qnx_youtube_watch_shim_buffer_) {
    if (should_pause_reading_body_) {
      paused_reading_body_ = true;
      return;
    }
    if (!qnx_youtube_read_buffer_.get()) {
      qnx_youtube_read_buffer_ =
          base::MakeRefCounted<net::IOBufferWithSize>(65536);
    }
    read_in_progress_ = true;
    int bytes_read = url_request_->Read(qnx_youtube_read_buffer_.get(),
                                        qnx_youtube_read_buffer_->size());
    if (bytes_read != net::ERR_IO_PENDING) {
      DidRead(bytes_read, true);
    }
    return;
  }
#endif
  // Once the MIME type is sniffed, all data is sent as soon as it is read from
  // the network.
  DCHECK(consumer_handle_.is_valid() || !pending_write_);

  if (should_pause_reading_body_) {
    paused_reading_body_ = true;
    return;
  }

  if (!pending_write_.get()) {
    // TODO: we should use the abstractions in MojoAsyncResourceHandler.
    DCHECK_EQ(0u, pending_write_buffer_offset_);
    MojoResult result = NetToMojoPendingBuffer::BeginWrite(
        &response_body_stream_, &pending_write_);
    switch (result) {
      case MOJO_RESULT_OK:
        break;
      case MOJO_RESULT_SHOULD_WAIT:
        // The pipe is full. We need to wait for it to have more space.
        writable_handle_watcher_.ArmOrNotify();
        return;
      default:
        // The response body stream is in a bad state. Bail.
        NotifyCompleted(net::ERR_FAILED);
        return;
    }
    pending_write_buffer_size_ = pending_write_->size();
    DCHECK_GT(static_cast<uint32_t>(std::numeric_limits<int>::max()),
              pending_write_buffer_size_);
    if (consumer_handle_.is_valid()) {
      DCHECK_GE(pending_write_buffer_size_,
                static_cast<uint32_t>(net::kMaxBytesToSniff));
    }
  }

  auto buf = base::MakeRefCounted<NetToMojoIOBuffer>(
      pending_write_.get(), pending_write_buffer_offset_);
  read_in_progress_ = true;
  int bytes_read = url_request_->Read(
      buf.get(), static_cast<int>(pending_write_buffer_size_ -
                                  pending_write_buffer_offset_));
  if (bytes_read != net::ERR_IO_PENDING) {
    DidRead(bytes_read, true);
    // |this| may have been deleted.
  }
}

void URLLoader::DidRead(int num_bytes, bool completed_synchronously) {
#if defined(__QNX__)
  QNX_TRACE_FMT("QNX:UL:DidRead bytes=%d sync=%d\n",
                     num_bytes, completed_synchronously ? 1 : 0);
#endif
  DCHECK(read_in_progress_);
  read_in_progress_ = false;

#if defined(__QNX__) || defined(__QNXNTO__)
  if (qnx_youtube_player_buffer_ || qnx_youtube_watch_shim_buffer_) {
    if (num_bytes > 0 && qnx_youtube_player_buffer_) {
      qnx_youtube_player_body_.append(qnx_youtube_read_buffer_->data(),
                                      num_bytes);
    }
    if (num_bytes > 0 && qnx_youtube_watch_shim_buffer_) {
      if (qnx_youtube_watch_page_sniff_.size() < 512 * 1024) {
        const size_t room =
            512 * 1024 - qnx_youtube_watch_page_sniff_.size();
        const size_t take = std::min(room, static_cast<size_t>(num_bytes));
        qnx_youtube_watch_page_sniff_.append(
            qnx_youtube_read_buffer_->data(), take);
        BerryTryCacheVisitorDataFromWatchHtml(qnx_youtube_watch_page_sniff_);
      }
    }
    if (num_bytes > 0) {
      if (completed_synchronously) {
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE,
            base::BindOnce(&URLLoader::ReadMore,
                           weak_ptr_factory_.GetWeakPtr()));
      } else {
        ReadMore();
      }
      return;
    }
    NotifyCompleted(num_bytes);
    return;
  }
#endif

  if (memory_cache_writer_ && pending_write_ && num_bytes > 0) {
    if (!memory_cache_writer_->OnDataRead(
            pending_write_->buffer() + pending_write_buffer_offset_,
            num_bytes)) {
      memory_cache_writer_.reset();
    }
  }

  size_t new_data_offset = pending_write_buffer_offset_;
  if (num_bytes > 0) {
    pending_write_buffer_offset_ += num_bytes;

    // Only notify client of download progress if we're done sniffing and
    // started sending response.
    if (!consumer_handle_.is_valid()) {
      int64_t total_encoded_bytes = url_request_->GetTotalReceivedBytes();
      int64_t delta = total_encoded_bytes - reported_total_encoded_bytes_;
      DCHECK_LE(0, delta);
      if (delta)
        url_loader_client_.Get()->OnTransferSizeUpdated(delta);
      reported_total_encoded_bytes_ = total_encoded_bytes;
    }
  }

#if defined(__QNX__)
  if (base::QnxDecodeProbeEnabled() && !qnx_captcha_probe_logged_ &&
      num_bytes > 0 && pending_write_) {
    qnx_captcha_probe_logged_ = true;
    const std::string spec = url_request_->url().spec();
    std::string enc("(none)");
    std::string ctype("(none)");
    if (response_ && response_->headers) {
      response_->headers->GetNormalizedHeader("Content-Encoding", &enc);
      response_->headers->GetNormalizedHeader("Content-Type", &ctype);
    }
    const char* body = pending_write_->buffer() + new_data_offset;
    int n = num_bytes < 24 ? num_bytes : 24;
    char hex[80];
    int o = 0;
    for (int i = 0; i < n && o < static_cast<int>(sizeof(hex)) - 3; ++i) {
      o += snprintf(hex + o, sizeof(hex) - o, "%02x",
                    static_cast<unsigned char>(body[i]));
    }
    QNX_DECODE_PROBE_FMT(
        "BerryNav: DecodeProbe body enc=%s ctype=%s n=%d hex=%s url=\"%.90s\"\n",
        enc.c_str(), ctype.c_str(), num_bytes, hex, spec.c_str());
  }
#endif

  bool complete_read = true;
  if (consumer_handle_.is_valid()) {
    // |pending_write_| may be null if the job self-aborts due to a suspend;
    // this will have |consumer_handle_| valid when the loader is paused.
    if (pending_write_) {
      // Limit sniffing to the first net::kMaxBytesToSniff.
      size_t data_length = pending_write_buffer_offset_;
      if (data_length > net::kMaxBytesToSniff)
        data_length = net::kMaxBytesToSniff;

      base::StringPiece data(pending_write_->buffer(), data_length);
      bool stop_sniffing_after_processing_current_data =
          (num_bytes <= 0 ||
           pending_write_buffer_offset_ >= net::kMaxBytesToSniff);

      if (is_more_mime_sniffing_needed_) {
        const std::string& type_hint = response_->mime_type;
        std::string new_type;
        is_more_mime_sniffing_needed_ = !net::SniffMimeType(
            data, url_request_->url(), type_hint,
            net::ForceSniffFileUrlsForHtml::kDisabled, &new_type);
        // SniffMimeType() returns false if there is not enough data to
        // determine the mime type. However, even if it returns false, it
        // returns a new type that is probably better than the current one.
        response_->mime_type.assign(new_type);
        response_->did_mime_sniff = true;

        if (stop_sniffing_after_processing_current_data)
          is_more_mime_sniffing_needed_ = false;
      }

      if (is_more_corb_sniffing_needed_) {
        corb::ResponseAnalyzer::Decision corb_decision =
            corb::ResponseAnalyzer::Decision::kSniffMore;

        // `has_new_data_to_sniff` can be false at the end-of-stream.
        bool has_new_data_to_sniff = new_data_offset < data.length();
        if (has_new_data_to_sniff)
          corb_decision = corb_analyzer_->Sniff(data);

        if (corb_decision == corb::ResponseAnalyzer::Decision::kSniffMore &&
            stop_sniffing_after_processing_current_data) {
          corb_decision = corb_analyzer_->HandleEndOfSniffableResponseBody();
          DCHECK_NE(corb::ResponseAnalyzer::Decision::kSniffMore,
                    corb_decision);
        }

        if (MaybeBlockResponseForCorb(corb_decision))
          return;
      }
    }

    if (!is_more_mime_sniffing_needed_ && !is_more_corb_sniffing_needed_) {
      SendResponseToClient();
    } else {
      complete_read = false;
    }
  }

  if (num_bytes <= 0) {
    // There may be no |pending_write_| if a URLRequestJob cancelled itself in
    // URLRequestJob::OnSuspend() after receiving headers, while there was no
    // pending read.
    // TODO(mmenke): That case is rather unfortunate - something should be done
    // at the socket layer instead, both to make for a better API (Only complete
    // reads when there's a pending read), and to cover all TCP socket uses,
    // since the concern is the effect that entering suspend mode has on
    // sockets. See https://crbug.com/651120.
#if defined(__QNX__) || defined(__QNXNTO__)
    if (pending_write_ && pending_write_buffer_offset_ > 0) {
      std::string body(pending_write_->buffer(),
                       pending_write_buffer_offset_);
      if (BerryStripSabrFromYoutubePlayerResponse(url_request_->url(),
                                                 &body)) {
        if (body.size() <= pending_write_->size()) {
          memcpy(pending_write_->buffer(), body.data(), body.size());
          pending_write_buffer_offset_ = body.size();
        }
      }
    }
#endif
    if (pending_write_)
      CompletePendingWrite(num_bytes == 0);
    NotifyCompleted(num_bytes);
    // |this| will have been deleted.
    return;
  }

  if (complete_read) {
    CompletePendingWrite(true /* success */);
  }
  if (completed_synchronously) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&URLLoader::ReadMore, weak_ptr_factory_.GetWeakPtr()));
  } else {
    ReadMore();
  }
}

void URLLoader::OnReadCompleted(net::URLRequest* url_request, int bytes_read) {
  DCHECK(url_request == url_request_.get());

  DidRead(bytes_read, false);
  // |this| may have been deleted.
}

int URLLoader::OnBeforeStartTransaction(
    const net::HttpRequestHeaders& headers,
    net::NetworkDelegate::OnBeforeStartTransactionCallback callback) {
  if (header_client_) {
    header_client_->OnBeforeSendHeaders(
        cookies_from_browser_.empty()
            ? headers
            : AttachCookies(headers, cookies_from_browser_),
        base::BindOnce(&URLLoader::OnBeforeSendHeadersComplete,
                       weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
    return net::ERR_IO_PENDING;
  }

  // Additional cookies were added to the existing headers, so `callback` must
  // be invoked to ensure that the cookies are included in the request.
  if (!cookies_from_browser_.empty()) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), net::OK,
                       AttachCookies(headers, cookies_from_browser_)));
    return net::ERR_IO_PENDING;
  }

  return net::OK;
}

int URLLoader::OnHeadersReceived(
    net::CompletionOnceCallback callback,
    const net::HttpResponseHeaders* original_response_headers,
    scoped_refptr<net::HttpResponseHeaders>* override_response_headers,
    const net::IPEndPoint& endpoint,
    absl::optional<GURL>* preserve_fragment_on_redirect_url) {
  if (header_client_) {
    header_client_->OnHeadersReceived(
        original_response_headers->raw_headers(), endpoint,
        base::BindOnce(&URLLoader::OnHeadersReceivedComplete,
                       weak_ptr_factory_.GetWeakPtr(), std::move(callback),
                       override_response_headers,
                       preserve_fragment_on_redirect_url));
    return net::ERR_IO_PENDING;
  }
  return net::OK;
}

URLLoader::PartialLoadInfo URLLoader::GetPartialLoadInfo() const {
  return PartialLoadInfo(url_request_->GetLoadState(),
                         url_request_->GetUploadProgress());
}

mojom::LoadInfoPtr URLLoader::CreateLoadInfo(
    const PartialLoadInfo& partial_load_info) {
  return mojom::LoadInfo::New(
      base::TimeTicks::Now(), url_request_->url().host(),
      partial_load_info.load_state.state, partial_load_info.load_state.param,
      partial_load_info.upload_progress.position(),
      partial_load_info.upload_progress.size());
}

net::LoadState URLLoader::GetLoadState() const {
  return url_request_->GetLoadState().state;
}

net::UploadProgress URLLoader::GetUploadProgress() const {
  return url_request_->GetUploadProgress();
}

int32_t URLLoader::GetProcessId() const {
  return factory_params_->process_id;
}

void URLLoader::SetEnableReportingRawHeaders(bool allow) {
  enable_reporting_raw_headers_ = allow;
}

uint32_t URLLoader::GetResourceType() const {
  return resource_type_;
}

bool URLLoader::AllowCookies(
    const GURL& url,
    const net::SiteForCookies& site_for_cookies) const {
  net::StaticCookiePolicy::Type policy =
      net::StaticCookiePolicy::ALLOW_ALL_COOKIES;
  if (options_ & mojom::kURLLoadOptionBlockAllCookies) {
    policy = net::StaticCookiePolicy::BLOCK_ALL_COOKIES;
  } else if (options_ & mojom::kURLLoadOptionBlockThirdPartyCookies) {
    policy = net::StaticCookiePolicy::BLOCK_ALL_THIRD_PARTY_COOKIES;
  } else {
    return true;
  }
  return net::StaticCookiePolicy(policy).CanAccessCookies(
             url, site_for_cookies) == net::OK;
}

// static
URLLoader* URLLoader::ForRequest(const net::URLRequest& request) {
  auto* pointer =
      static_cast<UnownedPointer*>(request.GetUserData(kUserDataKey));
  if (!pointer)
    return nullptr;
  return pointer->get();
}

void URLLoader::OnAuthCredentials(
    const absl::optional<net::AuthCredentials>& credentials) {
  auth_challenge_responder_receiver_.reset();

  if (!credentials.has_value()) {
    url_request_->CancelAuth();
  } else {
    // CancelAuth will proceed to the body, so cookies only need to be reported
    // here.
    ReportFlaggedResponseCookies(false);
    url_request_->SetAuth(credentials.value());
  }
}

void URLLoader::ContinueWithCertificate(
    const scoped_refptr<net::X509Certificate>& x509_certificate,
    const std::string& provider_name,
    const std::vector<uint16_t>& algorithm_preferences,
    mojo::PendingRemote<mojom::SSLPrivateKey> ssl_private_key) {
  client_cert_responder_receiver_.reset();
  auto key = base::MakeRefCounted<SSLPrivateKeyInternal>(
      provider_name, algorithm_preferences, std::move(ssl_private_key));
  url_request_->ContinueWithCertificate(std::move(x509_certificate),
                                        std::move(key));
}

void URLLoader::ContinueWithoutCertificate() {
  client_cert_responder_receiver_.reset();
  url_request_->ContinueWithCertificate(nullptr, nullptr);
}

void URLLoader::CancelRequest() {
  client_cert_responder_receiver_.reset();
  url_request_->CancelWithError(net::ERR_SSL_CLIENT_AUTH_CERT_NEEDED);
}

#if defined(__QNX__) || defined(__QNXNTO__)
void URLLoader::SendCompletionToClient(URLLoaderCompletionStatus status) {
  if (qnx_response_delivery_pending_) {
    qnx_completion_pending_ = true;
    qnx_pending_completion_status_ = std::move(status);
    return;
  }
  if (memory_cache_writer_)
    memory_cache_writer_->OnCompleted(status);
  if (url_loader_client_.Get())
    url_loader_client_.Get()->OnComplete(status);
  DeleteSelf();
}

void URLLoader::RunQnxDeferredClientCompletion() {
  qnx_response_delivery_pending_ = false;
  if (!qnx_completion_pending_)
    return;
  qnx_completion_pending_ = false;
  URLLoaderCompletionStatus status = std::move(qnx_pending_completion_status_);
  if (memory_cache_writer_)
    memory_cache_writer_->OnCompleted(status);
  if (url_loader_client_.Get())
    url_loader_client_.Get()->OnComplete(status);
  DeleteSelf();
}
#endif

void URLLoader::NotifyCompleted(int error_code) {
  // Ensure sending the final upload progress message here, since
  // OnResponseCompleted can be called without OnResponseStarted on cancellation
  // or error cases.
  if (upload_progress_tracker_) {
    upload_progress_tracker_->OnUploadCompleted();
    upload_progress_tracker_ = nullptr;
  }

  auto total_received = url_request_->GetTotalReceivedBytes();
  auto total_sent = url_request_->GetTotalSentBytes();
  if (total_received > 0) {
    base::UmaHistogramCustomCounts("DataUse.BytesReceived3.Delegate",
                                   total_received, 50, 10 * 1000 * 1000, 50);
  }

  if (total_sent > 0) {
    UMA_HISTOGRAM_COUNTS_1M("DataUse.BytesSent3.Delegate", total_sent);
  }
  if ((total_received > 0 || total_sent > 0)) {
    if (url_loader_network_observer_ && provide_data_use_updates_) {
      url_loader_network_observer_->OnDataUseUpdate(
          url_request_->traffic_annotation().unique_id_hash_code,
          total_received, total_sent);
    }
  }

  if (url_loader_client_.Get()) {
#if defined(__QNX__) || defined(__QNXNTO__)
    if (qnx_youtube_watch_shim_buffer_) {
      if (error_code == net::OK && qnx_watch_shim_response_ready_ &&
          qnx_watch_shim_scrubbed_ && response_ &&
          !qnx_youtube_watch_shim_html_.empty()) {
        QnxFlushWatchShimBody();
      } else {
        QNX_NAV_LOG_FMT(
            "BerryNav: %s skip flush loader=%p err=%d ready=%d scrub=%d "
            "resp=%d\n",
            qnx_google_sorry_bounce_ ? "SorryBounce" : "WatchShim", this,
            error_code, qnx_watch_shim_response_ready_ ? 1 : 0,
            qnx_watch_shim_scrubbed_ ? 1 : 0, response_ ? 1 : 0);
        qnx_youtube_watch_shim_buffer_ = false;
        qnx_youtube_watch_shim_active_ = false;
        qnx_google_sorry_bounce_ = false;
        qnx_youtube_watch_shim_html_.clear();
        consumer_handle_.reset();
      }
    } else if (qnx_youtube_player_buffer_ && !qnx_youtube_player_body_.empty()) {
      QnxFlushBufferedYoutubePlayerBody();
    }
#endif
    if (consumer_handle_.is_valid() && response_)
      SendResponseToClient();

    URLLoaderCompletionStatus status;
    status.error_code = error_code;
    if (error_code == net::ERR_QUIC_PROTOCOL_ERROR) {
      net::NetErrorDetails details;
      url_request_->PopulateNetErrorDetails(&details);
      status.extended_error_code = details.quic_connection_error;
    } else if (error_code == net::ERR_INCONSISTENT_IP_ADDRESS_SPACE) {
      // The error code is only used internally, translate it into a CORS error.
      DCHECK(cors_error_status_.has_value());
      status.error_code = net::ERR_FAILED;
    }
    status.exists_in_cache = url_request_->response_info().was_cached;
    status.completion_time = base::TimeTicks::Now();
    status.encoded_data_length = url_request_->GetTotalReceivedBytes();
    status.encoded_body_length = url_request_->GetRawBodyBytes();
    status.decoded_body_length = total_written_bytes_;
    status.proxy_server = url_request_->proxy_chain().proxy_server();
    status.resolve_error_info =
        url_request_->response_info().resolve_error_info;
    if (trust_token_status_)
      status.trust_token_operation_status = *trust_token_status_;
    status.cors_error_status = cors_error_status_;

#if defined(__QNX__)
    // Diagnose webpack ChunkLoadError on JS-heavy SPAs (e.g. x.com): log the
    // completion of script subresources so we can tell a network/HTTP failure
    // (error_code != 0 or non-200) apart from a corrupt/truncated decode
    // (error_code == 0 but decoded != Content-Length, often a Brotli/Zstd bug).
    {
      const std::string spec = url_request_->url().spec();
      const bool is_js = spec.find(".js") != std::string::npos;
      const bool is_spa_host = spec.find("twimg.com") != std::string::npos ||
                               spec.find("/client-web/") != std::string::npos ||
                               spec.find("x.com") != std::string::npos;
      const bool is_googlevideo = spec.find("googlevideo.com") != std::string::npos;
      const bool is_maps_tile =
          spec.find("khms") != std::string::npos ||
          spec.find("maps/vt") != std::string::npos ||
          spec.find("maps.googleapis.com/maps/vt") != std::string::npos ||
          spec.find("mt0.google.com") != std::string::npos ||
          spec.find("mt1.google.com") != std::string::npos ||
          spec.find("google.com/maps/vt") != std::string::npos ||
          (spec.find("maps.googleapis.com") != std::string::npos &&
           spec.find("/maps/api/js") != std::string::npos);
      if ((is_js && is_spa_host) || error_code != 0 || is_googlevideo ||
          is_maps_tile) {
        std::string enc("(none)");
        std::string clen("(none)");
        int http_status = -1;
        if (response_ && response_->headers) {
          response_->headers->GetNormalizedHeader("Content-Encoding", &enc);
          response_->headers->GetNormalizedHeader("Content-Length", &clen);
          http_status = response_->headers->response_code();
        }
        const std::string err_str = net::ErrorToShortString(error_code);
        const std::string& method = url_request_->method();
        const long long sent_body =
            (long long)url_request_->GetTotalSentBytes();
        QNX_NAV_LOG_FMT(
            "BerryNav: ChunkDone err=%d (%s) http=%d method=%s enc=%s clen=%s "
            "sentBody=%lld encBody=%lld decBody=%lld url=\"%.110s\"\n",
            error_code, err_str.c_str(), http_status, method.c_str(),
            enc.c_str(), clen.c_str(), sent_body,
            (long long)status.encoded_body_length,
            (long long)status.decoded_body_length, spec.c_str());
      }
    }
#endif

    if ((options_ & mojom::kURLLoadOptionSendSSLInfoForCertificateError) &&
        net::IsCertStatusError(url_request_->ssl_info().cert_status)) {
      status.ssl_info = url_request_->ssl_info();
    }

#if defined(__QNX__) || defined(__QNXNTO__)
    SendCompletionToClient(std::move(status));
    return;
#else
    if (memory_cache_writer_)
      memory_cache_writer_->OnCompleted(status);

    url_loader_client_.Get()->OnComplete(status);
#endif
  }

  DeleteSelf();
}

void URLLoader::OnMojoDisconnect() {
  NotifyCompleted(net::ERR_FAILED);
}

void URLLoader::OnResponseBodyStreamConsumerClosed(MojoResult result) {
  NotifyCompleted(net::ERR_FAILED);
}

void URLLoader::OnResponseBodyStreamReady(MojoResult result) {
  if (result != MOJO_RESULT_OK) {
    NotifyCompleted(net::ERR_FAILED);
    return;
  }

  ReadMore();
}

void URLLoader::DeleteSelf() {
  std::move(delete_callback_).Run(this);
}

void URLLoader::SendResponseToClient() {
  TRACE_EVENT("loading", "network::URLLoader::SendResponseToClient",
              perfetto::Flow::FromPointer(this), "url", url_request_->url());
#if defined(__QNX__)
  QNX_TRACE_FMT("QNX:UL:SendResp handle=%d\n",
                     consumer_handle_.is_valid() ? 1 : 0);
#endif
#if defined(__QNX__) || defined(__QNXNTO__)
  if (qnx_response_sent_to_client_)
    return;
  // Buffered YouTube player JSON: defer ResponseReceived until body is stripped
  // and the data pipe is created (NotifyCompleted → QnxFlushBufferedYoutubePlayerBody).
  if ((qnx_youtube_player_buffer_ || qnx_youtube_watch_shim_buffer_) &&
      !consumer_handle_.is_valid())
    return;
#endif
  DCHECK_EQ(emitted_devtools_raw_request_, emitted_devtools_raw_response_);
  response_->emitted_extra_info = emitted_devtools_raw_request_;

#if defined(__QNX__) || defined(__QNXNTO__)
  // Defer subresource responses so the IO thread can keep ReadMore() without
  // reentrancy deadlocks. Main-frame navigations must not sit in the IO task
  // queue behind subresources — that added ~15–20s TTFB on BB10.
  if (resource_type_ != 0) {
    qnx_response_sent_to_client_ = true;
    qnx_response_delivery_pending_ = true;
    network::mojom::URLResponseHeadPtr head = response_->Clone();
    mojo::ScopedDataPipeConsumerHandle body = std::move(consumer_handle_);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<URLLoader> loader,
               network::mojom::URLResponseHeadPtr head,
               mojo::ScopedDataPipeConsumerHandle body) {
              if (!loader)
                return;
              mojom::URLLoaderClient* client = loader->url_loader_client_.Get();
              if (!client) {
                loader->RunQnxDeferredClientCompletion();
                return;
              }
              client->OnReceiveResponse(std::move(head), std::move(body),
                                        absl::nullopt);
#if defined(__QNX__)
              QNX_TRACE_MSG("QNX:UL:SendRespDone\n");
#endif
              loader->RunQnxDeferredClientCompletion();
            },
            weak_ptr_factory_.GetWeakPtr(), std::move(head), std::move(body)));
    return;
  }
#endif

  url_loader_client_.Get()->OnReceiveResponse(
      response_->Clone(), std::move(consumer_handle_), absl::nullopt);
#if defined(__QNX__) || defined(__QNXNTO__)
  qnx_response_sent_to_client_ = true;
#endif
#if defined(__QNX__)
  QNX_TRACE_MSG("QNX:UL:SendRespDone\n");
#endif
}

void URLLoader::CompletePendingWrite(bool success) {
  if (success) {
    // The write can only be completed immediately in case of a success, since
    // doing so invalidates memory of any attached NetToMojoIOBuffer's; but in
    // case of an abort, particularly one caused by a suspend, the failure may
    // be delivered to URLLoader while the disk_cache layer is still hanging on
    // to the now-invalid IOBuffer in some worker thread trying to commit it to
    // disk.  In case of an error, this will have to wait till everything is
    // destroyed.
    response_body_stream_ =
        pending_write_->Complete(pending_write_buffer_offset_);
  }
  total_written_bytes_ += pending_write_buffer_offset_;
  pending_write_ = nullptr;
  pending_write_buffer_offset_ = 0;
}

void URLLoader::SetRawResponseHeaders(
    scoped_refptr<const net::HttpResponseHeaders> headers) {
  raw_response_headers_ = headers;
}

void URLLoader::NotifyEarlyResponse(
    scoped_refptr<const net::HttpResponseHeaders> headers) {
  DCHECK(!has_received_response_);
  DCHECK(url_loader_client_.Get());
  DCHECK(headers);
  DCHECK_EQ(headers->response_code(), 103);

  // Calculate IP address space.
  mojom::ParsedHeadersPtr parsed_headers =
      PopulateParsedHeaders(headers.get(), url_request_->url());
  std::vector<GURL> url_list_via_service_worker;
  net::IPEndPoint transaction_endpoint;
  bool has_endpoint =
      url_request_->GetTransactionRemoteEndpoint(&transaction_endpoint);
  DCHECK(has_endpoint);
  CalculateClientAddressSpaceParams params(
      url_list_via_service_worker, parsed_headers, transaction_endpoint);
  mojom::IPAddressSpace ip_address_space =
      CalculateClientAddressSpace(url_request_->url(), params);

  mojom::ReferrerPolicy referrer_policy = ParseReferrerPolicy(*headers);

  url_loader_client_.Get()->OnReceiveEarlyHints(mojom::EarlyHints::New(
      std::move(parsed_headers), referrer_policy, ip_address_space));
}

void URLLoader::SetRawRequestHeadersAndNotify(
    net::HttpRawRequestHeaders headers) {
  // If we have seen_raw_request_headers_, then don't notify DevTools to prevent
  // duplicate ExtraInfo events.
  if (!seen_raw_request_headers_ && devtools_observer_ &&
      devtools_request_id()) {
    std::vector<network::mojom::HttpRawHeaderPairPtr> header_array;
    header_array.reserve(headers.headers().size());

    for (const auto& header : headers.headers()) {
      network::mojom::HttpRawHeaderPairPtr pair =
          network::mojom::HttpRawHeaderPair::New();
      pair->key = header.first;
      pair->value = header.second;
      header_array.push_back(std::move(pair));
    }
    DispatchOnRawRequest(std::move(header_array));
  }

  if (cookie_observer_) {
    std::vector<mojom::CookieOrLineWithAccessResultPtr> reported_cookies;
    for (const auto& cookie_with_access_result :
         url_request_->maybe_sent_cookies()) {
      if (ShouldNotifyAboutCookie(
              cookie_with_access_result.access_result.status)) {
        reported_cookies.push_back(mojom::CookieOrLineWithAccessResult::New(
            mojom::CookieOrLine::NewCookie(cookie_with_access_result.cookie),
            cookie_with_access_result.access_result));
      }
    }

    if (!reported_cookies.empty()) {
      cookie_access_details_.emplace_back(mojom::CookieAccessDetails::New(
          mojom::CookieAccessDetails::Type::kRead, url_request_->url(),
          url_request_->isolation_info().top_frame_origin().value_or(
              url::Origin()),
          url_request_->site_for_cookies(), std::move(reported_cookies),
          devtools_request_id(), /*count=*/1, is_ad_tagged_,
          url_request_->cookie_setting_overrides()));
    }
  }
}

bool URLLoader::IsSharedDictionaryReadAllowed() {
  return shared_dictionary_checker_->CheckAllowedToReadAndReport(
      url_request_->url(), url_request_->site_for_cookies(),
      url_request_->isolation_info());
}

void URLLoader::DispatchOnRawRequest(
    std::vector<network::mojom::HttpRawHeaderPairPtr> headers) {
  DCHECK(devtools_observer_ && devtools_request_id());

  seen_raw_request_headers_ = true;

  net::LoadTimingInfo load_timing_info;
  url_request_->GetLoadTimingInfo(&load_timing_info);

  emitted_devtools_raw_request_ = true;

  absl::optional<bool> site_has_cookie_in_other_partition =
      url_request_->context()->cookie_store()->SiteHasCookieInOtherPartition(
          net::SchemefulSite(url_request_->url()),
          net::CookiePartitionKey::FromNetworkIsolationKey(
              url_request_->isolation_info().network_isolation_key()));
  network::mojom::OtherPartitionInfoPtr other_partition_info = nullptr;
  if (site_has_cookie_in_other_partition.has_value()) {
    other_partition_info = network::mojom::OtherPartitionInfo::New();
    other_partition_info->site_has_cookie_in_other_partition =
        *site_has_cookie_in_other_partition;
  }

  devtools_observer_->OnRawRequest(
      devtools_request_id().value(), url_request_->maybe_sent_cookies(),
      std::move(headers), load_timing_info.request_start,
      private_network_access_checker_.CloneClientSecurityState(),
      std::move(other_partition_info));
}

bool URLLoader::DispatchOnRawResponse() {
  if (!devtools_observer_ || !devtools_request_id() ||
      !url_request_->response_headers()) {
    return false;
  }

  if (url_request_->was_cached() && !seen_raw_request_headers_) {
    // If a response in a redirect chain has been cached,
    // we need to clear the emitted_devtools_raw_request_ and
    // emitted_devtools_raw_response_ flags to prevent misreporting
    // that extra info was available on the response. We also suppress
    // reporting the extra info events here.
    emitted_devtools_raw_request_ = false;
    emitted_devtools_raw_response_ = false;
    return false;
  }

  std::vector<network::mojom::HttpRawHeaderPairPtr> header_array;

  // This is gated by enable_reporting_raw_headers_ to be backwards compatible
  // with the old report_raw_headers behavior, where we wouldn't even send
  // raw_response_headers_ to the trusted browser process based devtools
  // instrumentation. This is observed in the case of HSTS redirects, where
  // url_request_->response_headers has the HSTS redirect headers, like
  // Non-Authoritative-Reason, but raw_response_headers_ has something else
  // which doesn't include HSTS information. This is tested by
  // DevToolsTest.TestRawHeadersWithRedirectAndHSTS.
  // TODO(crbug.com/1234823): Remove enable_reporting_raw_headers_
  const net::HttpResponseHeaders* response_headers =
      raw_response_headers_ && enable_reporting_raw_headers_
          ? raw_response_headers_.get()
          : url_request_->response_headers();

  size_t iterator = 0;
  std::string name, value;
  while (response_headers->EnumerateHeaderLines(&iterator, &name, &value)) {
    network::mojom::HttpRawHeaderPairPtr pair =
        network::mojom::HttpRawHeaderPair::New();
    pair->key = name;
    pair->value = value;
    header_array.push_back(std::move(pair));
  }

  // Only send the "raw" header text when the headers were actually send in
  // text form (i.e. not QUIC or SPDY)
  absl::optional<std::string> raw_response_headers;

  const net::HttpResponseInfo& response_info = url_request_->response_info();

  if (!response_info.DidUseQuic() && !response_info.was_fetched_via_spdy) {
    raw_response_headers =
        absl::make_optional(net::HttpUtil::ConvertHeadersBackToHTTPResponse(
            response_headers->raw_headers()));
  }

  if (!seen_raw_request_headers_) {
    // If we send OnRawResponse(), make sure we send OnRawRequest() event if
    // we haven't had the callback from net, to make the client life easier.
    DispatchOnRawRequest({});
  }

  emitted_devtools_raw_response_ = true;
  devtools_observer_->OnRawResponse(
      devtools_request_id().value(), url_request_->maybe_stored_cookies(),
      std::move(header_array), raw_response_headers,
      private_network_access_checker_.ResponseAddressSpace().value_or(
          mojom::IPAddressSpace::kUnknown),
      response_headers->response_code(), url_request_->cookie_partition_key());

  return true;
}

void URLLoader::SendUploadProgress(const net::UploadProgress& progress) {
  url_loader_client_.Get()->OnUploadProgress(
      progress.position(), progress.size(),
      base::BindOnce(&URLLoader::OnUploadProgressACK,
                     weak_ptr_factory_.GetWeakPtr()));
}

void URLLoader::OnUploadProgressACK() {
  if (upload_progress_tracker_)
    upload_progress_tracker_->OnAckReceived();
}

void URLLoader::OnSSLCertificateErrorResponse(const net::SSLInfo& ssl_info,
                                              int net_error) {
  if (net_error == net::OK) {
    url_request_->ContinueDespiteLastError();
    return;
  }

  url_request_->CancelWithSSLError(net_error, ssl_info);
}

bool URLLoader::HasDataPipe() const {
  return pending_write_ || response_body_stream_.is_valid();
}

void URLLoader::ResumeStart() {
  url_request_->LogUnblocked();
  url_request_->Start();
}

void URLLoader::OnBeforeSendHeadersComplete(
    net::NetworkDelegate::OnBeforeStartTransactionCallback callback,
    int result,
    const absl::optional<net::HttpRequestHeaders>& headers) {
  std::move(callback).Run(result, headers);
}

void URLLoader::OnHeadersReceivedComplete(
    net::CompletionOnceCallback callback,
    scoped_refptr<net::HttpResponseHeaders>* out_headers,
    absl::optional<GURL>* out_preserve_fragment_on_redirect_url,
    int result,
    const absl::optional<std::string>& headers,
    const absl::optional<GURL>& preserve_fragment_on_redirect_url) {
  if (headers) {
    *out_headers =
        base::MakeRefCounted<net::HttpResponseHeaders>(headers.value());
  }
  *out_preserve_fragment_on_redirect_url = preserve_fragment_on_redirect_url;
  std::move(callback).Run(result);
}

void URLLoader::CompleteBlockedResponse(
    int error_code,
    bool should_report_corb_blocking,
    absl::optional<mojom::BlockedByResponseReason> reason) {
  if (has_received_response_) {
    // The response headers and body shouldn't yet be sent to the
    // URLLoaderClient.
    DCHECK(response_);
    DCHECK(consumer_handle_.is_valid());
  }

  // Tell the URLLoaderClient that the response has been completed.
  URLLoaderCompletionStatus status;
  status.error_code = error_code;
  status.completion_time = base::TimeTicks::Now();
  status.encoded_data_length = 0;
  status.encoded_body_length = 0;
  status.decoded_body_length = 0;
  status.should_report_corb_blocking = should_report_corb_blocking;
  status.blocked_by_response_reason = reason;

  if (memory_cache_writer_)
    memory_cache_writer_->OnCompleted(status);
  url_loader_client_.Get()->OnComplete(status);

  // Reset the connection to the URLLoaderClient.  This helps ensure that we
  // won't accidentally leak any data to the renderer from this point on.
  url_loader_client_.Reset();
  memory_cache_writer_.reset();
}

URLLoader::BlockResponseForCorbResult URLLoader::BlockResponseForCorb() {
  // CORB should only do work after the response headers have been received.
  DCHECK(has_received_response_);

  // Caller should have set up a CorbAnalyzer for BlockResponseForCorb to be
  // able to do its job.
  DCHECK(corb_analyzer_);

  // The response headers and body shouldn't yet be sent to the URLLoaderClient.
  DCHECK(response_);
  DCHECK(consumer_handle_.is_valid());

  // Send stripped headers to the real URLLoaderClient.
  corb::SanitizeBlockedResponseHeaders(*response_);

  // Determine error code. This essentially handles the "ORB v0.1" and "ORB
  // v0.2" difference.
  int blocked_error_code =
      (corb_analyzer_->ShouldHandleBlockedResponseAs() ==
       corb::ResponseAnalyzer::BlockedResponseHandling::kEmptyResponse)
          ? net::OK
          : net::ERR_BLOCKED_BY_ORB;

  // todo(lukasza/vogelheim): https://crbug.com/827633#c5:
  // This preserves compatibility with current implementations, which use
  // net::ERR_ABORTED when the resource is detachable. This is also used for
  // resources with an empty destination in "ORB v0.2". This behaviour will
  // no longer be used once kOpaqueResponseBlockingErrorsForAllFetches is
  // perma-enabled.
  if (corb_detachable_ && blocked_error_code == net::OK) {
    CHECK(!base::FeatureList::IsEnabled(
        features::kOpaqueResponseBlockingErrorsForAllFetches));
    blocked_error_code = net::ERR_ABORTED;
  }

  // Send empty body to the real URLLoaderClient. This preserves "ORB v0.1"
  // behaviour and will also go away once
  // OpaqueResponseBlockingErrorsForAllFetches is perma-enabled.
  if (blocked_error_code == net::OK || blocked_error_code == net::ERR_ABORTED) {
    mojo::ScopedDataPipeProducerHandle producer_handle;
    mojo::ScopedDataPipeConsumerHandle consumer_handle;
    MojoResult result = mojo::CreateDataPipe(kBlockedBodyAllocationSize,
                                             producer_handle, consumer_handle);
    if (result != MOJO_RESULT_OK) {
      // Defer calling NotifyCompleted to make sure the caller can still access
      // |this|.
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(&URLLoader::NotifyCompleted,
                                    weak_ptr_factory_.GetWeakPtr(),
                                    net::ERR_INSUFFICIENT_RESOURCES));

      return kWillCancelRequest;
    }
    producer_handle.reset();

    // Tell the real URLLoaderClient that the response has been completed.
    url_loader_client_.Get()->OnReceiveResponse(
        response_->Clone(), std::move(consumer_handle), absl::nullopt);
  }

  // At this point, corb_analyzer_ has done its duty. We'll reset it now
  // to force UMA reporting to happen earlier, to support easier testing.
  bool should_report_blocked_response =
      corb_analyzer_->ShouldReportBlockedResponse();
  corb_analyzer_.reset();
  CompleteBlockedResponse(blocked_error_code, should_report_blocked_response);

  // If the factory is asking to complete requests of this type, then we need to
  // continue processing the response to make sure the network cache is
  // populated.  Otherwise we can cancel the request.
  //
  // TODO(lukasza/vogelheim): The `corb_detachable_` logic is meant to ensure a
  // response is cached (in some cases). With HTTP cache partitioning, this is
  // likely much less effective than it used to be. Maybe this mechanism should
  // be retired.
  if (corb_detachable_) {
    // Discard any remaining callbacks or data by rerouting the pipes to
    // EmptyURLLoaderClient.
    receiver_.reset();
    EmptyURLLoaderClientWrapper::DrainURLRequest(
        url_loader_client_.BindNewPipeAndPassReceiver(),
        receiver_.BindNewPipeAndPassRemote());
    receiver_.set_disconnect_handler(
        base::BindOnce(&URLLoader::OnMojoDisconnect, base::Unretained(this)));

    // Ask the caller to continue processing the request.
    return kContinueRequest;
  }

  // Close the socket associated with the request, to prevent leaking
  // information.
  url_request_->AbortAndCloseConnection();

  // Delete self and cancel the request - the caller doesn't need to continue.
  //
  // DeleteSelf is posted asynchronously, to make sure that the callers (e.g.
  // URLLoader::OnResponseStarted and/or URLLoader::DidRead instance methods)
  // can still safely dereference |this|.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&URLLoader::DeleteSelf, weak_ptr_factory_.GetWeakPtr()));
  return kWillCancelRequest;
}

bool URLLoader::MaybeBlockResponseForCorb(
    corb::ResponseAnalyzer::Decision corb_decision) {
  DCHECK(corb_analyzer_);
  DCHECK(is_more_corb_sniffing_needed_);
  bool will_cancel = false;
  switch (corb_decision) {
    case network::corb::ResponseAnalyzer::Decision::kBlock: {
      will_cancel = BlockResponseForCorb() == kWillCancelRequest;
      corb_analyzer_.reset();
      is_more_corb_sniffing_needed_ = false;
      break;
    }
    case network::corb::ResponseAnalyzer::Decision::kAllow:
      corb_analyzer_.reset();
      is_more_corb_sniffing_needed_ = false;
      break;
    case network::corb::ResponseAnalyzer::Decision::kSniffMore:
      break;
  }
  DCHECK_EQ(is_more_corb_sniffing_needed_, !!corb_analyzer_);
  return will_cancel;
}

void URLLoader::ReportFlaggedResponseCookies(bool call_cookie_observer) {
  if (!cookie_observer_) {
    return;
  }

  std::vector<mojom::CookieOrLineWithAccessResultPtr> reported_cookies;
  for (const auto& cookie_line_and_access_result :
       url_request_->maybe_stored_cookies()) {
    if (ShouldNotifyAboutCookie(
            cookie_line_and_access_result.access_result.status)) {
      mojom::CookieOrLinePtr cookie_or_line;
      if (cookie_line_and_access_result.cookie.has_value()) {
        cookie_or_line = mojom::CookieOrLine::NewCookie(
            cookie_line_and_access_result.cookie.value());
      } else {
        cookie_or_line = mojom::CookieOrLine::NewCookieString(
            cookie_line_and_access_result.cookie_string);
      }

      reported_cookies.push_back(mojom::CookieOrLineWithAccessResult::New(
          std::move(cookie_or_line),
          cookie_line_and_access_result.access_result));
    }
  }

  if (!reported_cookies.empty()) {
    cookie_access_details_.emplace_back(mojom::CookieAccessDetails::New(
        mojom::CookieAccessDetails::Type::kChange, url_request_->url(),
        url_request_->isolation_info().top_frame_origin().value_or(
            url::Origin()),
        url_request_->site_for_cookies(), std::move(reported_cookies),
        devtools_request_id(), /*count=*/1, is_ad_tagged_,
        url_request_->cookie_setting_overrides()));
    if (call_cookie_observer) {
      cookie_observer_->OnCookiesAccessed(std::move(cookie_access_details_));
    }
  }
}

void URLLoader::StartReading() {
#if defined(__QNX__)
  QNX_TRACE_FMT("QNX:UL:StartRead sniff=%d corb=%d\n",
                     is_more_mime_sniffing_needed_ ? 1 : 0,
                     is_more_corb_sniffing_needed_ ? 1 : 0);
#endif
  if (!is_more_mime_sniffing_needed_ && !is_more_corb_sniffing_needed_) {
    // Treat feed types as text/plain.
    if (response_->mime_type == "application/rss+xml" ||
        response_->mime_type == "application/atom+xml") {
      response_->mime_type.assign("text/plain");
    }
#if defined(__QNX__) || defined(__QNXNTO__)
    if (!qnx_youtube_player_buffer_ && !qnx_youtube_watch_shim_buffer_) {
      SendResponseToClient();
    }
#else
    SendResponseToClient();
#endif
  }

  // Start reading...
  ReadMore();
}

bool URLLoader::ShouldForceIgnoreSiteForCookies(
    const ResourceRequest& request) {
  // Ignore site for cookies in requests from an initiator covered by the
  // same-origin-policy exclusions in `origin_access_list_` (typically requests
  // initiated by Chrome Extensions).
  if (request.request_initiator.has_value() &&
      cors::OriginAccessList::AccessState::kAllowed ==
          origin_access_list_->CheckAccessState(
              request.request_initiator.value(), request.url)) {
    return true;
  }

  // Convert `site_for_cookies` into an origin (an opaque origin if
  // `net::SiteForCookies::IsNull()` returns true).
  //
  // Note that `site_for_cookies` is a _site_ rather than an _origin_, but for
  // Chrome Extensions the _site_ and _origin_ of a host are the same extension
  // id.  Thanks to this, for Chrome Extensions, we can pass a _site_ into
  // OriginAccessChecks (which normally expect an _origin_).
  url::Origin site_origin =
      url::Origin::Create(request.site_for_cookies.RepresentativeUrl());

  // If `site_for_cookies` represents an origin that is granted access to the
  // initiator and the target by `origin_access_list_` (typically such
  // `site_for_cookies` represents a Chrome Extension), then we also should
  // force ignoring of site for cookies if the initiator and the target are
  // same-site.
  //
  // Ideally we would walk up the frame tree and check that each ancestor is
  // first-party to the main frame (treating the `origin_access_list_`
  // exceptions as "first-party").  But walking up the tree is not possible in
  // //services/network and so we make do with just checking the direct
  // initiator of the request.
  //
  // We also check same-siteness between the initiator and the requested URL,
  // because setting `force_ignore_site_for_cookies` to true causes Strict
  // cookies to be attached, and having the initiator be same-site to the
  // request URL is a requirement for Strict cookies (see
  // net::cookie_util::ComputeSameSiteContext).
  if (!site_origin.opaque() && request.request_initiator.has_value()) {
    bool site_can_access_target =
        cors::OriginAccessList::AccessState::kAllowed ==
        origin_access_list_->CheckAccessState(site_origin, request.url);
    bool site_can_access_initiator =
        cors::OriginAccessList::AccessState::kAllowed ==
        origin_access_list_->CheckAccessState(
            site_origin, request.request_initiator->GetURL());
    net::SiteForCookies site_of_initiator =
        net::SiteForCookies::FromOrigin(request.request_initiator.value());
    bool are_initiator_and_target_same_site =
        site_of_initiator.IsFirstParty(request.url);
    if (site_can_access_initiator && site_can_access_target &&
        are_initiator_and_target_same_site) {
      return true;
    }
  }

  return false;
}

void URLLoader::SetRequestCredentials(const GURL& url) {
  bool coep_allow_credentials = CoepAllowCredentials(url);

  bool allow_credentials = ShouldAllowCredentials(request_credentials_mode_) &&
                           coep_allow_credentials;

  bool allow_client_certificates =
      ShouldSendClientCertificates(request_credentials_mode_) &&
      coep_allow_credentials;

  // The decision not to include credentials is sticky. This is equivalent to
  // checking the tainted origin flag in the fetch specification.
  if (!allow_credentials)
    url_request_->set_allow_credentials(false);
  if (!allow_client_certificates)
    url_request_->set_send_client_certs(false);

  // Contrary to Firefox or blink's cache, the HTTP cache doesn't distinguish
  // requests including user's credentials from the anonymous ones yet. See
  // https://docs.google.com/document/d/1lvbiy4n-GM5I56Ncw304sgvY5Td32R6KHitjRXvkZ6U
  // As a workaround until a solution is implemented, the cached responses
  // aren't used for those requests.
  if (!coep_allow_credentials) {
    url_request_->SetLoadFlags(url_request_->load_flags() |
                               net::LOAD_BYPASS_CACHE);
  }
}

// [spec]:
// https://fetch.spec.whatwg.org/#cross-origin-embedder-policy-allows-credentials
bool URLLoader::CoepAllowCredentials(const GURL& url) {
  // [spec]: To check if Cross-Origin-Embedder-Policy allows credentials, given
  //         a request request, run these steps:

  // [spec]  1. If request’s mode is not "no-cors", then return true.
  switch (request_mode_) {
    case mojom::RequestMode::kCors:
    case mojom::RequestMode::kCorsWithForcedPreflight:
    case mojom::RequestMode::kNavigate:
    case mojom::RequestMode::kSameOrigin:
      return true;

    case mojom::RequestMode::kNoCors:
      break;
  }

  // [spec]: 2. If request’s client is null, then return true.
  if (!factory_params_->client_security_state)
    return true;

  // [spec]: 3. If request’s client’s policy container’s embedder policy’s value
  //            is not "credentialless", then return true.
  if (factory_params_->client_security_state->cross_origin_embedder_policy
          .value != mojom::CrossOriginEmbedderPolicyValue::kCredentialless) {
    return true;
  }

  // [spec]: 4. If request’s origin is same origin with request’s current URL’s
  //            origin and request does not have a redirect-tainted origin, then
  //            return true.
  url::Origin request_initiator =
      url_request_->initiator().value_or(url::Origin());
  if (request_initiator.IsSameOriginWith(url))
    return true;

  // [spec]: 5. Return false.
  return false;
}

}  // namespace network
