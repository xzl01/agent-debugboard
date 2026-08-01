import React from "react";
import ReactDOM from "react-dom/client";
import App from "./App";
import { ThemeProvider } from "./lib/theme";
import { LanguageProvider } from "./lib/i18n";
import { loadReactDiagnostics } from "./lib/devtools";
import "./index.css";

async function bootstrap(): Promise<void> {
  await loadReactDiagnostics();
  ReactDOM.createRoot(document.getElementById("root")!).render(
    <React.StrictMode>
      <ThemeProvider>
        <LanguageProvider>
          <App />
        </LanguageProvider>
      </ThemeProvider>
    </React.StrictMode>
  );
}

void bootstrap();
