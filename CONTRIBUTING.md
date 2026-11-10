# Contributing to PS5 Package Receiver

Thanks for wanting to help! The receiver is a small plain-C payload with a
React page on top, and there is useful work at every level — from "I tried
it on my console and here is what happened" to a full pull request. This
guide walks you through it. If you just want to report something, jump
straight to [Reporting bugs](#reporting-bugs).

One rule before anything else: be kind. The
[code of conduct](CODE_OF_CONDUCT.md) covers every issue, pull request and
message in this project.

## Where things live

| Repository | What it is |
| --- | --- |
| [`AldySan/ps5-package-receiver-elf`](https://github.com/AldySan/ps5-package-receiver-elf) | The receiver: the ELF payload that runs on the console. You are here. |
| [`AldySan/ps5-package-sender`](https://github.com/AldySan/ps5-package-sender) | The PC application that sends packages. Issues about the PC app belong there. |

Both halves speak the same protocol. If a problem is ambiguous (an install
that never starts, say), open the issue here and mention which side you
were on — the receiver's notification carries its build tag, and the PC
app shows its version too.

## Ways to help

- **Test on real hardware.** Reports like "works on FW 10.40 with etaHEN"
  or "fails with 0x80B21104 on a PS5 Pro" are genuinely valuable. A lot
  of what this project knows about the console was measured by users.
- **Report bugs and request features.** The issue templates ask for
  exactly what helps; filling them in well is a real contribution.
- **Improve the docs.** Small wording fixes count.
- **Send code.** The rest of this file is for you.

## Setting up

Any Linux machine (or WSL) works. You need:

- for the payload: GNU make, `wget` and `unzip`, and clang + lld 18
  (packaged as `clang-18` and `lld-18` on many distributions);
- for the web dashboard: Node.js 22 and npm.

Fork the repository on GitHub, then:

```sh
git clone https://github.com/<your-name>/ps5-package-receiver-elf.git
cd ps5-package-receiver-elf
git remote add upstream https://github.com/AldySan/ps5-package-receiver-elf.git
```

`dev` is the default branch. Branch from it and open pull requests
against it. `main` only receives release commits, so never target it
directly.

## Project layout

```text
payload/              the receiver itself (plain C)
├─ main.c             startup, self-replacement, connection loop
├─ server.c           HTTP routing (one handler per endpoint)
├─ handlers_*.c       endpoint handlers, split by theme
├─ installer.c        Sony's installer, staged fallback, home-screen shortcut
├─ pull.c             segmented copies into /data/homebrew
├─ files.c            path jailing for every file operation
├─ discovery.c        LAN beacon + PC announcement listener
├─ notify.c           console notifications
├─ webui.c            embeds webui.html into the ELF
└─ webui.html         the dashboard page, built from webui/
webui/                dashboard source (React + TypeScript)
.github/              CI workflows, issue templates, pull-request template
```

## Building the payload

```sh
curl -fsSL -o ps5-payload-sdk.zip \
  https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip
sudo unzip -q -o ps5-payload-sdk.zip -d /opt

make -C payload PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk          # -> payload/pkg-receiver.elf
make -C payload PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk TEST_ONLY=1
```

The build runs with `-Wall -Werror`: a warning is an error. Fix warnings,
don't silence them. Every pull request is built this same way by CI, so
you always know whether your change compiles even without a console.

## Working on the web dashboard

```sh
cd webui
npm install
npm run dev          # local dev server at http://localhost:5173
npm run build:embed  # typecheck + build + refresh payload/webui.html
```

Three rules save everyone time:

1. The dev server has no receiver behind it, so `/api/...` calls fail
   until the page runs against a real console. That is expected.
2. `payload/webui.html` is committed to the repository. Whenever you
   change `webui/src/`, run `npm run build:embed` and commit the
   refreshed `payload/webui.html` together with your change. CI rebuilds
   the page and fails the pull request if the committed one is stale.
3. If the page's behavior changes, bump `RECEIVER_BUILD` in
   `payload/common.h` (date-style tag, e.g. `20261108-01`) so a cached
   old page reports a MISMATCH instead of pretending to be current, then
   re-run `build:embed`.

Two deliberate constraints worth knowing before you touch the page:

- **Tailwind stays on v3, and the build target stays ES2019.** The PS5
  browser is a WebKit build roughly at Safari 13; Tailwind v4 and newer
  JavaScript silently break there.
- **Dark theme only, plain English copy, no em dashes.** The reasons
  live in `webui/DESIGN.md`, which also explains the visual direction.

`webui/README.md` has the full layout of the folder and how to add
shadcn/ui components.

## Code style

The spirit of the rules is simple: match the file you are editing. When
in doubt, look at a neighboring line and do that.

### The payload (C)

- Tabs for indentation, classic BSD layout: the return type sits on its
  own line, and braces open on their own line.
- `snake_case` for functions and variables, one concern per module —
  keep new endpoints in the right `handlers_*.c` file.
- Comments are `/* ... */` blocks, and they explain **why**, not what.
  Measured console behavior ("Sony's downloader appends `?product=…` to
  every request") is written down in the code on purpose. If you learn
  something new about the console, leave it as a comment for the next
  person.

### The web dashboard (TypeScript)

- Two-space indent, React function components.
- Endpoint calls stay typed in `src/lib/api.ts`; UI pieces live in
  `src/components/`.
- User-facing copy is plain and honest, no marketing adjectives.

### Commit messages

Short conventional commits with a module scope, all lowercase:

```text
fix(installer): say 0x80B21104 needs room for two copies of the package
```

- The subject line says **what** changed; keep it under ~65 characters.
- The body (wrapped at ~72 columns) says **why**, in plain words. Skip it
  only when the subject truly is the whole story.
- Common types: `fix`, `feat`, `docs`, `test`, `chore`. Scopes are module
  names: `installer`, `webui`, `server`, `pull`, `discovery`, `files`,
  `http`, `notify`.
- Reference issues as `Refs #59.`, or `Fixes #59.` when the change closes
  one.

## Sending a pull request

1. Branch from `dev`:

   ```sh
   git switch -c fix/my-thing dev
   ```

2. Make the change and build it (payload, web dashboard, or both).
3. Push to your fork and open a pull request **against `dev`**.
4. Fill in the template: what changed, why, and how you tested it.
5. Keep CI green. It builds both ELFs, typechecks the page, and verifies
   the embedded `webui.html` is up to date.

Reviews are friendly and usually quick. Pull requests are merged with a
merge commit, so your branch history is preserved as-is.

## Reporting bugs

Open an issue and pick the **Bug report** template — it asks for the
details that actually matter when debugging. The two most important:

- **The receiver's build tag**, shown in every console notification, e.g.
  `PS5 PKG Receiver 20261108-01`.
- **Console model, firmware and payload loader** (etaHEN, kstuff, ...),
  plus how the install was started (PC app or browser).

Photos of the TV screen are welcome; error codes like `0x80B21104` even
more so.
