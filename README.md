# PS5 Package Receiver

A small program that runs on a jailbroken PlayStation 5 and installs game
packages (`.pkg` files) over your home network — no USB drive needed.

Run it once and it sits on your console, listening on port 12800. From then
on:

- the **PKG Sender** app on your PC finds the console by itself and sends
  installs to it, or
- you open `http://<your-ps5-ip>:12800` in any browser on the same network
  and install from there.

Everything you start shows up as notifications on the console — queued,
installing, done or failed — and a "pkg remote installer" shortcut can be
added to the home screen so you can reopen the page from the couch.

The console also acts as a receiver for larger files (homebrew disc images
and the like): the PC app can copy them to the console's `/data/homebrew`
folder with several parallel connections, and you can pause, cancel or
resume the copy from either side.

This repository is the console half of [pkg-sender] by Loopayeh, extracted
so the receiver can be developed and improved on its own. The PC
application lives upstream; the receiver speaks the same protocol either
way.

## Requirements

- A PS5 that is already jailbroken, i.e. anywhere you can load custom ELF
  payloads today (etaHEN, kstuff, or whatever setup you normally use).
- A PC (or any machine serving packages) on the same network.
- No extra setup on the console beyond loading the payload.

## Getting the receiver

Easiest: grab a ready-made ELF from the [Releases][releases] page. Each
release carries:

| File | What it is |
| --- | --- |
| `pkg-receiver.elf` | The receiver. Install and copy features enabled. |
| `pkg-receiver-testonly.elf` | Discovery-only build: beacons and status probes work, but every install or file-write request is refused. Handy for testing network discovery without risk. |

Builds in between releases are available from CI: **Actions → build → the
latest run → Artifacts**. Or build it yourself — see
[For developers](#for-developers).

## Running it

Load the ELF the same way you load any other PS5 payload. Two common ways:

- drop `pkg-receiver.elf` into your payload loader's folder (with etaHEN:
  `/data/etaHEN/payloads/`), or
- send it over the network with your usual ELF sender.

On startup the console shows a notification such as
`192.168.1.42:12800 listening`. That address is the one you open in a
browser or point the PC app at. A previous instance is replaced
automatically, so re-sending the payload does not need a reboot.

## How installs happen

Installs go through Sony's own installer (`sceAppInstUtilInstallByPackage`),
the same path a normal download would take. The package can come from:

- an HTTP URL the console fetches itself (the PC app serves packages this
  way), or
- a file already on the console under `/data` (for example pushed there by
  the PC app, or on a USB stick you browsed with another tool).

PS4 packages install the same way; the console handles them natively.

## For developers

The receiver is a handful of small C modules in `payload/` (one per
concern: discovery, HTTP plumbing, per-endpoint handlers, installer,
file transfers) plus the `webui/` React app for the browser dashboard.
It provides:

- an HTTP server on TCP 12800 (WebUI + JSON API),
- a UDP beacon on port 12801, announcing `PKGSENDER v1 <ip>` every 3 s so
  senders can find the console without a scan,
- a listener on UDP 12802 for the PC app's announcements, re-served to
  browsers as `/api/pc`,
- an install queue running each install on its own worker thread,
- a segmented downloader (16 parallel range requests) for large file
  copies into `/data/homebrew`, with pause/cancel/resume,
- path jailing for every file operation: writes never leave
  `/data/homebrew`.

### Building

You need GNU make, `wget`/`unzip`, and an LLVM toolchain with clang and
lld (18 is what the SDK is tested with; many distributions package this as
`clang-18` and `lld-18`).

```sh
curl -fsSL -o ps5-payload-sdk.zip \
  https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip
sudo unzip -q -o ps5-payload-sdk.zip -d /opt

make -C payload PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk          # -> payload/pkg-receiver.elf
make -C payload PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk TEST_ONLY=1
```

Every pull request and push to `dev` is built by CI with the same
toolchain.

### The web page (webui/)

The dashboard you open in the browser is a small React + TypeScript app
(Vite, Tailwind CSS, shadcn/ui components and Motion for animation) in
the `webui/` folder. The build turns it into **one self-contained HTML
file**, `payload/webui.html`, which is embedded into the ELF as raw bytes
— so the payload keeps serving the page with no extra files and no Node
on the console.

Changing the page:

```sh
cd webui
npm install
npm run dev                        # local dev server
npm run build:embed                 # typecheck + build + refresh payload/webui.html
git add -A ../payload ../webui      # commit the baked page together with src/
```

Two things to know:

- The page shows `page <tag> • receiver <tag>` and warns when they differ.
  Both come from `RECEIVER_BUILD` in `payload/common.h` — bump it when
  the page's behavior changes, then re-run `build:embed`.
- CI rebuilds the page and fails the pull request if the committed
  `payload/webui.html` does not match your `src/` — you cannot forget to
  re-bake it.

Tailwind is pinned to v3 on purpose: the PS5 console browser is an older
WebKit, and Tailwind v4's CSS needs a much newer one. Details are in
`webui/README.md`.

### HTTP API

| Request | Purpose |
| --- | --- |
| `GET /` | WebUI |
| `GET /api/status` | install/copy progress, as JSON |
| `GET /api/version` | build tag (compare page vs. receiver) |
| `GET /api/pc` | last heard PC address and age |
| `GET /api/space` | free/total space on `/data` |
| `GET /install?url=…&name=…&icon=…` | queue an install |
| `POST /api/install` | JSON: `{"packages":["http://…"], "name":…, "icon_url":…}` |
| `POST /upload` | form body containing a URL |
| `POST /api/files/pull` | JSON: `{"url":"http://…", "path":"/data/homebrew/…", "mode":"resume"\|"overwrite"}` — copy a file to the console |
| `POST /api/pull/pause` | JSON: `{"paused":1}` or `{"paused":0}` |
| `POST /api/pull/cancel` | stop the running copy |
| `GET /api/files/stat?path=…` | file exists / size |
| `POST /api/files/mkdir` | JSON: `{"path":…}` (under `/data/homebrew`) |
| `POST /api/files/write?path=…&offset=…` | raw chunk append |
| `POST /api/files/done` | JSON: `{"path":…, "size":N}` — verify and notify |

The PC app serves its library on port 9898 (`/catalog`, `/pkg/<id>`,
`/icon/<id>`); the WebUI reads that to show your library with cover art.

### Differences from upstream pkg-sender

Changes made in this repository, all from a code audit of the imported
payload:

- `SIGPIPE` is ignored, so a client hanging up mid-reply can no longer
  kill the receiver (it used to require re-injecting the payload).
- Multi-segment copies write to a `.part` file and are renamed into place
  only when every segment finished. Before, a failed or cancelled copy
  left a full-size file full of holes that later looked "complete" or
  resumed into a corrupt file.
- A request must fully arrive within a time budget (15 s for headers,
  30 s for a body); a client dripping bytes could previously hold the
  single-threaded server indefinitely.
- `GET /api/status` can no longer emit truncated/invalid JSON (larger
  reply buffer; the copy name is sanitized before it is published).
- A second copy request while one is running is rejected instead of
  corrupting both.
- The ~8 MB request buffer is reused across connections instead of being
  allocated and freed for every poll.
- `GET /api/pc` returned invalid JSON (a stray closing brace), which made
  the browser page's automatic PC detection silently fail.
- The temporarily-disabled Files-tab code compiled again: its header was
  committed with template corruption and its implementation was missing
  an include, both unnoticed because the feature is behind an off flag.
- The giant request-routing `if/else` chain became a route table with one
  handler per endpoint, split by theme — same behavior, verified against
  the old chain for 340 method+path combinations.
- `GET /install` (the WebUI's install buttons) sanitizes spaces in the
  URL and icon the same way the JSON route does.
- Copies from servers without byte-range support (e.g.
  `python -m http.server`) fall back to a single stream instead of
  failing every parallel segment.
- A copy name containing control characters can no longer break the
  `/api/status` reply, and a pause pressed just as a copy finished can
  no longer start the next one frozen.

### Branching

`dev` is the working branch; all pull requests target it. `main` is only
used for releases and receives a single squashed commit per release, so
release history stays clean.

## Security notes

The receiver has no authentication: anyone on the network can queue an
install or write into `/data/homebrew`. That is the same trust model as
the upstream project and most LAN payload tools — run it on a network you
control. File operations are jailed to `/data/homebrew`, and the
`TEST_ONLY` build exists precisely for testing discovery without exposing
any write path.

## License

This project is licensed under the [GNU GPL v3](LICENSE).

The receiver payload is derived from [pkg-sender] by Loopayeh, which is
MIT-licensed. Its notice is retained for the derived code:

> Copyright (c) 2026 Loopayeh
>
> Permission is hereby granted, free of charge, to any person obtaining a
> copy of this software and associated documentation files (the
> "Software"), to deal in the Software without restriction, including
> without limitation the rights to use, copy, modify, merge, publish,
> distribute, sublicense, and/or sell copies of the Software, and to
> permit persons to whom the Software is furnished to do so, subject to
> the following conditions:
>
> The above copyright notice and this permission notice shall be included
> in all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
> OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
> MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
> IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
> CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
> TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
> SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

[pkg-sender]: https://github.com/Loopayeh/pkg-sender
[releases]: https://github.com/AldySan/ps5-package-receiver-elf/releases
