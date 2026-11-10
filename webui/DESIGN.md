# WebUI design notes

The direction, the dials and a one-line reason for every decision that
shapes the page. Consolidated from the product owner's direction and
antislop audit 001 (2026-11-08); before this file the reasons lived in
scattered code comments, and some decisions had none.

## Direction

Modern minimalist, clean and professional. A utility dashboard for a
homebrew PS5 audience, read from a couch or a phone as often as from a
desktop. No decoration that does not carry meaning.

- Palette: `#171717` page, `#202020` cards, `#4F8EF7` accent; the
  palette matches the payload's embedded logo. Dark-only on purpose;
  there is no light mode and none is planned.
- Chips (format / role / platform) are tokens in `index.css`
  (`--chip-*`); chip text is `--chip-fg` so every chip clears WCAG AA.
- Copy: plain English, plain punctuation. No em dashes, no marketing
  adjectives; every number shown comes from the console or the PC.

## Dials (antislop vocabulary)

ENERGY 1 / RHYTHM 1 / MOTION 1-2: one quiet page, few moving parts,
nothing decorative in motion.

## Decisions, one line each

- Mount fade-up on cards (250ms): feedback that the grid just loaded,
  not decoration; disabled under `prefers-reduced-motion`. [R-19]
- Hover lift (-2px) on cards: affordance that the tile opens a dialog;
  disabled under `prefers-reduced-motion`. [R-19]
- Card `shadow-md`, `hover:shadow-xl`: elevation is meaning - cards
  sit one level above the page and the deeper hover shadow is the
  interactivity signal; nothing else on the page floats. [R-12]
- System font stack: zero network fonts on a console browser; native
  fonts are fastest and match the platform.
- Base font size 18px: the page is read at couch/phone distance; 16px
  tested small on the PS5 browser.
- One radius token (0.5rem) shared by cards, buttons, inputs and
  chips, so nothing looks bolted on.
- Tailwind v3 (not v4) and ES2019 build target: the PS5 browser is
  WebKit at roughly Safari 13; v4 needs Safari 16.4+ (cascade layers,
  color-mix). Do not bump without checking caniuse.
- Dark-only (`:root` and `.dark` carry identical values): the
  dashboard lives next to the console; a second theme would double
  the surface to test for no one asking for it.
- Wire behavior (endpoints, query strings, 2s poll) is unchanged from
  the pre-React page: the UI may move, the protocol may not.

## Working on the page

Edit `src/`, then run `npm run build:embed` and commit the refreshed
`payload/webui.html` together with your change; CI fails a PR whose
embed is stale. If the page changes what the user sees, bump
`RECEIVER_BUILD` in `payload/common.h` (date-style tag mid-cycle,
e.g. `20261108-01`; release prep turns it into `vX.Y.Z`) so a cached
old page reports MISMATCH instead of pretending to be current.
