/**
 * Development-only React diagnostics loader.
 *
 * `react-grab` and `react-scan` are devDependencies loaded through dynamic
 * imports behind `import.meta.env.DEV`, so the production build statically
 * eliminates them. Set `VITE_DISABLE_REACT_DEVTOOLS=1` to suppress loading in
 * development; any other value (unset, empty, `0`) keeps diagnostics enabled.
 */
export function shouldEnableReactDiagnostics(
  isDev: boolean,
  disableFlag: string | undefined
): boolean {
  return isDev && disableFlag !== "1";
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
