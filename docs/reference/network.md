# Network Addressing

Each Radxa Linkr Debugger exposes one USB-NCM Ethernet interface. The firmware
derives the interface IPv4 address and the DHCP server pool from one persisted
subnet octet so that several debugger boards can be attached to the same host
without sharing an address.

## Address Plan

| Item | Value |
| --- | --- |
| Device address | `172.29.<subnet>.1` |
| DHCP server base address | `172.29.<subnet>.10` |
| DHCP pool size | 1 address (`CONFIG_NET_DHCPV4_SERVER_ADDR_COUNT=1`) |
| Valid subnet octet | `1` through `254` |
| Default subnet octet | `203` |
| Persistent settings key | `linkr/net/subnet` |

The octet is stored as one byte under `linkr/net/subnet`. The default `203`
produces the established `172.29.203.1` device address. A missing, malformed, or
out-of-range stored value falls back to `203`.

Changing the subnet persists the new value and applies it immediately: the
firmware stops the running DHCPv4 server, removes the old manual IPv4 address,
adds `172.29.<subnet>.1`, and restarts the DHCPv4 server with
`172.29.<subnet>.10` as its base. A failure is retried with the existing
network-start backoff. This is a single-host USB-NCM link, so firmware disables
DHCP ICMP address probing (`CONFIG_NET_DHCPV4_SERVER_ICMP_PROBE_TIMEOUT=0`);
there is no competing L2 client, and disabling probes avoids the upstream
DHCP/ICMP lock interaction during an explicit reconfiguration. The DHCP server
restart does **not** force the host DHCP
client to discard its old lease. A host connected through the old address must
disconnect and reconnect the USB-NCM interface, or explicitly release and renew
its DHCP lease, before using the new address.

Firmware reserves two unicast IPv4 slots on the NCM interface. During a live
change the first slot may remain referenced by the HTTP connection that issued
the request while the second slot installs the new manual address; the old slot
is released when that connection closes.

For example, with NetworkManager or `dhclient` on Linux (replace `<ncm-iface>`
with the interface name):

```sh
nmcli device disconnect <ncm-iface>
nmcli device connect <ncm-iface>

# Or, when dhclient manages the interface:
sudo dhclient -r <ncm-iface>
sudo dhclient <ncm-iface>
```

Because the old address is removed before the HTTP reply is sent, a PUT or CLI
set over the old NCM address can lose its response even though the value was
persisted. Treat a transport error after such a set as "possibly applied" and
verify at the new address. The CDC ACM shell is the reliable way to change the
subnet because that link is independent of the NCM address.

### Multiple Boards On One Host

The default `203` collides with an existing board on the same host. Set the new
board's subnet through the CDC ACM shell before the host configures its NCM
interface, or keep the host DHCP client off the new interface until then:

```text
ip subnet 204
```

The NCM local and remote MAC addresses are currently identical on every board.
If the host's network manager refuses two interfaces with the same MAC, bring
the second NCM interface up manually or keep only one NCM interface active at a
time.

## HTTP

The firmware exposes the setting at `/api/v1/net`.

```sh
curl -fsS http://172.29.203.1/api/v1/net
curl -fsS -X PUT -H 'Content-Type: application/json' \
  -d '{"subnet":204}' http://172.29.203.1/api/v1/net
```

`GET` response:

```json
{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"net","action":"get","subnet":203,"ip":"172.29.203.1","reconnect_required":false}
```

`PUT` accepts only a JSON object with an integer `subnet` in `1..254`. It
returns `200` with the new `subnet` and `ip`, `400` for a missing, malformed, or
out-of-range body, and `500` when the live apply fails after the value was
persisted. A successful change explicitly reports that the host link must be
renewed; it does not claim that the host moved to the new subnet seamlessly:

```json
{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"net","action":"set","subnet":204,"ip":"172.29.204.1","reconnect_required":true,"message":"Reconnect the USB NCM interface or renew its DHCP lease"}
```

## CDC ACM Shell

The shell command group is `ip`; `net` is registered as an alias with the same
behavior.

```text
ip subnet
ip subnet 204
```

`ip subnet` prints the current octet, the device address, and the board URL:

```text
subnet=203 ip=172.29.203.1 url=http://172.29.203.1/
```

`ip subnet <1-254>` validates the octet, persists it through the same firmware
path as the HTTP endpoint, applies it live, and prints the resulting state. The
alias `net subnet ...` is equivalent.

## Rust CLI

The host CLI calls the firmware HTTP endpoint.

```sh
radxa-linkr-debuggerctl ip subnet
radxa-linkr-debuggerctl ip subnet 204
radxa-linkr-debuggerctl --json ip subnet
```

The `net subnet ...` spelling is accepted as an alias. Plain output is:

```text
subnet=203 ip=172.29.203.1 url=http://172.29.203.1/
```

`--json` prints the firmware envelope unchanged. Invalid octets are rejected
locally before any request is sent. After changing the subnet, point the CLI at
the new address for later commands. First disconnect/reconnect the USB-NCM
interface or renew its DHCP lease as described above:

```sh
radxa-linkr-debuggerctl --url http://172.29.204.1 ip subnet
```

## Captive-Portal URI Limitation

`CONFIG_NET_DHCPV4_SERVER_OPTION_CAPTIVE_PORTAL_URI` is compiled into Zephyr at
build time. This repository does not patch Zephyr, so the DHCP captive-portal
URI remains the default `http://172.29.203.1/captive-portal/api` even when the
subnet is changed. The captive-portal redirect/body and the boot-time default
network configuration also remain compiled around `172.29.203.1`.

For any subnet other than `203`, open the board manually at
`http://172.29.<subnet>.1/` instead of relying on the advertised captive-portal
URI. The REST API and the Web UI work at that address.
