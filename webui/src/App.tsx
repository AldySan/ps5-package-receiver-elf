import * as React from "react";
import { famOf, isBase, qTotal, useReceiver } from "@/lib/receiver";
import { fmtSize } from "@/lib/format";
import type { CatalogEntry } from "@/lib/types";
import { Header } from "@/components/Header";
import { PcRow } from "@/components/PcRow";
import { FilterBar, type Kind, type Plat } from "@/components/FilterBar";
import { StatusArea } from "@/components/StatusArea";
import { GameCard } from "@/components/GameCard";
import { ImageCard } from "@/components/ImageCard";
import { GameDialog } from "@/components/GameDialog";
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from "@/components/ui/alert-dialog";
import { Button } from "@/components/ui/button";

export default function App() {
  const r = useReceiver();

  /* filters */
  const [q, setQ] = React.useState("");
  const [plat, setPlat] = React.useState<Plat>("all");
  const [kind, setKind] = React.useState<Kind>("games");

  /* dialogs */
  const [openEntry, setOpenEntry] = React.useState<CatalogEntry | null>(null);
  const [pendingAll, setPendingAll] = React.useState<CatalogEntry[] | null>(
    null,
  );
  const [copyAsk, setCopyAsk] = React.useState<{
    g: CatalogEntry;
    state: "same" | "partial";
  } | null>(null);

  /* the visible slice of the catalog (same rules as the old page) */
  const list = React.useMemo(() => {
    const ql = q.trim().toLowerCase();
    const matchQ = (g: CatalogEntry) =>
      !ql || (g.title + " " + (g.titleId || "")).toLowerCase().indexOf(ql) >= 0;
    const matchP = (g: CatalogEntry) => plat === "all" || g.platform === plat;

    const src = r.catalog.filter((g) => matchP(g) && matchQ(g));
    const byTitle = (a: CatalogEntry, b: CatalogEntry) =>
      a.title.toLowerCase() < b.title.toLowerCase() ? -1 : 1;

    if (kind === "images")
      return src.filter((g) => g.format !== "pkg").sort(byTitle);
    return src
      .filter((g) => g.format === "pkg" && isBase(g))
      .sort(byTitle);
  }, [r.catalog, q, plat, kind]);

  /* "N games" / "N images" — set imperatively so the flows (install,
   * copy progress) own the same line in between, like the old page */
  React.useEffect(() => {
    if (!r.catalogLoaded) return;
    if (list.length === 0) {
      r.setMsg({
        text: r.catalog.length
          ? "No match."
          : "Library is empty — tick Publish library in PKG Sender.",
        kind: "info",
      });
    } else {
      r.setMsg({
        text: list.length + (kind === "images" ? " images" : " games"),
        kind: "info",
      });
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [list, r.catalogLoaded, r.catalog.length, kind]);

  const family = React.useMemo(
    () => (openEntry ? famOf(r.catalog, openEntry) : []),
    [r.catalog, openEntry],
  );

  /* install actions */
  const installOne = (g: CatalogEntry) => {
    void r.installEntry(g);
  };
  const askInstallAll = (base: CatalogEntry) => {
    setOpenEntry(null);
    setPendingAll([base, ...famOf(r.catalog, base)]);
  };
  const confirmInstallAll = () => {
    const list = pendingAll;
    setPendingAll(null);
    if (list) void r.runInstallAll(list);
  };

  /* copy actions */
  const askCopy = async (g: CatalogEntry) => {
    const state = await r.prepareCopy(g);
    if (state === "fresh") void r.startCopy(g, "overwrite");
    else setCopyAsk({ g, state });
  };
  const confirmCopy = (mode: "overwrite" | "resume") => {
    const ask = copyAsk;
    setCopyAsk(null);
    if (ask) void r.startCopy(ask.g, mode);
  };

  const setPlatAndKind = (p: Plat) => {
    setPlat(p);
    if (p === "PS4") setKind("games"); /* PS4 has no image formats */
  };

  const pendingTotal = pendingAll ? qTotal(pendingAll) : 0;
  const notEnough =
    pendingTotal > 0 && r.space !== null && pendingTotal > r.space.free;

  return (
    <div className="container py-6">
      <Header
        versionState={r.versionState}
        receiverBuild={r.receiverBuild}
        pc={r.pc}
        pcSource={r.pcSource}
        space={r.space}
        onPcPill={() => r.setPcRowOpen(!r.pcRowOpen)}
      />

      <PcRow
        key={r.pc || "empty"}
        open={r.pcRowOpen}
        value={r.pc}
        onSave={r.savePc}
        onRefresh={r.loadCatalog}
      />

      <FilterBar
        q={q}
        onQ={setQ}
        plat={plat}
        onPlat={setPlatAndKind}
        kind={kind}
        onKind={setKind}
      />

      <StatusArea
        msg={r.msg}
        bar={r.bar}
        copyActive={!!r.status?.pull}
        onPause={() => void r.pauseCopy()}
        onCancel={() => void r.cancelCopy()}
      />

      <div className="mt-4 grid grid-cols-[repeat(auto-fill,minmax(180px,1fr))] items-stretch gap-4">
        {kind === "images"
          ? list.map((g) => (
              <ImageCard key={g.id} g={g} pc={r.pc} onCopy={askCopy} />
            ))
          : list.map((g) => (
              <GameCard
                key={g.id}
                g={g}
                pc={r.pc}
                addons={famOf(r.catalog, g).length}
                onInstall={installOne}
                onOpen={setOpenEntry}
              />
            ))}
      </div>

      <GameDialog
        entry={openEntry}
        family={family}
        onInstall={installOne}
        onInstallAll={askInstallAll}
        onOpenChange={(open) => {
          if (!open) setOpenEntry(null);
        }}
      />

      {/* install-all confirmation */}
      <AlertDialog
        open={!!pendingAll}
        onOpenChange={(open) => {
          if (!open) setPendingAll(null);
        }}
      >
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>
              Install {pendingAll?.length ?? 0} packages?
            </AlertDialogTitle>
            <AlertDialogDescription asChild>
              <div className="max-h-64 overflow-y-auto">
                <ol className="list-decimal space-y-1 pl-5">
                  {pendingAll?.map((m, i) => (
                    <li key={m.id + m.role + i}>
                      {m.title}{" "}
                      <span className="text-muted-foreground">
                        ({m.role})
                      </span>
                    </li>
                  ))}
                </ol>
                {pendingTotal > 0 && (
                  <p className="mt-3 font-semibold">
                    Total size: {fmtSize(pendingTotal)}
                    {r.space && pendingTotal > 0 && (
                      <span className={notEnough ? "text-destructive" : ""}>
                        {" "}
                        — console free: {fmtSize(r.space.free)}
                        {notEnough && " — NOT ENOUGH SPACE!"}
                      </span>
                    )}
                  </p>
                )}
              </div>
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>Cancel</AlertDialogCancel>
            <AlertDialogAction onClick={confirmInstallAll}>
              Install all
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>

      {/* copy destination check */}
      <AlertDialog
        open={!!copyAsk}
        onOpenChange={(open) => {
          if (!open) setCopyAsk(null);
        }}
      >
        <AlertDialogContent>
          {copyAsk && (
            <>
              <AlertDialogHeader>
                <AlertDialogTitle>
                  {copyAsk.state === "same"
                    ? "Already on the console"
                    : "Partial copy on the console"}
                </AlertDialogTitle>
                <AlertDialogDescription>
                  {copyAsk.state === "same"
                    ? `${copyAsk.g.file || copyAsk.g.title} is already there. Overwrite it?`
                    : `${copyAsk.g.file || copyAsk.g.title} is only partially copied. Resume it, or overwrite from zero?`}
                </AlertDialogDescription>
              </AlertDialogHeader>
              <AlertDialogFooter>
                <AlertDialogCancel>Stop</AlertDialogCancel>
                {copyAsk.state === "partial" && (
                  <Button
                    variant="secondary"
                    onClick={() => confirmCopy("overwrite")}
                  >
                    Overwrite from zero
                  </Button>
                )}
                <AlertDialogAction
                  onClick={() =>
                    confirmCopy(copyAsk.state === "partial" ? "resume" : "overwrite")
                  }
                >
                  {copyAsk.state === "partial" ? "Resume" : "Overwrite"}
                </AlertDialogAction>
              </AlertDialogFooter>
            </>
          )}
        </AlertDialogContent>
      </AlertDialog>
    </div>
  );
}
