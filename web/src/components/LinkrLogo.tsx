export function LinkrLogo({
  state,
  rails,
  logicAnalyzerActive,
  uartBridgeActive,
}: {
  readonly state: "connecting" | "ready" | "offline";
  readonly rails: readonly [boolean, boolean, boolean];
  readonly logicAnalyzerActive: boolean;
  readonly uartBridgeActive: boolean;
}) {
  return (
    <svg
      width="158"
      height="36"
      viewBox="0 0 316 72"
      fill="none"
      xmlns="http://www.w3.org/2000/svg"
      aria-hidden="true"
      focusable="false"
      data-state={state}
      data-logic-analyzer-active={logicAnalyzerActive}
      data-testid="linkr-logo"
      className="h-8 w-auto shrink-0 text-[rgb(var(--c-identity))]"
    >
      <rect data-linkr-backplate x="0" y="4" width="64" height="64" rx="14" />
      <svg data-radxa-x x="0" y="4" width="64" height="64" viewBox="292 94 122 113">
        <path data-linkr-heartbeat d="M353.111 134.403L369.308 150.6L353.112 166.799L336.914 150.601Z" fill="currentColor" />
        <g data-linkr-rotor>
          <path data-linkr-arm data-indicator="5v_out" data-active={rails[0]} d="M328.786 103.66C332.706 108.611 349.543 129.891 353.113 134.401L336.915 150.602C327.237 142.942 307.506 127.329 297.728 119.589C297.613 119.5 297.613 119.351 297.717 119.248L315 101.965C318.699 98.2658 324.798 98.6209 328.045 102.724L328.786 103.66Z" fill="currentColor" />
          <path data-linkr-arm data-indicator="12v_out" data-active={rails[1]} d="M400.051 126.275C395.1 130.195 373.82 147.031 369.31 150.602L353.109 134.404C359.082 126.859 369.886 113.204 378.026 102.919C381.468 98.5665 387.486 98.2286 391.412 102.151L408.692 119.434L400.051 126.275Z" fill="currentColor" />
          <path data-linkr-arm data-indicator="uart_bridge" data-active={uartBridgeActive} d="M377.436 197.54C373.516 192.59 356.677 171.309 353.109 166.8L369.307 150.599C378.985 158.258 398.716 173.872 408.495 181.612C408.609 181.7 408.609 181.849 408.506 181.952L391.223 199.235C387.524 202.935 381.425 202.58 378.178 198.477L377.436 197.54Z" fill="currentColor" />
          <path data-linkr-arm data-indicator="20v_out" data-active={rails[2]} d="M306.173 174.929C311.123 171.009 332.404 154.173 336.913 150.602L353.114 166.8C347.098 174.399 336.18 188.198 328.02 198.511C324.781 202.603 318.705 202.947 315.018 199.256L297.531 181.77L306.173 174.929Z" fill="currentColor" />
        </g>
      </svg>
      <g transform="translate(72)" stroke="currentColor" strokeWidth="11" strokeLinecap="round" strokeLinejoin="round">
        <path d="M12 10V48C12 55 16 58 23 58" />
        <path d="M44 30V58" />
        <path d="M74 58V30M74 43C74 35 81 29 90 29C101 29 108 36 108 47V58" />
        <path d="M140 10V58M172 29L140 53M153 44L174 58" />
        <path d="M204 58V30M204 43C204 35 211 29 220 29H234" />
      </g>
      <circle cx="116" cy="13" r="5.5" fill="currentColor" />
    </svg>
  );
}
