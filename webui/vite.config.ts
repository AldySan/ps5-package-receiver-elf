import path from "node:path";
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import { viteSingleFile } from "vite-plugin-singlefile";

// The whole UI ships inside the payload ELF as one HTML file, so every
// JS/CSS chunk gets inlined into index.html (vite-plugin-singlefile).
// The browsers that matter: the PS5 console browser (a WebKit build
// roughly at Safari 13 level) plus ordinary PC and phone browsers —
// hence the conservative ES2019 target.
export default defineConfig({
  plugins: [react(), viteSingleFile()],
  resolve: {
    alias: {
      "@": path.resolve(import.meta.dirname, "./src"),
    },
  },
  define: {
    // baked at build time by scripts/embed.mjs (payload/common.h tag)
    __PAGE_BUILD__: JSON.stringify(process.env.PAGE_BUILD ?? "dev"),
  },
  build: {
    target: "es2019",
    cssTarget: "safari13",
  },
});
