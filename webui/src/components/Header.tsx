import type { SpaceReply } from "@/lib/types";
import { fmtSize as fmt } from "@/lib/format";
import { cn } from "@/lib/utils";
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
      return page + " • receiver " + (build ?? "?") + " • MISMATCH — resend the newest ELF";
  }
}

export function Header({
  versionState,
  receiverBuild,
  pc,
  pcSource,
  space,
  onPcPill,
}: HeaderProps) {
  return (
    <header className="flex flex-wrap items-center gap-3 pb-1">
      <img
        src="/logo.png"
        alt="PKG Sender logo"
        className="h-11 w-auto rounded-lg"
      />
      <h1 className="flex-1 text-2xl font-bold leading-tight">
        pkg remote installer
      </h1>
      <span className="text-xs text-muted-foreground">
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
      {space && (
        <Badge
          variant="secondary"
          className="whitespace-nowrap rounded-full px-3 text-xs font-normal text-muted-foreground"
        >
          {space.free >= 0
            ? "Free " + fmt(space.free) + " / " + fmt(space.total)
            : ""}
        </Badge>
      )}
    </header>
  );
}

