# Google CAPTCHA / "I'm Not a Robot" — QNX/BB10 Investigation Log

> **Decision (2026-07-03): TABLED.** Route around Google (YT Search shim + DuckDuckGo HTML).
> See `/root/berry-agent-handoff-google-captcha.md` for mechanism-vs-adversary framing,
> hard-stop on desktop-UA A/B, and `berry-decode.debug` probe usage.

## Symptom
On Google search, Berry gets redirected to `/sorry/index` (HTTP 429), shows reCAPTCHA
checkbox, user taps it, spinner runs forever. Same on headless and interactive.

## Root causes identified

### 1. User-Agent / Client Hints mismatch (FIXED)
**Before:** HTTP UA said `Chrome/999.0.0.0` but Sec-CH-UA advertised
`content_shell` + `platform=Unknown`. Google flags this immediately.

**After (2026-06-24):** QNX embedder emits aligned Chrome Linux desktop fingerprint:
```
BerryShell: UA="Mozilla/5.0 (X11; Linux armv7l) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36" platform=Linux brands=[Google Chrome/120, Chromium/120, Not-A.Brand/99]
```

Files: `content/shell/browser/shell_content_browser_client.{cc,h}`,
`content/shell/browser/shell_browser_main_parts.cc`

### 2. Still blocked after UA fix
Even with correct UA, Google still redirects search → `/sorry` (429). Likely IP /
device reputation + non-Chrome fingerprint signals (canvas, WebGL, missing attestation).

### 3. reCAPTCHA Timeout — NOT a touch issue
Device log shows **no touch events** during CAPTCHA window. Failures are JS-side:
```
SyntaxError: Unexpected token ... is not valid JSON  (recaptcha anchor + sorry page)
Error: reCAPTCHA Timeout / (b) / (f)
ReferenceError: solveSimpleChallenge is not defined
```

### 4. Compression ruled out for main-frame HTML
With `enable_brotli=false` and `enable_zstd=false` on QNX network context:
```
BerryNav: CaptchaResp code=200 enc=(none) type=text/html; charset=utf-8 url="...recaptcha/enterprise/anchor..."
BerryNav: CaptchaResp code=429 enc=(none) type=text/html url="...google.com/sorry/index..."
```
Main-frame HTML is uncompressed and loads. Garbled JSON comes from **reCAPTCHA's
internal XHR/fetch** (likely attestation / token protobuf or encrypted payload parsed
as JSON). Next investigation: log subresource XHR responses on `recaptcha` POST URLs.

## Changes shipped (2026-06-24)
| Change | File |
|--------|------|
| Chrome/120 UA + Client Hints on QNX | shell_content_browser_client.cc |
| Startup UA log line | shell_browser_main_parts.cc |
| Disable brotli + zstd on QNX network | shell_content_browser_client.cc |
| CaptchaResp header logging (nav + loader) | navigation_url_loader_impl.cc, throttling_url_loader.cc |

## Test protocol
```bash
# On device after deploy:
rm -f .../Default/Cookies .../Default/Network/Cookies
# Launch, search google.com for "hi", watch berry-kbd.log:
grep -E 'BerryShell: UA|CaptchaResp|sorry|429|reCAPTCHA|Timeout|SyntaxError' berry-kbd.log
```

## Realistic expectations
- Cannot programmatically bypass Google CAPTCHA.
- UA fix is necessary but not sufficient alone.
- Checkbox spinner = reCAPTCHA token generation failed server-side, not bad touch coords.
- DuckDuckGo / other sites unaffected by this Google-specific bot gate.

## DEFINITIVE ROOT CAUSE (2026-06-24, second pass)

The earlier "garbled JSON = undecoded `0x78 0x9c` zlib stream" reading was a
**misinterpretation**. Captured live on device, the garbage first bytes vary
every run (`"y"..`, `"<Uh>.."`, `"G .."`, `"̷룟6.."`) — i.e. random binary, NOT a
fixed compression header. We instrumented the network service and probed the
JS engine to settle it conclusively.

### Evidence (all captured on the Passport, single-process build)
1. **Web Compression Streams API works.** `deploy/decompress_probe.html` round-trips:
   ```
   PROBE: deflate=PASS  deflate-raw=PASS  gzip=PASS  deflate-roundtrip=PASS
   ```
2. **WebCrypto works (bit-exact).** Same probe:
   ```
   PROBE: sha256=PASS (matches NIST "abc" vector)
   PROBE: aes-gcm=PASS  aes-gcm-vector=PASS (hardcoded AES-256-GCM zero vector)
   ```
3. **Every reCAPTCHA network response is clean and uncompressed.** Temporary
   logging in `services/network/url_loader.cc` (`OnResponseStarted` +
   `DidRead`, keyed on URL and on json/protobuf/octet-stream content-types)
   showed for all of enterprise.js / recaptcha__en.js / anchor / styles / png /
   webworker.js / sorry:
   ```
   CaptchaResp ... enc=(none) ...
   CaptchaBody firstbytes=[3c 21 44 4f ...]  ("<!DOCTYP")  / "(functio" / PNG magic
   ```
   No response carried compressed or garbled bytes at the network boundary.
4. **The failing reload/userverify XHR is never issued.** With the content-type
   filter widened to json/octet-stream, zero such requests appear. reCAPTCHA
   throws `SyntaxError` on the **anchor init** (decoding Google's `s=` token in
   the anchor URL) *before* any reload happens.

### Conclusion
The QNX/BB10 browser's transport, decompression, and crypto are all correct.
The garbled-JSON is generated **inside reCAPTCHA's own obfuscated JS** when it
tries to make sense of a token Google deliberately poisoned: the search request
is flagged `429 -> /sorry` **before reCAPTCHA even runs**, so reCAPTCHA is handed
data it cannot resolve and `JSON.parse` chokes. This is Google's anti-bot gate
(IP/device reputation + missing attestation/Botguard signals), **not a fixable
browser defect**. No transport/decode code change was shipped (the diagnostic
logging in `url_loader.{cc,h}` was reverted).

### What would (and would not) help
- Would NOT help: fixing decompression/crypto (already correct), UA tweaks
  alone (429 precedes reCAPTCHA), forcing identity encoding.
- Might marginally help but unverified/adversarial: cleaner IP reputation,
  full Botguard/attestation support, canvas/WebGL fingerprint parity. Out of
  scope per decision to fix real bugs rather than chase the CAPTCHA bypass.
- Practical: other search engines / sites are unaffected; this gate is
  Google-specific.

### Reusable tool
`deploy/decompress_probe.html` — push to `/accounts/1000/shared/misc/` and open
via `file://` to re-verify Compression Streams + WebCrypto on any future build.

## Request-fingerprint alignment to Chrome 120 (2026-06-24, third pass)

We chased the pre-reCAPTCHA `429` by aligning the request fingerprint to real
Chrome. Two concrete divergences were found and fixed:

### 1. No Client Hints were being sent (FIXED)
`content_shell` never registers a `ClientHintsControllerDelegate`
(`set_client_hints_controller_delegate` is never called), so
`GetClientHintsControllerDelegate()` returned `nullptr` and the UA-hint block in
`content/browser/client_hints/client_hints.cc` was skipped entirely. Result: a
`Chrome/120` User-Agent with ZERO `Sec-CH-UA*` headers - a blatant bot tell.

Fix: `content/shell/browser/shell_browser_context.cc` now returns a
`MockClientHintsControllerDelegate(GetShellUserAgentMetadata())` on QNX (owned
by the context). content_shell already links that mock target (sibling Mock*
delegates). Low-entropy hints are sent by default once a delegate exists.

### 2. Accept-Encoding was forced to `identity` (FIXED)
`net/http/http_request_headers.cc` `SetAcceptEncodingIfMissing()` had a QNX hack
forcing `Accept-Encoding: identity` on every request (a leftover from the
debunked "compressed payload" theory). Chrome never sends `identity`. Removed
it, and re-enabled `enable_brotli`/`enable_zstd` in
`shell_content_browser_client.cc` `ConfigureNetworkContextParamsForShell`.

### Before -> after (captured on device, google.com/search)
```
BEFORE: search?q=... -> 429 /sorry immediately.
        (no sec-ch-ua; Accept-Encoding absent/identity)
AFTER:  BerryHdr: chua="Google Chrome";v="120", "Chromium";v="120", "Not-A.Brand";v="99"
                  chp="Linux" chm="?0" ae="gzip, deflate, br, zstd"
                  ua="...Chrome/120.0.0.0 Safari/537.36"
        search?q=hello+world -> code=200  (previously 429!)
        ...but a follow-up navigation to /sorry?...&sei=... still returns 429.
```

### Outcome
Header/fingerprint alignment is a real, measurable improvement: the search
request itself now returns 200 instead of an instant 429. However Google still
bounces a subsequent navigation to `/sorry` (429), confirming the residual gate
is driven by IP/device reputation and behavioral/TLS-JA3/HTTP2 signals, not the
request headers. These are the adversarial signals we scoped out. The Client
Hints + Accept-Encoding fixes are kept (they make the browser more Chrome-like
and benefit all sites); the throwaway `BerryHdr` header logging was removed.

## JS-environment fingerprint alignment (2026-06-24, fourth pass)

Sign-in still said "this browser or app may not be secure" and the reCAPTCHA
checkbox kept spinning. An on-device JS probe (`deploy/fp_probe.html`, run via
the app launcher) showed *why* — `content_shell` did not look like Chrome to
page JavaScript:

```
BEFORE (app launcher path):
  FP: webdriver=true            <- automation-controlled mode (hard bot tell)
  FP: window.chrome=undefined   <- real Chrome always exposes window.chrome
  FP: chrome.runtime=undefined
  FP: chrome.loadTimes=undefined
  FP: chrome.csi=undefined
  FP: plugins.length=0          <- real Chrome ships the internal PDF viewer
  FP: mimeTypes.length=0
  FP: pdfViewerEnabled=false
  FP: platform=Linux armv7l     <- mismatches the X11; Linux x86_64 UA
```

### Fixes
1. **`navigator.webdriver=true` (root cause).** The app launcher
   (`deploy/berry-shell-bar/launcher.c`) passed `--remote-debugging-port=0`,
   which puts Chromium into automation-controlled mode (sets
   `navigator.webdriver=true`) and opens a CDP port. Removed the flag. This is
   why the earlier bundle-path probe showed `webdriver=false` but the tapped-app
   path showed `true` — only the launcher added that switch.
2. **Renderer JS shim.** `content/shell/renderer/shell_render_frame_observer.cc`
   `DidClearWindowObject()` now (QNX-only) injects a document-start script that:
   - forces `navigator.webdriver=false` (belt-and-suspenders, covers every
     launch path regardless of flags),
   - adds `window.chrome` with `runtime`, `loadTimes()`, `csi()`, `app`,
   - fakes the 5 internal-PDF-viewer `navigator.plugins` + 2 `mimeTypes` and
     `pdfViewerEnabled=true`,
   - aligns `navigator.platform` to `Linux x86_64` to match the spoofed UA.
   Everything is wrapped in try/catch so it can never break page scripts.

### Before -> after (on device, app launcher path)
```
AFTER:
  FP: webdriver=false
  FP: window.chrome=object
  FP: chrome.runtime=object / loadTimes=function / csi=function
  FP: plugins.length=5
  FP: mimeTypes.length=2
  FP: pdfViewerEnabled=true
  FP: platform=Linux x86_64
```

### Expectation / ceiling
These changes remove the obvious JS-detectable "this is not Chrome" tells, which
is exactly what Google's secure-browser sign-in check and reCAPTCHA's checkbox
flow inspect first, so they should materially improve those flows. They cannot
beat the deeper adversarial layer (server-side TLS JA3/HTTP2 fingerprint, IP/
device reputation, behavioral scoring) — if the spinner/`/sorry` 429 persists it
is coming from that layer, not from the JS surface. Needs an on-device user
retest of Google sign-in + the reCAPTCHA checkbox.

## DEFINITIVE root-cause pass (2026-06-24, fifth pass) — reCAPTCHA spinner

The fingerprint work got us *past the gate* (anchor now loads, `code=200`), but
the checkbox still spins and the console shows, repeatedly:
```
Uncaught (in promise) SyntaxError: Unexpected token '...', "򢝡򥳿 𘻴䯪I"... is not valid JSON
Uncaught (in promise) Error: reCAPTCHA Timeout
```
We instrumented and ruled out EVERY plausible browser-side cause, on-device:

### 1. Network transport is clean (NOT a decode bug)
Added one-shot `BerryNav: CaptchaBody` logging in `services/network/url_loader.cc`
`DidRead()` — dumps `Content-Encoding` + first post-//net-decode body bytes for
`/recaptcha/` + `/sorry` URLs. Every body arrives correctly decoded:
```
/sorry/index    enc=(none) text/html   -> "<!DOCTYPE html PUBLIC \"-"
enterprise.js   enc=gzip               -> "/* PLEASE DO NOT COPY AN"
recaptcha__en.js enc=gzip              -> "(function(){/*\n\n Copyrig"
anchor          enc=gzip text/html     -> "<!DOCTYPE HTML><html dir"
styles.css/logo.png/webworker.js       -> all clean (PNG magic 89504e47, etc.)
```
gzip decode works perfectly. The brotli theory was WRONG.

### 2. Every client-side primitive works (`deploy/wasm_probe.html`)
Main thread: `WebAssembly.validate=true`, `wasm.add(2,3)=5`, `TextEncoder/Decoder
OK`, `atob/btoa OK`, `SHA-256 OK`, AES-`GCM`/`CTR`/`CBC` encrypt+decrypt all `OK`.

### 3. Web Workers work too (`deploy/worker_probe.html`)
Dedicated worker (Blob URL): `wasm=5`, `SHA-256 OK`, `AES-GCM OK`, and
`fetch()+response.json() OK` — all inside the worker.

### 4. reCAPTCHA self-aborts during init, before any challenge fetch
With a 50s window, reCAPTCHA NEVER issues a `reload` / `payload` / `userverify`
request. It loads the scripts + anchor (clean), throws the `SyntaxError` while
processing the encrypted `s=` state token it already holds, and times out. The
identical garbled-JSON failure was present BEFORE any fingerprint spoofing.

### Conclusion (adversarial ceiling)
This is not a missing or broken browser capability. Network is clean, gzip/WASM/
WebCrypto/TextDecoder/base64/Workers all function. reCAPTCHA's own obfuscated
protection code refuses to proceed on this client — consistent with the server
issuing a poisoned/undecodable state token to a client it has already classified
as non-genuine (BB10/QNX device + ARM + headless-derived content_shell + no real
Google account session). No client-side patch can manufacture the trust signal
reCAPTCHA wants; this is by design. The CaptchaBody probe is QNX-gated + one-shot
and left in place for any future re-check.
