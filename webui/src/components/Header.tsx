import type { SpaceReply } from "@/lib/types";
import { fmtSize as fmt } from "@/lib/format";
import { cn } from "@/lib/utils";
import { Eraser } from "lucide-react";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";

type VersionState =
  | "checking"
  | "match"
  | "mismatch"
  | "unknown"
  | "unreachable";

interface HeaderProps {
  versionState: VersionState;
  receiverBuild: string | null;
  pc: string;
  pcSource: "auto" | "manual" | "none";
  space: SpaceReply | null;
  onPcPill: () => void;
  onClearStaged: () => void;
}

/* "page X • receiver Y" — the page and the payload ELF carry the same
 * build tag; a mismatch means the ELF on the console is older than
 * the page you are looking at. */
function versionLine(state: VersionState, build: string | null): string {
  const page = "page " + __PAGE_BUILD__;
  switch (state) {
    case "checking":
      return page + " • receiver ...";
    case "unknown":
      return page + " • receiver: unknown (old payload? resend ELF)";
    case "unreachable":
      return page + " • receiver: unreachable";
    case "match":
      return page + " • receiver " + (build ?? "?");
    case "mismatch":
      return page + " • receiver " + (build ?? "?") + " • MISMATCH: resend the newest ELF";
  }
}

export function Header({
  versionState,
  receiverBuild,
  pc,
  pcSource,
  space,
  onPcPill,
  onClearStaged,
}: HeaderProps) {
  return (
    <header className="flex flex-wrap items-center gap-3 pb-1">
      {/* explicit dimensions so the header cannot shift when the logo
       * loads; the asset is square (512x512) */}
      <img
        src="/logo.png"
        alt="PKG Sender logo"
        width={44}
        height={44}
        className="h-11 w-11 rounded-lg"
      />
      <h1 className="flex-1 text-2xl font-bold leading-tight">
        pkg remote installer
      </h1>
      {/* its own row on narrow screens: full-width basis until md, so
       * the version line stops floating between logo and pill */}
      <span className="basis-full text-xs text-muted-foreground md:basis-auto">
        {versionLine(versionState, receiverBuild)}
      </span>
      <Button
        variant="secondary"
        size="sm"
        className={cn(
          "whitespace-nowrap rounded-full px-3 text-xs",
          pcSource === "auto" && "text-success",
        )}
        onClick={onPcPill}
      >
        PC:{" "}
        {pc
          ? pc + (pcSource === "auto" ? " (auto)" : " (manual)")
          : "?"}
      </Button>
      {space && space.free >= 0 && (
        <Badge
          variant="secondary"
          className="whitespace-nowrap rounded-full px-3 text-xs font-normal text-muted-foreground"
        >
          {"Free " + fmt(space.free) + " / " + fmt(space.total)}
        </Badge>
      )}
      <Button
        variant="secondary"
        size="sm"
        className="whitespace-nowrap rounded-full px-3 text-xs"
        onClick={onClearStaged}
        title="Delete leftover staged packages — a payload killed mid-install can leave gigabytes behind"
        aria-label="Clear leftover staged packages"
      >
        <Eraser className="h-4 w-4" aria-hidden="true" /> Clean
      </Button>
    </header>
  );
}

