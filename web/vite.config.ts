import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import path from "path";

// The Radxa Linkr Debugger firmware serves its HTTP/WebSocket control API on the
// USB-NCM link at http://172.29.203.1:8080. The dev server proxies /api to that
// address (including WebSocket upgrades) so the browser can talk to the board
// same-origin and avoid CORS. Point this at a different host when needed.
export default defineConfig({
  base: process.env.VITE_BASE_PATH || "/",
  plugins: [react()],
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
      },
    },
  },
});
