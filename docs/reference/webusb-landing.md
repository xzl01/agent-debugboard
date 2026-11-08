# WebUSB landing-page handoff

This is a navigation hint, not a WebUSB data transport. The existing NCM,
control CDC, J4 CDC, HTTP and WebSocket protocols are unchanged. The firmware
does not add a vendor data interface, endpoint, WinUSB binding or MS OS descriptor.

## USB contract

When the existing network identity can be resolved during USB initialization:

- Full-speed `bcdUSB` is `0x0210`. Zephyr accepts BOS from `0x0201`, but
  Chromium only automatically probes WebUSB descriptors from USB 2.1.
- BOS contains a USB 2.0 Extension with zero attributes (no LPM claim), plus a
  24-byte WebUSB Platform Capability with UUID
  `3408b638-09a9-47a0-8bfd-a0768815b665`.
- WebUSB descriptor version is `0x0100`, vendor request code is `0x01`, landing
  URL index is `1`.
- GET_URL is device-recipient vendor IN: `bmRequestType=0xC0`, `bRequest=1`,
  `wIndex=2`, `wValue=1`. Unsupported requests/indices are rejected; the response
  is truncated to `wLength`.
- The URL descriptor contains `{bLength, 0x03, 0x01, UTF-8 suffix}`. Scheme `1`
  supplies `https://`; the suffix is
  `xzl01.github.io/agent-debugboard/connect.html#device=<board IPv4>`.

The board IPv4 is formatted from `network_identity` using the existing device
host offset. It is not independently derived or persisted. The descriptor is a
startup snapshot generated before `usbd_enable`; its control callback only copies
prepared bytes and performs no settings access or network operation. If identity
or URL preparation fails, WebUSB is omitted while original USB initialization
continues. Restart after changing the network identity to regenerate the URL;
previous browser notifications may retain an older snapshot.

The fixed Pages prefix is in `linkr_debugger_webusb_url.h`. Forks publishing at
another origin must update that prefix and the encoder's golden vectors. No new
runtime configuration or legacy URL format is provided.

## Pages contract

`web/public/connect.html` and `connect.js` are copied by the Pages build, not by
the firmware build (`publicDir=false`). This separate entry does not mount the
dashboard or use its loopback gateway configuration.

Only `#device=<IPv4>` is accepted. The value must be canonical dotted-decimal
RFC1918 IPv4; leading zeros, encoding, additional/duplicate keys, hostnames,
ports, paths, public/loopback/link-local addresses and arbitrary URLs are rejected.
The destination is constructed as `http://<IPv4>/`. There is no version matrix,
serial number, MAC, token or saved browser target.

The page shows the address and exposes a same-tab navigation link only after
validation. It does not navigate automatically, scan, open USB, fetch board APIs,
create WebSockets or write browser storage. Invalid input clears any prior link.
The private-address restriction is not device authentication: the user must
recognize the destination before opening it.

Fragments are not part of HTTP request URLs or Referer headers; the guide uses
`no-referrer` and contains no analytics. Page JavaScript can still read a fragment,
so it must not be treated as secret storage. Top-level navigation to the local
HTTP dashboard is distinct from cross-origin HTTP/WS requests within an HTTPS app.

## Delivery and platform boundaries

Firmware and the matching Pages artifact must both be published. Flashing does
not deploy Pages, and this feature does not provide offline caching of the guide.
Direct access to the board-hosted page and existing Tray/CLI remain available.

WebUSB notifications are optional browser behavior, not an auto-open guarantee.
Linux USB access permissions and Windows WinUSB requirements can affect whether
the browser reads the URL. Adding this BOS capability alone does not establish
cross-platform notification support.
