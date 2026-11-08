const MAX_FRAGMENT_LENGTH = 64;

/**
 * Parse a WebUSB landing-page fragment into a private IPv4 device URL.
 *
 * The private-address check limits where the link can point; it does not
 * authenticate the device at that address.
 */
export function parseDeviceFragment(hash) {
  if (typeof hash !== "string" || hash.length > MAX_FRAGMENT_LENGTH) {
    return null;
  }

  const match = /^#device=((?:0|[1-9]\d{0,2})\.(?:0|[1-9]\d{0,2})\.(?:0|[1-9]\d{0,2})\.(?:0|[1-9]\d{0,2}))$/.exec(
    hash,
  );
  if (!match || match[0] !== hash) {
    return null;
  }

  const octets = match[1].split(".").map(Number);
  if (octets.some((octet) => octet > 255)) {
    return null;
  }

  const [first, second] = octets;
  const isPrivate =
    first === 10 ||
    (first === 172 && second >= 16 && second <= 31) ||
    (first === 192 && second === 168);
  if (!isPrivate) {
    return null;
  }

  const host = octets.join(".");
  return { host, url: `http://${host}/` };
}

export function renderDeviceConnection(doc, hash) {
  const status = doc.querySelector("#connection-status");
  const address = doc.querySelector("#device-address");
  const link = doc.querySelector("#device-link");
  if (!status || !address || !link) {
    return;
  }

  link.hidden = true;
  link.removeAttribute("href");
  address.textContent = "—";

  if (!hash) {
    doc.documentElement.dataset.state = "missing";
    status.textContent = "未找到设备地址。";
    return;
  }

  const device = parseDeviceFragment(hash);
  if (!device) {
    doc.documentElement.dataset.state = "invalid";
    status.textContent = "设备地址无效。";
    return;
  }

  doc.documentElement.dataset.state = "ready";
  status.textContent = "已读取设备地址，请确认后打开本地页面。";
  address.textContent = device.host;
  link.setAttribute("href", device.url);
  link.hidden = false;
}

export function initializeDeviceConnection(doc, location, eventTarget) {
  const update = () => renderDeviceConnection(doc, location.hash);
  update();
  eventTarget.addEventListener("hashchange", update);
  return update;
}

if (typeof document !== "undefined" && typeof window !== "undefined") {
  initializeDeviceConnection(document, window.location, window);
}
