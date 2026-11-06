/* The page's brain: PC discovery, catalog, install/copy flows and the
 * 2-second status poll — a React port of the logic that used to live
 * inline in payload/webui.c. Wire behavior is unchanged: same
 * endpoints, same query strings, same polling cadence. */
import { useCallback, useEffect, useRef, useState } from "react";
import { toast } from "sonner";
import { api } from "./api";
import type { CatalogEntry, Msg, SpaceReply, StatusReply } from "./types";
import { fmtSize, fmtSpd } from "./format";

/* PKG Sender's HTTP port on the PC */
const PC_PORT = 9898;

/* formats the console installs as a package (a plain .pkg or a .fpkg);
 * anything else (exfat/ffpkg/ffpfsc/ffpfs) is a raw file copy */
export function isPkgFmt(g: CatalogEntry): boolean {
  return g.format === "pkg" || g.format === "fpkg";
}

/* same helpers the old page had */
export function lone(g: CatalogEntry): boolean {
  return (g.familyKey || "").indexOf("FILE:") === 0;
}

export function isBase(g: CatalogEntry): boolean {
  return g.role === "Game" || lone(g);
}

/* entries that get their own tile in the Games grid: a Game, or an
 * add-on whose base game is missing from the library — otherwise it
 * would be invisible in both tabs. baseKeys = the family keys that
 * have a base game of their own. */
export function isTileBase(
  g: CatalogEntry,
  baseKeys: Set<string>,
): boolean {
  return (
    isPkgFmt(g) &&
    (isBase(g) || !g.familyKey || !baseKeys.has(g.familyKey))
  );
}

function rank(role: string | undefined): number {
  if (role === "Patch") return 1;
  if (role === "DLC") return 2;
  return 0;
}

/* patches + DLC that belong to the same family as `g` */
export function famOf(
  all: CatalogEntry[],
  g: CatalogEntry,
  baseKeys: Set<string>,
): CatalogEntry[] {
  return all
    .filter(
      (m) =>
        isPkgFmt(m) &&
        !isTileBase(m, baseKeys) &&
        m.familyKey === g.familyKey,
    )
    .sort((a, b) => rank(a.role) - rank(b.role));
}

export function qTotal(list: CatalogEntry[]): number {
  let t = 0;
  for (const q of list) t += q.size || 0;
  return t;
}

const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

type VersionState = "checking" | "match" | "mismatch" | "unknown" | "unreachable";

export interface CopyBar {
  done: number;
  total: number;
  paused?: boolean;
}

export function useReceiver() {
  const [receiverBuild, setReceiverBuild] = useState<string | null>(null);
  const [versionState, setVersionState] = useState<VersionState>("checking");
  const [pc, setPc] = useState<string>(
    () => localStorage.getItem("pri_pc") || "",
  );
  const [pcSource, setPcSource] = useState<"auto" | "manual" | "none">("none");
  const [space, setSpace] = useState<SpaceReply | null>(null);
  const [status, setStatus] = useState<StatusReply | null>(null);
  const [catalog, setCatalog] = useState<CatalogEntry[]>([]);
  const [catalogLoaded, setCatalogLoaded] = useState(false);
  const [msg, setMsg] = useState<Msg>({
    text: "Loading...",
    kind: "info",
  });
  const [queue, setQueue] = useState<{ done: number; total: number } | null>(
    null,
  );
  const [pcRowOpen, setPcRowOpen] = useState(false);

  /* the pull we started ourselves (external pulls show up too); want is
   * the catalog size we expect at the end, 0 when unknown */
  const copyRef = useRef<{
    file: string;
    cancelled: boolean;
    want: number;
  } | null>(null);
  /* speed is measured across consecutive status polls */
  const speedRef = useRef<{ got: number; t: number }>({ got: 0, t: 0 });
  const copyVisibleRef = useRef(false);
  const pcRef = useRef(pc);
  pcRef.current = pc;

  const loadSpace = useCallback(async () => {
    try {
      const s = await api.space();
      setSpace(s.free >= 0 ? s : null);
    } catch {
      setSpace(null);
    }
  }, []);

  const checkVersion = useCallback(async () => {
    try {
      const r = await fetch("/api/version");
      const t = await r.text();
      if (t.charAt(0) !== "{") {
        setVersionState("unknown");
        return;
      }
      const j = JSON.parse(t) as { build: string };
      setReceiverBuild(j.build);
      setVersionState(j.build === __PAGE_BUILD__ ? "match" : "mismatch");
    } catch {
      setVersionState("unreachable");
    }
  }, []);

  /* auto PC (UDP announce relayed by the payload), then manual input.
   * An address the user typed and saved sticks: pri_manual keeps any
   * fresh announce from silently switching the library to another PC
   * on the LAN. Clearing the field (or the Auto button) lifts it. */
  const resolvePc = useCallback(async (): Promise<string> => {
    const stored = (localStorage.getItem("pri_pc") || "").trim();
    if (localStorage.getItem("pri_manual") === "1" && stored) {
      setPc(stored);
      setPcSource("manual");
      return stored;
    }
    try {
      const j = await api.pc();
      if (j.pc && j.age >= 0 && j.age < 15) {
        setPc(j.pc);
        setPcSource("auto");
        localStorage.setItem("pri_pc", j.pc);
        localStorage.removeItem("pri_manual");
        return j.pc;
      }
    } catch {
      /* fall through to the stored address */
    }
    if (stored) {
      setPc(stored);
      setPcSource("manual");
      return stored;
    }
    setPcSource("none");
    return "";
  }, []);

  const loadCatalog = useCallback(async (manual?: string) => {
    /* an address the user just typed wins this load — a fresh auto
     * announce from another PC must not silently override it */
    const target = manual || (await resolvePc());
    if (!target) {
      setMsg({
        text: "No PC found — tick Publish library in PKG Sender, or type the PC address.",
        kind: "info",
      });
      setPcRowOpen(true);
      return;
    }
    localStorage.setItem("pri_pc", target);
    setMsg({ text: "Loading...", kind: "info" });
    setCatalog([]);
    try {
      const all = (await api.catalog(target)) as CatalogEntry[];
      setCatalog(all);
      setCatalogLoaded(true);
      loadSpace();
    } catch (ex) {
      const slow =
        typeof ex === "object" &&
        ex !== null &&
        (ex as { name?: string }).name === "AbortError";
      setMsg({
        text: slow
          ? "Error: " +
            target +
            " did not answer — check the address and that Publish library is on."
          : "Error: " + ex + " — is Publish library on and the PC reachable?",
        kind: "err",
      });
      /* the address is the thing to fix — show the input */
      setPcRowOpen(true);
    }
  }, [loadSpace, resolvePc]);

  const savePc = useCallback(
    (value: string) => {
      /* accept a pasted full URL but use the bare host everywhere */
      const v = value
        .trim()
        .replace(/^https?:\/\//i, "")
        .replace(/\/+$/, "")
        .replace(/:\d+$/, "");
      setPc(v);
      if (v) {
        setPcSource("manual");
        localStorage.setItem("pri_pc", v);
        localStorage.setItem("pri_manual", "1");
        loadCatalog(v);
      } else {
        /* an emptied field forgets the choice entirely */
        localStorage.removeItem("pri_pc");
        localStorage.removeItem("pri_manual");
        setPcSource("none");
        loadCatalog();
      }
    },
    [loadCatalog],
  );

  /* back to discovery: forget the pinned address and take whatever
   * announces itself (or say so when nothing does) */
  const useAutoPc = useCallback(() => {
    localStorage.removeItem("pri_pc");
    localStorage.removeItem("pri_manual");
    setPc("");
    setPcSource("none");
    loadCatalog();
  }, [loadCatalog]);

  /* ── installs ───────────────────────────────────────────────────── */

  /* queue one install; returns the install URL when it was accepted
   * (the caller matches it against the status poll to learn the real
   * outcome), or null when it was not */
  const installOne = useCallback(
    async (g: CatalogEntry): Promise<string | null> => {
      const host = pcRef.current;
      if (!host) {
        setMsg({ text: "No PC address — save one first.", kind: "err" });
        return null;
      }
      const u =
        "http://" + host + ":" + PC_PORT + "/pkg/" + encodeURIComponent(g.id);
      const icon = g.hasIcon
        ? "http://" + host + ":" + PC_PORT + "/icon/" + encodeURIComponent(g.id)
        : undefined;
      try {
        const t = await api.install(u, g.title, icon);
        if (t.indexOf("ok:") !== 0) {
          setMsg({ text: t, kind: "err" });
          return null;
        }
        return u;
      } catch (ex) {
        setMsg({ text: "Error: " + ex, kind: "err" });
        return null;
      }
    },
    [],
  );

  /* follow an install we queued (matched by its URL) until the console
   * reports an outcome. Returns the status reply that ended the watch:
   * on newer payloads lastUrl/lastOk/lastErr carry the real result,
   * on older ones the caller can only say "check the console". Returns
   * null when the console stopped answering entirely. */
  const watchInstall = useCallback(
    async (
      url: string,
      progress?: (s: StatusReply) => void,
    ): Promise<StatusReply | null> => {
      let misses = 0;
      for (let i = 0; i < 630; i++) {
        await sleep(2000);
        let s: StatusReply;
        try {
          s = await api.status();
          misses = 0;
        } catch {
          /* one dropped reply is normal mid-install; only give up
           * after a few in a row */
          if (++misses >= 3) return null;
          continue;
        }
        if (s.busy) {
          progress?.(s);
          continue;
        }
        /* our own outcome is on record — that is the verdict */
        if (s.lastUrl === url) return s;
        /* idle without our record: the worker may simply not be up
         * yet; after ~10 s the record is too old to trust either way */
        if (i >= 5) return s;
      }
      /* ~21 min and still busy: the install outlived the watch */
      return {
        busy: true,
        active: 1,
        pull: false,
        pullName: "",
        pullGot: 0,
        pullWant: 0,
        pullPaused: false,
      };
    },
    [],
  );

  /* install a single package, showing progress until it is done */
  const installEntry = useCallback(
    async (g: CatalogEntry) => {
      const u = await installOne(g);
      if (!u) return;
      toast.success("Install queued", {
        description: g.title + " — watch the console notifications.",
      });
      setMsg({
        text: "ok: install queued for " + g.title + " — watch the console notifications.",
        kind: "info",
      });
      const s = await watchInstall(u, (st) => {
        setMsg({
          text: "Installing " + g.title + "... (active: " + st.active + ")",
          kind: "info",
        });
      });
      loadSpace();
      if (!s) {
        setMsg({
          text: "Lost track of " + g.title + " — check the console.",
          kind: "err",
        });
        return;
      }
      if (s.lastUrl === u && s.lastOk === false) {
        const why = s.lastErr ? " — " + s.lastErr : "";
        setMsg({
          text: "Install failed: " + (s.lastName || g.title) + why,
          kind: "err",
        });
        toast.error("Install failed", {
          description: (s.lastName || g.title) + why,
        });
        return;
      }
      if (s.busy) {
        setMsg({
          text:
            "Still installing " +
            g.title +
            " — watch the console notifications.",
          kind: "info",
        });
        return;
      }
      setMsg({
        text: "Done: " + g.title + " — check the console.",
        kind: "ok",
      });
      toast.success("Installed", { description: g.title });
    },
    [installOne, loadSpace, watchInstall],
  );

  /* install a game and its add-ons back to back, one at a time,
   * stopping honestly when one of them fails */
  const runInstallAll = useCallback(
    async (list: CatalogEntry[]) => {
      const total = qTotal(list);
      let done = 0;
      setQueue({ done: 0, total });
      for (let i = 0; i < list.length; i++) {
        const label =
          "Queue " +
          (i + 1) +
          "/" +
          list.length +
          ": " +
          list[i].title +
          "..." +
          (total > 0
            ? " (" + fmtSize(done) + " of " + fmtSize(total) + " done)"
            : "");
        setMsg({ text: label, kind: "info" });
        const u = await installOne(list[i]);
        if (!u) {
          setQueue(null); /* installOne already showed why it failed */
          return;
        }
        const s = await watchInstall(u);
        if (!s) {
          setQueue(null);
          setMsg({
            text:
              "Install queue stopped — could not follow " +
              list[i].title +
              ". Check the console.",
            kind: "err",
          });
          return;
        }
        if (s.lastUrl === u && s.lastOk === false) {
          setQueue(null);
          const why = s.lastErr ? ": " + s.lastErr : "";
          setMsg({
            text:
              "Install queue stopped — " +
              (s.lastName || list[i].title) +
              " failed" +
              why +
              ". Check the console.",
            kind: "err",
          });
          toast.error("Install queue stopped", {
            description: (s.lastName || list[i].title) + why,
          });
          return;
        }
        done += list[i].size || 0;
        setQueue({ done, total });
      }
      setQueue(null);
      setMsg({
        text:
          "Done: " +
          list[0].title +
          " + " +
          (list.length - 1) +
          " add-ons" +
          (total > 0 ? " (" + fmtSize(total) + ")" : "") +
          " — check the console.",
        kind: "ok",
      });
      toast.success("Install queue finished", { description: list[0].title });
      loadSpace();
    },
    [installOne, loadSpace, watchInstall],
  );

  /* ── PC -> console copies (images etc.) ──────────────────────────── */

  /* check the destination before asking the console to copy */
  const prepareCopy = useCallback(
    async (
      g: CatalogEntry,
    ): Promise<"fresh" | "same" | "partial"> => {
      const file = g.file || g.title;
      try {
        const st = await api.stat("/data/homebrew/" + file);
        if (st.exists && (g.size || 0) > 0 && st.size === g.size)
          return "same";
        if (st.exists && st.size > 0 && st.size < (g.size || 0))
          return "partial";
      } catch {
        /* treat like a fresh copy */
      }
      return "fresh";
    },
    [],
  );

  const startCopy = useCallback(
    async (g: CatalogEntry, mode: "overwrite" | "resume") => {
      const host = pcRef.current;
      if (!host) {
        setMsg({ text: "No PC address — save one first.", kind: "err" });
        return;
      }
      const file = g.file || g.title;
      setMsg({ text: "Copying " + file + "...", kind: "info" });
      try {
        const t = await api.pullStart(
          "http://" + host + ":" + PC_PORT + "/pkg/" + encodeURIComponent(g.id),
          "/data/homebrew/" + file,
          mode,
        );
        if (t.indexOf("started") < 0) {
          setMsg({ text: t, kind: "err" });
          return;
        }
        copyRef.current = { file, cancelled: false, want: g.size || 0 };
        speedRef.current = { got: 0, t: Date.now() };
      } catch (ex) {
        setMsg({ text: "Error: " + ex, kind: "err" });
      }
    },
    [],
  );

  const pauseCopy = useCallback(async () => {
    try {
      await api.pullPause(!status?.pullPaused);
    } catch {
      /* the next poll shows the truth */
    }
  }, [status]);

  const cancelCopy = useCallback(async () => {
    if (copyRef.current) copyRef.current.cancelled = true;
    try {
      await api.pullCancel();
    } catch {
      /* the next poll shows the truth */
    }
  }, []);

  /* ── the 2-second poll: install state + copy progress ────────────── */

  useEffect(() => {
    const iv = setInterval(async () => {
      try {
        setStatus(await api.status());
      } catch {
        /* keep the last known state */
      }
    }, 2000);
    return () => clearInterval(iv);
  }, []);

  useEffect(() => {
    if (!status) return;
    if (status.pull) {
      copyVisibleRef.current = true;
      const now = Date.now();
      const { got, t } = speedRef.current;
      const spd =
        now > t && status.pullGot >= got
          ? " • " + fmtSpd(((status.pullGot - got) * 1000) / (now - t))
          : "";
      speedRef.current = { got: status.pullGot, t: now };
      const name = copyRef.current?.file || status.pullName || "file";
      const pp = status.pullPaused ? " (paused)" : "";
      setMsg({
        text:
          status.pullWant > 0
            ? "Copying " +
              name +
              ": " +
              Math.floor((status.pullGot * 100) / status.pullWant) +
              "% (" +
              fmtSize(status.pullGot) +
              " / " +
              fmtSize(status.pullWant) +
              ")" +
              spd +
              pp
            : "Copying " + name + ": " + fmtSize(status.pullGot) + spd + pp,
        kind: "info",
      });
    } else if (copyVisibleRef.current) {
      copyVisibleRef.current = false;
      const sess = copyRef.current;
      copyRef.current = null;
      speedRef.current = { got: 0, t: Date.now() };
      if (sess?.cancelled) {
        setMsg({ text: "Copy cancelled: " + sess.file + ".", kind: "info" });
        toast.warning("Copy cancelled", { description: sess.file });
      } else if (sess) {
        /* the console toasts the real outcome; when we know the expected
         * size, check it so a failed copy never reads as success here */
        void (async () => {
          let ok = true;
          if (sess.want > 0) {
            try {
              const st = await api.stat("/data/homebrew/" + sess.file);
              ok = st.exists && st.size === sess.want;
            } catch {
              /* cannot verify — trust the console toast */
            }
          }
          if (ok) {
            setMsg({
              text: "Done: " + sess.file + " — check the console.",
              kind: "ok",
            });
            toast.success("Copy finished", { description: sess.file });
          } else {
            setMsg({
              text:
                "Copy did not finish: " +
                sess.file +
                " — check the console.",
              kind: "err",
            });
            toast.error("Copy did not finish", { description: sess.file });
          }
          loadSpace();
        })();
      } else {
        /* a copy somebody else started from elsewhere finished */
        setMsg({ text: "Copy finished — check the console.", kind: "ok" });
      }
    }
  }, [status, loadSpace]);

  /* the progress bar: queue during installs, byte count during copies */
  const bar: CopyBar | null = status?.pull
    ? {
        done: status.pullGot,
        total: status.pullWant,
        paused: status.pullPaused,
      }
    : queue;

  /* bootstrap: version banner + first catalog load */
  useEffect(() => {
    checkVersion();
    loadCatalog();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return {
    /* state */
    receiverBuild,
    versionState,
    pc,
    pcSource,
    space,
    status,
    catalog,
    catalogLoaded,
    msg,
    setMsg,
    bar,
    pcRowOpen,
    setPcRowOpen,
    /* actions */
    loadCatalog,
    savePc,
    useAutoPc,
    installEntry,
    runInstallAll,
    prepareCopy,
    startCopy,
    pauseCopy,
    cancelCopy,
  };
}
