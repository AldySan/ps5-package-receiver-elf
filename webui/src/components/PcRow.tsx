import * as React from "react";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";

interface PcRowProps {
  open: boolean;
  value: string;
  onSave: (value: string) => void;
  onRefresh: () => void;
}

/* shown when no PC announced itself, or after clicking the PC pill */
export function PcRow({ open, value, onSave, onRefresh }: PcRowProps) {
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
    </div>
  );
}
