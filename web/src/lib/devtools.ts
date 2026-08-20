/**
 * Development-only React diagnostics loader.
 *
 * `react-grab` and `react-scan` are devDependencies loaded through dynamic
 * imports behind `import.meta.env.DEV`, so the production build statically
 * eliminates them. The normal product preview stays clean; set
 * `VITE_DISABLE_REACT_DEVTOOLS=0` explicitly when React diagnostics are needed.
 */
export function shouldEnableReactDiagnostics(
  isDev: boolean,
  disableFlag: string | undefined
): boolean {
  return isDev && disableFlag === "0";
}

export async function loadReactDiagnostics(): Promise<void> {
  // The import.meta.env.DEV operand must sit directly in this condition so the
  // production build statically folds it to false and eliminates the dynamic
  // imports below. Moving the check into a helper call keeps the packages in
  // the bundle.
  if (
    import.meta.env.DEV &&
    shouldEnableReactDiagnostics(
      import.meta.env.DEV,
      import.meta.env.VITE_DISABLE_REACT_DEVTOOLS
    )
  ) {
    try {
      const [, { scan }] = await Promise.all([
        import("react-grab"),
        import("react-scan"),
      ]);
      scan({ enabled: true });
    } catch (error) {
      console.warn(
        "[devtools] React diagnostics failed to load; continuing without them",
        error
      );
    }
  }
}
