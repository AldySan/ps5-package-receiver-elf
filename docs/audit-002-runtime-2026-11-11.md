# runtime audit 002 — PS5 Package Receiver (payload)

- **Date:** 2026-11-11
- **Scope:** the whole payload runtime — `installer.c`, `pull.c`, `http.c`,
  `server.c`, `files.c`, `handlers_*.c`, `discovery.c`, `notify.c`, `main.c`
  (about 4,800 lines, read in full).
- **Method:** full code inspection against the measured console behavior the
  project has accumulated (ShellCore range fetching, the loopback install
  route, the staged two-copy choreography) and the PC app's wire contract.
  The payload was not executed; both ELF targets build under `-Wall -Werror`
  in CI.
- **Companion audit:** the PC app has its own findings in
  `ps5-package-sender` (`anti-slop/audit-002-2026-11-11.md`); its F-1/F-2
  pair with R-F1 below.

Priority tiers: HIGH = wrong user-facing outcome, hang or crash; MEDIUM =
wrong outcome under real conditions; LOW = hardening.

---

## HIGH

### 1. R-F1 — the last-install outcome died with the process
The record served on `/api/status` (`lastUrl`/`lastOk`/`lastErr`, which the
PC app matches its queue rows against) lived only in memory. A re-injected,
crashed or power-cut receiver came back with no memory of the install it
ran, leaving the PC unable to tell a dead install from a finished one — and
the old PC code then reported "installed" on no evidence (see the companion
audit's F-1).
Fix: fixed — `record_last_install` mirrors the record to
`/data/homebrew/.pkg-receiver-last-install` (temp file plus rename, so a
power cut cannot tear it) and `install_last_restore()` reloads it at boot
before the first status poll (PR #80).

### 2. R-F2 — a dropped connection killed the whole copy
Every pull (Images tab / PC-to-console copy) ran on connections that had to
survive the entire transfer: one dropped or stalled socket failed its
segment, the scattered `.part` was discarded, and the row ended as a failed
copy. This also broke a supported feature: pausing a copy for longer than
the PC's 75-second idle timeout made the PC close the paused sockets, so
unpausing killed the copy.
Fix: fixed — each of the 16 segments (and the single-stream path) now
reconnects with a fresh Range for the bytes it is still missing, up to four
attempts; a 206 is required on every reconnect so a Range-ignoring server
still fails instead of corrupting (PR #81).

## MEDIUM

### 3. R-F3 — the staged pull was declared dead after 60 seconds of silence
After a staged install registers its loopback URL, `staged_install` waits
`LB_FIRST_S` (60 s) for the console's first fetch before failing the
install, unlinking the staged package and stopping the loopback server. A
console that queues that download behind another one can legitimately take
longer than a minute to send its first range request; the staged file was
then deleted under a live intent and the install died with a downloader
error that reads as a firmware bug.
Fix: fixed — the first-fetch window is three minutes; the 300 s mid-pull
idle timeout and the two-hour cap are unchanged (PR #82).

## LOW (deferred, no code change)

### 4. R-F4 — `%00` in a decoded string truncates it
`url_decode` decodes `%00` into a literal NUL, so any path, URL or name
carrying it is silently truncated at that point. Only self-inflicted inputs
can do this (the PC encodes properly); worst case is a wrong-but-contained
path inside the jail. Rejecting or skipping NUL bytes changes shared parsing
behavior for no observed failure.

### 5. R-F5 — unlocked cross-thread string publishes
`g_pull_name` (worker thread) and `g_pc_addr` (UDP listener thread) are read
by the status thread without a lock. A torn read can serve a garbled name or
IP once; the JSON stays valid (both are sanitized/bounded). Cosmetic.

### 6. R-F6 — a hostile PC can slow-drip the stage headers
`stage_fetch` reads headers byte-wise with a 30 s per-recv timeout, so a
server dripping one byte per timeout can stretch the header phase for hours.
The PC is the user's own machine (trusted threat model); the attempt count
bounds it in practice.

### 7. R-F7 — symlink escape from the jail
`jail_path` rejects `..` and outside prefixes, but a symlink planted inside
`/data/homebrew` pointing elsewhere would be followed. Every writer here is
the user's own PC or browser on the LAN; documented as the trust boundary.

## Verified clean (no action)

- **Socket timeouts everywhere**: the API server sets 10 s SO_RCVTIMEO/
  SO_SNDTIMEO per connection, the staging pull 30 s, the loopback server
  60 s; `read_headers` has a wall-clock deadline. No path can wedge the
  single-threaded accept loop.
- **`handle_client` buffer math**: the shared 16 KiB + 8 MiB buffer's
  body/terminator arithmetic is exact (worst case fits with the NUL); short
  bodies are refused rather than served stale.
- **`stage_fetch` resume**: trusts `Content-Range` over the requested offset
  (a hole or a rewind restarts from zero instead of corrupting), gates on
  free space for both console-side copies, unlinks its file on every
  failure, and caps attempts.
- **Loopback server semantics**: clamped overshoot, suffix ranges, 416 with
  `Content-Range: bytes */N`, query-string routing, keep-alive handling —
  the same set the PC's server needed for the console's downloader.
- **Stage hygiene**: boot and pre-install sweeps run only while no staged
  install can own a file (`g_stage_lock`), and `/api/stage/clear` refuses
  while one is live.
- **`queue_install` duplicate guard**: per-URL slots, released by the
  worker that claimed them; a double click stacks a toast, not a second
  multi-gigabyte download.
- **AppInstUtil session handling**: one install per initialized session,
  rebuilt in place (Terminate/dlclose/dlopen/Initialize) before every call
  past the first.
- **`files.c` / handlers**: path jailing on every write-capable endpoint,
  bounded snprintf usage, JSON escaping on every status string.
