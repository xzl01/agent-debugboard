function statusJsonCapacity(source) {
  const match = source.match(/^#define\s+LINKR_DEBUGGER_HTTP_STATUS_JSON_BUFSZ\s+(\d+)U$/m);
  return match ? Number.parseInt(match[1], 10) : 0;
}

function statusJsonSpecContract(spec, capacity) {
  return spec.includes(`验证 HTTP 响应 JSON 长度低于专用 ${capacity} 字节 status buffer（协议限制检查）：`)
    && spec.includes(`[ "$LEN" -lt ${capacity} ] || { echo "status response must be below ${capacity} bytes"; exit 1; }`);
}

function wsStatusSnapshotContract(spec, source) {
  return spec.includes("initial snapshot") && spec.includes("event-driven subsequent snapshots")
    && spec.includes("const MAX_SNAPSHOTS = 1;") && /if\s*\(events & LINKR_DEBUGGER_WS_EVENT_STATE\)\s*\{/.test(source);
}

function captivePortalDhcpContract(spec, prjConfig) {
  const firmwarePolicy = /^CONFIG_NET_DHCPV4_SERVER_OPTION_ROUTER=n$/m.test(prjConfig)
    && /^CONFIG_NET_DHCPV4_SERVER_OPTION_DNS_ADDRESS=""$/m.test(prjConfig)
    && /^CONFIG_NET_DHCPV4_SERVER_OPTION_CAPTIVE_PORTAL=y$/m.test(prjConfig)
    && /^CONFIG_NET_DHCPV4_SERVER_OPTION_CAPTIVE_PORTAL_URI="http:\/\/172\.29\.203\.1\/captive-portal\/api"$/m.test(prjConfig);
  const dhcpSpec = spec.includes("DHCP DORA 的客户端 PRL 必须请求 DHCP option 114")
    && /DHCPACK 必须包含 DHCP option\s+114，URI 必须为/.test(spec)
    && /DHCPACK 不得通告 DHCP\s+option 3（router）或 DHCP\s+option 6（DNS）/.test(spec);
  const noBoardDns = !/\bdig\s+@172\.29\.203\.1(?:\s|$)/m.test(spec)
    && !/DHCPACK[^\n]*必须包含[^\n]*(?:DHCP option 3|DHCP option 6)/.test(spec)
    && !spec.includes("captured by the board DNS");
  const externalNetworkChecks = spec.includes("**多宿主路由回归检查**")
    && spec.includes("ip route get 8.8.8.8") && spec.includes("getent ahostsv4 example.com");
  return firmwarePolicy && dhcpSpec && noBoardDns && externalNetworkChecks;
}

function otaNegativeUploadTimeoutContract(script) {
  const uploadTimeouts = [
    'plan run_timeout "$UPLOAD_TIMEOUT" curl -sS -o /tmp/linkr-ota-bad-sha.json',
    'bad_sha_http=$(run_timeout "$UPLOAD_TIMEOUT" curl -sS -o "$bad_sha_body"',
    'plan run_timeout "$UPLOAD_TIMEOUT" curl -sS -o /tmp/linkr-ota-bad-type.json',
    'bad_type_http=$(run_timeout "$UPLOAD_TIMEOUT" curl -sS -o "$bad_type_body"',
  ];
  const shortTimeouts = [
    'run_timeout "$SHORT_TIMEOUT" curl -fsS "$(api_url "$1")"',
    'maybe_run run_timeout "$SHORT_TIMEOUT" curl -fsS -X POST "$(api_url /ota/test)"',
    'maybe_run run_timeout "$SHORT_TIMEOUT" curl -fsS -X POST "$(api_url /ota/confirm)"',
    'status_body=$(run_timeout "$SHORT_TIMEOUT" curl -fsS "$(api_url /ota)")',
  ];
  return uploadTimeouts.every((fragment) => script.includes(fragment))
    && shortTimeouts.every((fragment) => script.includes(fragment));
}

export function checkHilContracts({ hilSpec, prjConfig, statusSource, wsSource, webOtaHil }) {
  const statusCapacity = statusJsonCapacity(statusSource);
  return {
    statusJson: statusCapacity === 6144 && statusJsonSpecContract(hilSpec, statusCapacity),
    wsStatusSnapshots: wsStatusSnapshotContract(hilSpec, wsSource),
    captivePortalLocalOnlyDhcp: captivePortalDhcpContract(hilSpec, prjConfig),
    otaNegativeUploadTimeouts: otaNegativeUploadTimeoutContract(webOtaHil),
  };
}
