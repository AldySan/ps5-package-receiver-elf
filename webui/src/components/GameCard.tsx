import * as React from "react";
import { motion, useReducedMotion } from "motion/react";
import { Package } from "lucide-react";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { cn } from "@/lib/utils";
import type { CatalogEntry } from "@/lib/types";

interface GameCardProps {
  g: CatalogEntry;
  pc: string;
  addons: number;
  onInstall: (g: CatalogEntry) => void;
  onOpen: (g: CatalogEntry) => void;
}

/* Library tile: cover (or placeholder), platform badge, add-on chip,
 * title, meta and an Install button. Clicking the card opens the
 * details dialog. PS4 covers are square, PS5 covers are rounded. */
export function GameCard({ g, pc, addons, onInstall, onOpen }: GameCardProps) {
  const [imgErr, setImgErr] = React.useState(false);
  const reduced = useReducedMotion();
  const ps4 = g.platform === "PS4";

  const open = () => onOpen(g);

  return (
    <motion.article
      initial={reduced ? undefined : { opacity: 0, y: 8 }}
      animate={{ opacity: 1, y: 0 }}
      transition={{ duration: 0.25 }}
      whileHover={reduced ? undefined : { y: -2 }}
      className="group relative flex cursor-pointer flex-col overflow-hidden rounded-xl bg-card p-3 text-center shadow-md transition-shadow hover:shadow-xl focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring"
      role="button"
      tabIndex={0}
      onClick={open}
      onKeyDown={(e) => {
        if (e.key === "Enter" || e.key === " ") {
          e.preventDefault();
          open();
        }
      }}
    >
      {g.hasIcon && !imgErr ? (
        <img
          src={"http://" + pc + ":9898/icon/" + encodeURIComponent(g.id)}
          alt={g.title}
          loading="lazy"
          onError={() => setImgErr(true)}
          className={cn(
            "mb-2 aspect-square w-full bg-card object-cover",
            ps4 ? "rounded-none" : "rounded-xl",
          )}
        />
      ) : (
        <div
          className={cn(
            "mb-2 flex aspect-square w-full items-center justify-center bg-card",
            ps4 ? "rounded-none" : "rounded-xl",
          )}
        >
          <Package className="h-14 w-14 text-foreground opacity-30" />
        </div>
      )}

      <div className="absolute left-5 top-5">
        {g.platform === "PS5" && (
          <Badge className="bg-foreground text-background">PS5</Badge>
        )}
        {ps4 && (
          <Badge className="border-transparent text-white" style={{ backgroundColor: "#0070D1" }}>
            PS4
          </Badge>
        )}
      </div>

      <h3 className="mb-1 min-h-11 break-words text-base font-semibold leading-snug">
        {g.title}
      </h3>
      <p className="mb-2 min-h-5 text-sm text-muted-foreground">
        {[g.titleId, g.version ? "v" + g.version : "", g.sizeText]
          .filter(Boolean)
          .join(" \u2022 ")}
      </p>
      {addons > 0 && (
        <span className="mx-auto mb-2 inline-block rounded-full border border-primary px-3 text-sm font-semibold text-primary">
          {addons} add-on{addons > 1 ? "s" : ""}
        </span>
      )}
      <Button
        className="mt-auto w-full"
        onClick={(e) => {
          e.stopPropagation();
          onInstall(g);
        }}
      >
        Install
      </Button>
    </motion.article>
  );
}
