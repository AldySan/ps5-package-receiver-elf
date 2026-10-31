/* size / speed formatting, ported from the previous inline page */

export function fmtSize(n: number): string {
  if (n < 1024) return n + " B";
  if (n < 1048576) return (n / 1024).toFixed(1) + " KB";
  if (n < 1073741824) return (n / 1048576).toFixed(1) + " MB";
  return (n / 1073741824).toFixed(2) + " GB";
}

export function fmtSpd(bytes: number): string {
  if (bytes < 1024) return bytes.toFixed(0) + " B/s";
  if (bytes < 1048576) return (bytes / 1024).toFixed(0) + " KB/s";
  return (bytes / 1048576).toFixed(1) + " MB/s";
}
