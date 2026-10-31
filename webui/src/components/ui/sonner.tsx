import type * as React from "react";
import { Toaster as Sonner } from "sonner";

type ToasterProps = React.ComponentProps<typeof Sonner>;

/* dark-only dashboard */
const Toaster = ({ ...props }: ToasterProps) => (
  <Sonner theme="dark" className="toaster group" {...props} />
);

export { Toaster };
