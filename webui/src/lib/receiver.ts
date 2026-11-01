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

/* same helpers the old page had */
export function lone(g: CatalogEntry): boolean {
  return (g.familyKey || "").indexOf("FILE:") === 0;
}

export function isBase(g: CatalogEntry): boolean {
  return g.role === "Game" || lone(g);
}

function rank(role: string | undefined): number {
  if (role === "Patch") return 1;
  if (role === "DLC") return 2;
  return 0;
}

/* patches + DLC that belong to the same family as `g` */
export function famOf(all: CatalogEntry[], g: CatalogEntry): CatalogEntry[] {
  return all
    .filter(
      (m) =>
        m.format === "pkg" &&
        !isBase(m) &&
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

  /* auto PC (UDP announce relayed by the payload), then manual input */
  const resolvePc = useCallback(async (): Promise<string> => {
    try {
      const j = await api.pc();
      if (j.pc && j.age >= 0 && j.age < 15) {
        setPc(j.pc);
        setPcSource("auto");
        localStorage.setItem("pri_pc", j.pc);
        return j.pc;
      }
    } catch {
      /* fall through to the manual address */
    }
    const m =
      (pcRef.current || localStorage.getItem("pri_pc") || "").trim() || "";
    if (m) {
      setPc(m);
      setPcSource("manual");
      return m;
    }
    setPcSource("none");
    return "";
  }, []);

  const loadCatalog = useCallback(async () => {
    const target = await resolvePc();
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
      setMsg({
        text: "Error: " + ex + " — is Publish library on and the PC reachable?",
        kind: "err",
      });
    }
  }, [loadSpace, resolvePc]);

  const savePc = useCallback(
    (value: string) => {
      const v = value.trim();
      setPc(v);
      if (v) localStorage.setItem("pri_pc", v);
      loadCatalog();
    },
    [loadCatalog],
  );

  /* ── installs ───────────────────────────────────────────────────── */

  /* queue one install; the reply tells us if it was accepted */
  const installOne = useCallback(async (g: CatalogEntry): Promise<boolean> => {
    const host = pcRef.current;
    if (!host) return false;
    const u = "http://" + host + ":" + PC_PORT + "/pkg/" + encodeURIComponent(g.id);
    const icon = g.hasIcon
      ? "http://" + host + ":" + PC_PORT + "/icon/" + encodeURIComponent(g.id)
      : undefined;
    try {
      const t = await api.install(u, g.title, icon);
      if (t.indexOf("ok:") !== 0) {
        setMsg({ text: t, kind: "err" });
        return false;
      }
      return true;
    } catch (ex) {
      setMsg({ text: "Error: " + ex, kind: "err" });
      return false;
    }
  }, []);

  /* install a single package, showing progress until it is done */
  const installEntry = useCallback(
    async (g: CatalogEntry) => {
      if (!(await installOne(g))) return;
      toast.success("Install queued", {
        description: g.title + " — watch the console notifications.",
      });
      setMsg({
        text: "ok: install queued for " + g.title + " — watch the console notifications.",
        kind: "info",
      });
      for (let i = 0; i < 300; i++) {
        await sleep(2000);
        try {
          const s = await api.status();
          if (!s.busy) {
            setMsg({
              text: "Done: " + g.title + " — check the console.",
              kind: "ok",
            });
            toast.success("Installed", { description: g.title });
            loadSpace();
            return;
          }
          setMsg({
            text: "Installing " + g.title + "... (active: " + s.active + ")",
            kind: "info",
          });
        } catch {
          return;
        }
      }
      /* polling window over, install still running: say so instead of
       * leaving "Installing..." on screen forever */
      setMsg({
        text:
          "Still installing " +
          g.title +
          " — watch the console notifications.",
        kind: "info",
      });
    },
    [installOne, loadSpace],
  );

  const waitBusy = useCallback(async (): Promise<boolean> => {
    for (let i = 0; i < 30; i++) {
      await sleep(1000);
      try {
        if ((await api.status()).busy) return true;
      } catch {
        return false;
      }
    }
    return false;
  }, []);

  const waitIdle = useCallback(async (): Promise<boolean> => {
    if (!(await waitBusy())) return false;
    for (let i = 0; i < 600; i++) {
      await sleep(2000);
      try {
        if (!(await api.status()).busy) return true;
      } catch {
        return false;
      }
    }
    return true;
  }, [waitBusy]);

  /* install a game and its add-ons back to back, one at a time */
  const runInstallAll = useCallback(
    async (list: CatalogEntry[]) => {
      const total = qTotal(list);
      let done = 0;
      setQueue({ done: 0, total });
      for (let i = 0; i < list.length; i++) {
        setMsg({
          text:
            "Queue " +
            (i + 1) +
            "/" +
            list.length +
            ": " +
            list[i].title +
            "..." +
            (total > 0
              ? " (" + fmtSize(done) + " of " + fmtSize(total) + " done)"
              : ""),
          kind: "info",
        });
        if (!(await installOne(list[i]))) {
          setQueue(null); /* installOne already showed why it failed */
          return;
        }
        if (!(await waitIdle())) {
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
    [installOne, loadSpace, waitIdle],
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
      if (!host) return;
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
    installEntry,
    runInstallAll,
    prepareCopy,
    startCopy,
    pauseCopy,
    cancelCopy,
  };
}
