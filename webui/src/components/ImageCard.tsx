import * as React from "react";
import { motion, useReducedMotion } from "motion/react";
import { Archive, HardDrive, Package } from "lucide-react";
import { Button } from "@/components/ui/button";
import { cn } from "@/lib/utils";
import type { CatalogEntry } from "@/lib/types";

interface ImageCardProps {
  g: CatalogEntry;
  pc: string;
  onCopy: (g: CatalogEntry) => void;
}

/* same colors and icons the old page used per image format */
function formatStyle(format: string | undefined) {
  if (format === "exfat") return { color: "#34B595", label: "EXFAT" };
  if (format === "ffpfsc") return { color: "#CE9C40", label: "FFPFSC" };
  if (format === "ffpkg") return { color: "#A27AD8", label: "FFPKG" };
  return { color: "#6498F0", label: (format || "img").toUpperCase() };
}

function FormatIcon({ format }: { format: string | undefined }) {
  const cls = "h-14 w-14 text-foreground opacity-30";
  if (format === "exfat") return <HardDrive className={cls} />;
  if (format === "ffpkg" || format === "ffpfsc")
    return <Archive className={cls} />;
  return <Package className={cls} />;
}

export function ImageCard({ g, pc, onCopy }: ImageCardProps) {
  const reduced = useReducedMotion();
  const [imgErr, setImgErr] = React.useState(false);
  const f = formatStyle(g.format);
  const ps4 = g.platform === "PS4";

  return (
    <motion.article
      initial={reduced ? undefined : { opacity: 0, y: 8 }}
      animate={{ opacity: 1, y: 0 }}
      transition={{ duration: 0.25 }}
      whileHover={reduced ? undefined : { y: -2 }}
      className="flex flex-col overflow-hidden rounded-xl bg-card p-3 text-center shadow-md transition-shadow hover:shadow-xl"
    >
      {g.hasIcon && !imgErr ? (
        <img
          src={"http://" + pc + ":9898/icon/" + encodeURIComponent(g.id)}
          alt=""
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
          <FormatIcon format={g.format} />
        </div>
      )}
      <h3 className="mb-1 break-words text-base font-semibold leading-snug">
        {g.title}
      </h3>
      <p className="mb-2 flex items-center justify-center gap-2 text-sm text-muted-foreground">
        <span
          className="rounded px-2 py-0.5 text-xs font-bold text-white"
          style={{ backgroundColor: f.color }}
        >
          {f.label}
        </span>
        {g.sizeText}
      </p>
      <Button className="mt-auto w-full" onClick={() => onCopy(g)}>
        Copy to homebrew
      </Button>
    </motion.article>
  );
}
