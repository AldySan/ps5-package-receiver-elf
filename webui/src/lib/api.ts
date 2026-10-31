/* Thin typed wrappers over the payload's endpoints (payload/handlers_*.c).
 * Replies are intentionally parsed loosely: several endpoints answer
 * with plain "error:..." text, and the page treats those as failures
 * rather than exceptions. */
import type {
  PcReply,
  SpaceReply,
  StatReply,
  StatusReply,
  VersionReply,
} from "./types";

async function getJson<T>(url: string): Promise<T> {
  const r = await fetch(url);
  return JSON.parse(await r.text()) as T;
}

async function postJson(url: string, body: object): Promise<string> {
  const r = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  return r.text();
}

async function getText(url: string): Promise<string> {
  const r = await fetch(url);
  return r.text();
}

export const api = {
  version: () => getJson<VersionReply>("/api/version"),

  pc: () => getJson<PcReply>("/api/pc"),

  space: () => getJson<SpaceReply>("/api/space"),

  status: () => getJson<StatusReply>("/api/status"),

  /* file exists/size under /data/homebrew (replies "error:..." on a
   * bad path — same as "not there" for the caller) */
  stat: async (path: string): Promise<StatReply> => {
    const t = await getText(
      "/api/files/stat?path=" + encodeURIComponent(path),
    );
    if (t.startsWith("error:")) return { exists: false, size: 0 };
    return JSON.parse(t) as StatReply;
  },

  /* GET /install — plain-text reply "ok: ..." / "error:..." */
  install: (url: string, name: string, icon?: string) => {
    let q =
      "/install?url=" +
      encodeURIComponent(url) +
      "&name=" +
      encodeURIComponent(name);
    if (icon) q += "&icon=" + encodeURIComponent(icon);
    return getText(q);
  },

  /* start a PC -> console copy; reply contains "started" on success */
  pullStart: (url: string, path: string, mode: string) =>
    postJson("/api/files/pull", { url, path, mode }),

  pullPause: (paused: boolean) =>
    postJson("/api/pull/pause", { paused: paused ? 1 : 0 }),

  pullCancel: () => fetch("/api/pull/cancel", { method: "POST" }).then(() => {}),

  catalog: (pc: string) =>
    fetch("http://" + pc + ":9898/catalog").then((r) => r.json()),
};
