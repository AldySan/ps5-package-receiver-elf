import { Download, Layers } from "lucide-react";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import type { CatalogEntry } from "@/lib/types";

interface GameDialogProps {
  entry: CatalogEntry | null;
  family: CatalogEntry[];
  onInstall: (g: CatalogEntry) => void;
  onInstallAll: (base: CatalogEntry) => void;
  onOpenChange: (open: boolean) => void;
}

function roleBadge(role: string | undefined) {
  if (role === "Patch")
    return <Badge className="border-transparent text-white" style={{ backgroundColor: "#E17B7B" }}>Patch</Badge>;
  if (role === "DLC")
    return <Badge className="border-transparent text-white" style={{ backgroundColor: "#E8A34C" }}>DLC</Badge>;
  return <Badge>{role}</Badge>;
}

function memberMeta(g: CatalogEntry) {
  return [g.titleId, g.version ? "v" + g.version : "", g.sizeText]
    .filter(Boolean)
    .join(" \u2022 ");
}

/* details for one game: install it alone, or install it together
 * with every patch/DLC in its family */
export function GameDialog({
  entry,
  family,
  onInstall,
  onInstallAll,
  onOpenChange,
}: GameDialogProps) {
  const patches = family.filter((m) => m.role === "Patch");
  const dlcs = family.filter((m) => m.role !== "Patch");

  const group = (title: string, list: CatalogEntry[]) =>
    list.length > 0 && (
      <div key={title} className="mt-1">
        <p className="mb-1 px-2 text-left text-xs font-bold uppercase tracking-wide text-muted-foreground">
          {title} ({list.length})
        </p>
        <div className="flex flex-col gap-2">
          {list.map((m) => (
            <div
              key={m.id + m.role}
              className="flex items-center gap-3 rounded-lg bg-background p-3 text-left"
            >
              <div className="min-w-0 flex-1">
                <p className="truncate text-base font-semibold">{m.title}</p>
                <p className="text-sm text-muted-foreground">
                  {memberMeta(m)}
                </p>
              </div>
              <div className="flex shrink-0 items-center gap-2">
                {roleBadge(m.role)}
                <Button onClick={() => onInstall(m)}>
                  <Download className="h-4 w-4" /> Install
                </Button>
              </div>
            </div>
          ))}
        </div>
      </div>
    );

  return (
    <Dialog open={!!entry} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-xl">
        {entry && (
          <>
            <DialogHeader>
              <DialogTitle>{entry.title}</DialogTitle>
              <DialogDescription className="flex flex-wrap items-center gap-2 text-sm">
                <span>{memberMeta(entry)}</span>
                {entry.platform === "PS5" && (
                  <Badge className="bg-foreground text-background">PS5</Badge>
                )}
                {entry.platform === "PS4" && (
                  <Badge
                    className="border-transparent text-white"
                    style={{ backgroundColor: "#0070D1" }}
                  >
                    PS4
                  </Badge>
                )}
              </DialogDescription>
            </DialogHeader>

            <Button className="w-full" size="lg" onClick={() => onInstall(entry)}>
              <Download className="h-4 w-4" /> Install game
            </Button>

            {family.length > 0 && (
              <Button
                variant="success"
                className="w-full"
                onClick={() => onInstallAll(entry)}
              >
                <Layers className="h-4 w-4" /> Install all (
                {family.length + 1} packages)
              </Button>
            )}

            {group("Patches", patches)}
            {group("DLC", dlcs)}
          </>
        )}
      </DialogContent>
    </Dialog>
  );
}
