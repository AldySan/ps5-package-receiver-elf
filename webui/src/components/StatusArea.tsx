import { Pause, Play, XCircle } from "lucide-react";
import { Progress } from "@/components/ui/progress";
import { Button } from "@/components/ui/button";
import { cn } from "@/lib/utils";
import type { Msg } from "@/lib/types";
import type { CopyBar } from "@/lib/receiver";

interface StatusAreaProps {
  msg: Msg;
  bar: CopyBar | null;
  copyActive: boolean;
  onPause: () => void;
  onCancel: () => void;
}

/* One line for progress text, one bar for queue/copy progress, and
 * pause/stop while a PC -> console copy runs (ours or not). */
export function StatusArea({ msg, bar, copyActive, onPause, onCancel }: StatusAreaProps) {
  const pct =
    bar && bar.total > 0 ? Math.min(100, (bar.done * 100) / bar.total) : null;

  return (
    <div className="mt-2">
      <p
        className={cn(
          "min-h-8 text-base",
          msg.kind === "ok" && "text-success",
          msg.kind === "err" && "text-destructive",
          msg.kind === "info" && "text-muted-foreground",
        )}
        role="status"
        aria-live="polite"
      >
        {msg.text}
      </p>

      {pct !== null && <Progress value={pct} className="mb-3" />}

      {copyActive && (
        <div className="mb-3 flex gap-2">
          <Button
            variant="secondary"
            className="flex-1"
            onClick={onPause}
          >
            {bar?.paused ? (
              <>
                <Play className="h-4 w-4" /> Resume
              </>
            ) : (
              <>
                <Pause className="h-4 w-4" /> Pause
              </>
            )}
          </Button>
          <Button variant="destructive" className="flex-1" onClick={onCancel}>
            <XCircle className="h-4 w-4" /> Cancel copy
          </Button>
        </div>
      )}
    </div>
  );
}
