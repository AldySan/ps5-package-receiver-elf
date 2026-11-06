import * as React from "react";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";

interface PcRowProps {
  open: boolean;
  value: string;
  onSave: (value: string) => void;
  onRefresh: () => void;
  onAuto: () => void;
}

/* shown when no PC announced itself, after clicking the PC pill, or
 * when a catalog load failed — the address is usually the fix */
export function PcRow({ open, value, onSave, onRefresh, onAuto }: PcRowProps) {
  const [text, setText] = React.useState(value);

  if (!open) return null;

  const save = () => onSave(text);

  return (
    <div className="mb-3 flex flex-wrap items-center gap-2">
      <Input
        className="flex-1 min-w-52"
        placeholder="PC address"
        value={text}
        onChange={(e) => setText(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === "Enter") save();
        }}
      />
      <Button onClick={save}>Save</Button>
      <Button variant="secondary" onClick={onRefresh}>
        Refresh
      </Button>
      {/* saved addresses stick until this is clicked — the way back to
       * automatic discovery */}
      <Button variant="secondary" onClick={onAuto}>
        Auto
      </Button>
    </div>
  );
}
