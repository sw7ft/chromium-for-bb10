# Log shipping — boot-time device logs to a hosted endpoint

Every Berry Browser launch POSTs the tail (last 192 KB) of the device log
(`shared/misc/berry-kbd.log`) to a small HTTP endpoint you host. Because it
runs in the **launcher before the engine starts**, each upload carries the
**previous session** — including crash backtraces — so you can debug from any
machine without USB/SSH access to the phone.

## How it works

- `launcher.c: berry_ship_log()` — forks a child that reads the log tail and
  does a raw-socket HTTP/1.1 POST. Socket timeouts (5 s) plus an `alarm(15)`
  hard stop mean a dead endpoint can never slow or hang boot. Fire-and-forget.
- Each device gets a persistent id (`berry-logship-id` marker, auto-generated
  from the device model + timestamp, e.g. `q10-48213`) so multiple phones
  don't mix logs.
- The request is `POST /ingest?device=<id>&build=<n>` with the log tail as a
  `text/plain` body. Plain HTTP (no TLS) — put a token in the URL path if the
  endpoint is public, and don't ship logs you consider sensitive.

## Server setup (one time, any VPS)

```sh
PORT=8787 TOKEN=mysecret node deploy/logship-server.js
```

Dependency-free (node >= 10). Stores per-device files in `device-logs/`,
rotates at 25 MB. Browse `http://myhost:8787/?t=mysecret` for the device
list; each device links to its raw log.

## Device setup

In **Settings > Developer**, set the *Log endpoint* field to:

```
myhost.example.com:8787/ingest?t=mysecret
```

(or write it to the marker directly:
`echo "myhost:8787/ingest?t=mysecret" > /accounts/1000/shared/misc/berry-logship-url`)

## Disabling

- Settings > Developer > **Ship log on boot** toggle (writes
  `berry-logship.disable`), or
- clear the endpoint field (no URL = feature dormant).

## Markers

| marker | meaning |
|---|---|
| `berry-logship-url` | `host[:port][/path]` endpoint; empty/missing = off |
| `berry-logship.disable` | kill switch even when a URL is set |
| `berry-logship-id` | auto-generated persistent device id |
