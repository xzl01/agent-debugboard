# Network Identity

Each Radxa Linkr Debugger exposes one USB-NCM Ethernet interface. The board
persists a single identity: a locally administered unicast MAC address and one
private IPv4 `/30` network. Both are derived once from the RP2350 chip ID, so
several debugger boards can share one host without reusing the same address.

## Address Plan

| Item | Value |
| --- | --- |
| Board address | `network + 1` |
| Host DHCP address | `network + 2` |
| DHCP pool size | 1 address (`CONFIG_NET_DHCPV4_SERVER_ADDR_COUNT=1`) |
| Prefix | `/30` |
| First-boot candidate pool | `10.0.0.0/8` split into 2^22 `/30` networks |
| MAC | 46 derived bits, locally administered, unicast (`02:...`) |
| Persistent settings key | `linkr/net/identity` |

The persisted blob is 12 bytes: version, origin (`derived` or `manual`), the
six MAC bytes, and the four network bytes in big-endian order. It is written as
one Settings value so a failed or interrupted save cannot leave a mixed
MAC/network pair.

## First Boot And Persistence

`hwinfo_get_device_id()` returns the RP2350 OTP chip ID when
`CONFIG_HWINFO_RPI_PICO_CHIP_ID=y`. The firmware rejects missing, all-zero, and
all-one IDs. It derives:

- a MAC with the I/G bit clear and the U/L bit set, so the address is unicast
  and locally administered;
- a `/30` candidate in `10.0.0.0/8` from a 22-bit hash index.

Both are persisted immediately on first boot. Later boots load the stored blob
and never rederive over it. A manually set MAC or network is authoritative,
persists in the same blob, and reports `origin=manual`. If a stored blob cannot
be read or decoded, firmware uses a volatile derived identity for that boot but
does not overwrite the stored value; the next boot retries the read.

Derivation is deterministic and public, not a security identity. A fleet-wide
zero-collision guarantee ultimately needs persisted allocation or manual
conflict resolution; the hash makes accidental reuse unlikely, not impossible.

## Applying An Identity

The firmware applies the MAC while the NCM interface is stopped. The boot path
sets the MAC before bringing the interface up
(`CONFIG_ETH_NET_IF_NO_AUTO_START=y`). A live MAC change stops the DHCP server,
takes the interface down, updates the link address, then brings it back up.

A network change also stops DHCP, removes the old manual IPv4 address, adds
`network + 1`, and restarts DHCP with `network + 2` as its base. The old address
may remain referenced by the HTTP connection that issued the request until that
connection closes; firmware reserves two unicast IPv4 slots for this case.

The change does **not** force the host DHCP client to discard its old lease.
Reconnect the USB-NCM interface or renew its lease before using the new
address. Because the old address can disappear before the HTTP reply is sent, a
transport error after a `PUT` means "possibly applied"; verify at the new
address. The CDC ACM shell is the reliable path when the NCM address changes.

## Multiple Boards On One Host

No manual address step is required on first boot: each board derives its own
MAC and `/30`. To override an identity, use the shell before the host configures
the NCM interface:

```text
ip
ip network 10.20.30.0/30
ip mac 02:11:22:33:44:55
```

`ip`/`net` is the command alias pair. `ip` prints the current identity:

```text
mac=02:11:22:33:44:55 origin=derived network=10.20.30.0/30 ip=10.20.30.1 dhcp_base=10.20.30.2 url=http://10.20.30.1/
```

The USB NCM host-side `iMACAddress` string remains the value compiled into the
Zephyr class descriptor (`remote-mac-address`); the current Zephyr API does not
allow replacing that string descriptor at runtime. The board's own Ethernet
address is unique, but if a host network manager refuses two NCM interfaces
with the same host-side iMACAddress, keep only one interface active or apply a
host-side MAC override. This is a Zephyr USB-class limitation, not a persisted
board identity setting.

## HTTP

The firmware exposes the identity at `/api/v1/net`. Use the address printed by
`ip` as `$BOARD_URL`.

```sh
curl -fsS "$BOARD_URL/api/v1/net"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  -d '{"network":"10.20.30.0"}' "$BOARD_URL/api/v1/net"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  -d '{"mac":"02:11:22:33:44:55"}' "$BOARD_URL/api/v1/net"
```

`GET` response:

```json
{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"net","action":"get","mac":"02:11:22:33:44:55","origin":"derived","network":"10.20.30.0","prefix":30,"ip":"10.20.30.1","dhcp_base":"10.20.30.2","url":"http://10.20.30.1/","reconnect_required":false}
```

`PUT` accepts a JSON object containing `mac`, `network`, or both. Omitted fields
keep their current value. `mac` must be a locally administered unicast address;
`network` must be a private `/30` base (an optional `/30` suffix is accepted).
The endpoint returns `200` with the resulting identity, `400` for a missing,
malformed, or out-of-range body, and `500` when persistence or the live apply
fails:

```json
{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"net","action":"set","mac":"02:11:22:33:44:55","origin":"manual","network":"10.20.30.0","prefix":30,"ip":"10.20.30.1","dhcp_base":"10.20.30.2","url":"http://10.20.30.1/","reconnect_required":true,"message":"Reconnect the USB NCM interface or renew its DHCP lease"}
```

## Rust CLI

The host CLI calls the same endpoint.

The host NCM address is `network + 2`; pass the board URL (`network + 1`) with
`--url` or set `LINKR_BOARD_URL` to it. The CDC shell `ip` command prints it when
the host address is unknown.

```sh
radxa-linkr-debuggerctl --url http://10.20.30.1 ip
radxa-linkr-debuggerctl --url http://10.20.30.1 ip network 10.20.30.0/30
radxa-linkr-debuggerctl --url http://10.20.30.1 ip mac 02:11:22:33:44:55
radxa-linkr-debuggerctl --url http://10.20.30.1 --json ip
```

`net` is accepted as an alias for `ip`. Plain output is:

```text
mac=02:11:22:33:44:55 origin=derived network=10.20.30.0/30 ip=10.20.30.1 dhcp_base=10.20.30.2 url=http://10.20.30.1/
```

`--json` prints the firmware envelope unchanged. Invalid MACs and non-private
or unaligned `/30` values are rejected locally before any request is sent. After
changing the network, point later commands at the new address:

```sh
radxa-linkr-debuggerctl --url http://10.20.30.1 ip
```

## DHCP Captive Portal

The DHCPv4 server advertises option 114. The override URI is empty, so Zephyr
builds the URI from the current DHCP server address and the `/generate_204`
path at runtime. Firmware serves that path with the same capport JSON as
`/captive-portal/api`, and redirects other non-API paths to the board URL. This
keeps captive-portal discovery correct on the derived `/30` without a
compile-time address.
