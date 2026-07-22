import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import http from "node:http";
import path from "path";

// The Radxa Linkr Debugger firmware serves its HTTP/WebSocket control API on the
// USB-NCM link at http://172.29.203.1:8080. The dev server proxies /api to that
// address (including WebSocket upgrades) so the browser can talk to the device
// same-origin and avoid CORS. Point this at a different host when needed.
export default defineConfig(({ mode }) => {
  const isFirmwareBuild = mode === "firmware" || Boolean(process.env.VITE_OUT_DIR);

  return {
    base: process.env.VITE_BASE_PATH || "/",
    plugins: [react()],
    build: {
      outDir: process.env.VITE_OUT_DIR || "dist",
      emptyOutDir: true,
      rolldownOptions: {
        output: {
          codeSplitting: isFirmwareBuild ? false : undefined,
          entryFileNames: "assets/app.js",
          chunkFileNames: "assets/[name].js",
          assetFileNames: "assets/app[extname]",
        },
      },
    },
    resolve: {
      alias: { "@": path.resolve(__dirname, "src") },
    },
    server: {
      host: true,
      port: 5173,
      proxy: {
        "/api": {
          target: "http://172.29.203.1:8080",
          changeOrigin: true,
          ws: true,
          // The firmware exposes only a small client pool. Reuse one upstream
          // connection instead of consuming an additional slot per poll.
          agent: new http.Agent({ keepAlive: true, maxSockets: 1, maxFreeSockets: 1 }),
        },
      },
    },
  };
});
