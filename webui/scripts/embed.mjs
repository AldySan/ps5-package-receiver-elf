/* Build the React UI and place the single-file output where the
 * payload expects it (payload/webui.html, embedded via .incbin).
 *
 * The page shows "page <tag> • receiver <tag>" and warns on mismatch,
 * so the same RECEIVER_BUILD from payload/common.h (the non-TEST one)
 * is baked into the JS bundle via vite's `define`.
 *
 * Workflow: edit src/ -> npm run build:embed -> commit the refreshed
 * payload/webui.html together with your changes. */
import { copyFileSync, readFileSync } from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";

const webui = path.resolve(import.meta.dirname, "..");
const repo = path.resolve(webui, "..");

/* read RECEIVER_BUILD, skipping the TEST_ONLY #ifdef block */
const lines = readFileSync(path.join(repo, "payload", "common.h"), "utf8")
  .split("\n");
let skipping = false;
let build = null;
for (const line of lines) {
  if (/^#ifdef\s+TEST_ONLY/.test(line)) {
    skipping = true;
  } else if (skipping && /^#else/.test(line)) {
    skipping = false;
  } else if (!skipping) {
    const m = line.match(/^#define RECEIVER_BUILD "([^"]+)"/);
    if (m) {
      build = m[1];
      break;
    }
  }
}
if (!build) throw new Error("RECEIVER_BUILD not found in payload/common.h");
console.log("page build:", build);

const res = spawnSync("npm", ["run", "build"], {
  cwd: webui,
  stdio: "inherit",
  env: { ...process.env, PAGE_BUILD: build },
  shell: process.platform === "win32",
});
if (res.status !== 0) throw new Error("webui build failed");

copyFileSync(
  path.join(webui, "dist", "index.html"),
  path.join(repo, "payload", "webui.html"),
);
console.log("wrote payload/webui.html");
