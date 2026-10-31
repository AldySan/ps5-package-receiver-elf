import * as React from "react";
import { cn } from "@/lib/utils";

/* No Radix dependency: a copy pull/queue progress needs nothing but a
 * width, and the PS5 browser does not need the extra bytes. */
interface ProgressProps extends React.HTMLAttributes<HTMLDivElement> {
  /* 0..100; the caller clamps */
  value?: number;
}

const Progress = React.forwardRef<HTMLDivElement, ProgressProps>(
  ({ className, value, ...props }, ref) => (
    <div
      ref={ref}
      role="progressbar"
      aria-valuemin={0}
      aria-valuemax={100}
      aria-valuenow={Math.round(value ?? 0)}
      className={cn(
        "relative h-2 w-full overflow-hidden rounded-full bg-secondary",
        className,
      )}
      {...props}
    >
      <div
        className="h-full bg-primary transition-[width] duration-500 ease-out"
        style={{ width: Math.min(100, Math.max(0, value ?? 0)) + "%" }}
      />
    </div>
  ),
);
Progress.displayName = "Progress";

export { Progress };
