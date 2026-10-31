# WebUI (React + Vite + TypeScript + Tailwind + shadcn/ui + Motion)

This folder is the source of the dashboard the payload serves at
`http://<console-ip>:12800/`. It is a normal Vite project — edit the code,
run it locally, and when you are happy, bake it into the payload.

## Quick start

```bash
npm install          # once
npm run dev          # dev server at http://localhost:5173
```

The dev server proxies nothing — `/api/...` calls will fail until the page
runs against a real payload. The easiest way to work against a real
receiver is to open the dev page and let the PC/catalog parts fail
gracefully, or temporarily point `api.ts` at a payload URL.

## Baking into the payload

```bash
npm run build:embed
```

This runs `tsc` + `vite build`, reads the `RECEIVER_BUILD` tag from
`payload/common.h`, bakes it into the bundle (the header shows
`page X • receiver Y` and warns on mismatch), and copies the output to
`payload/webui.html`. That file is committed to the repo and embedded
into the ELF with `.incbin` (see `payload/webui.c`), so:

- `make` in `payload/` never needs Node — `webui.html` is just a file.
- Always commit the refreshed `payload/webui.html` together with your
  `src/` changes. CI rebuilds it and fails the PR if they drift apart.
- Bump `RECEIVER_BUILD` in `payload/common.h` whenever the page's
  behavior changes, then re-run `build:embed` so the tags stay in sync.

The whole app is inlined into a single `index.html`
(`vite-plugin-singlefile`) — the payload serves exactly one page.

## Why Tailwind v3 (not v4)?

The PS5 console browser is a WebKit build roughly at Safari 13 level.
Tailwind v4's CSS requires Safari 16.4+ (cascade `@layer`, `color-mix`),
which would silently drop most styling on the console. v3 output is
plain CSS that works there. The same reasoning applies to the ES2019
build target in `vite.config.ts` — don't raise it without checking
caniuse for Safari 13.

## Adding shadcn/ui components

`components.json` is wired for the shadcn CLI:

```bash
npx shadcn@latest add <component>
```

Hand-written copies live in `src/components/ui/` (button, badge, input,
dialog, alert-dialog, progress, tabs, skeleton, sonner) so the bundle
stays small.

## Layout

```
src/
  App.tsx              page assembly, filters, the three dialogs
  lib/receiver.ts      the "brain": PC discovery, installs, copies, polling
  lib/api.ts           typed wrappers for the payload's endpoints
  lib/types.ts         catalog/status reply shapes
  lib/format.ts        size and speed formatting
  components/          Header, PcRow, FilterBar, StatusArea, cards, dialog
  components/ui/       shadcn/ui primitives
scripts/embed.mjs      build + tag + copy into payload/webui.html
```
