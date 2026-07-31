import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import http from "node:http";
import path from "path";

// The Radxa Linkr Debugger firmware serves its HTTP/WebSocket control API on the
// USB-NCM control service at http://172.29.203.1:8080. Port 80 is the
// compatibility relay used by the board-hosted page and has a much smaller
// connection pool, so the development proxy must use the native service.
// WebSocket upgrades stay same-origin in the browser to avoid CORS/PNA issues.
export default defineConfig(({ mode }) => {
  const isFirmwareBuild = mode === "firmware" || Boolean(process.env.VITE_OUT_DIR);
  const debuggerTarget = process.env.VITE_DEBUGGER_TARGET || "http://172.29.203.1:8080";

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
          target: debuggerTarget,
          changeOrigin: true,
          ws: true,
          // WebSocket upgrades keep an upstream socket occupied for their
          // entire lifetime. Allow all four firmware live sessions while
          // retaining four independent slots for status/control requests.
          // Keeping this aligned with CONFIG_HTTP_SERVER_MAX_CLIENTS avoids
          // the proxy surfacing an empty 502 while the board is still healthy.
          agent: new http.Agent({ keepAlive: true, maxSockets: 8, maxFreeSockets: 2 }),
        },
      },
    },
  };
});
