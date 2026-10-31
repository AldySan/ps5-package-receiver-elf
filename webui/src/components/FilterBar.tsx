import { Search } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Tabs, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { cn } from "@/lib/utils";

export type Plat = "all" | "PS5" | "PS4";
export type Kind = "games" | "images";

interface FilterBarProps {
  q: string;
  onQ: (q: string) => void;
  plat: Plat;
  onPlat: (p: Plat) => void;
  kind: Kind;
  onKind: (k: Kind) => void;
}

const PLATS: Plat[] = ["all", "PS5", "PS4"];

export function FilterBar({ q, onQ, plat, onPlat, kind, onKind }: FilterBarProps) {
  /* PS4 entries are all games — the old page hid the kind switch */
  const showKind = plat !== "PS4";

  return (
    <div className="mb-4 flex flex-wrap items-center gap-2">
      <div className="relative flex-1 min-w-56">
        <Search className="absolute left-3 top-1/2 h-4 w-4 -translate-y-1/2 text-muted-foreground" />
        <Input
          className="pl-9"
          placeholder="Search title or ID..."
          value={q}
          onChange={(e) => onQ(e.target.value)}
        />
      </div>
      <div className="flex gap-2">
        {PLATS.map((p) => (
          <Button
            key={p}
            size="lg"
            variant={plat === p ? "default" : "secondary"}
            className={cn(plat !== p && "text-foreground")}
            onClick={() => onPlat(p)}
          >
            {p === "all" ? "All" : p}
          </Button>
        ))}
      </div>
      {showKind && (
        <Tabs value={kind} onValueChange={(v) => onKind(v as Kind)}>
          <TabsList className="h-12">
            <TabsTrigger value="games" className="px-5">
              Games
            </TabsTrigger>
            <TabsTrigger value="images" className="px-5">
              Images
            </TabsTrigger>
          </TabsList>
        </Tabs>
      )}
    </div>
  );
}
