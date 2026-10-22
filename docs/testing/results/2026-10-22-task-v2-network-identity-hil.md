# Task-v2 and network identity HIL — 2026-10-22

## Status

**PASS for task-v2 atomic persistence, lossless source/IR, derived network identity,
HTTP/CDC fallback, and Web UI serving.** The OTG API cascade passed, but OTG
electrical behavior and effective/backfeed reporting remain pending; see
[Residual gaps](#residual-gaps).

## Artifact

| Item | Value |
| --- | --- |
| Board | Radxa Linkr Debugger, USB path `1-1.4.2`, host `eth1` |
| Firmware | combined `radxa-linkr-debugger-rp2350.uf2` |
| UF2 SHA-256 | `762f76ddb1e620e602fe8382ed072cee1b739a89aee5badab252e01c00217cfe` |
| Flash | 837948 / 847832 B used, 9884 B free |
| RAM | 499248 / 532480 B used, 33232 B free |
| Settings NVS | 128 KiB, `0x1b0000..0x1d0000`, 32 sectors (full erase used; pre-release 320 KiB layouts are not migrated) |
| Boot | normal application boot after ROM BOOTSEL flash |

The host NCM address was `10.123.5.150/30`; the board address was
`10.123.5.149/30`.

The full Task matrix below was run on an earlier build
(`6af7158650ca79dc4587ac2f9789c32640e7c5f397975c30177073069be064f4`). The
final artifact additionally contains the Web network-validation fixes, the
firmware unchanged-identity origin guard, the low-stack shell clear path, and
the task storage usage/gate. It was rechecked for network GET, unchanged PUT
preserving `origin:"derived"`, invalid-network rejection, Task A upsert with
exact source/full-IR digests, storage accounting, clear, CDC `task clear`, and
the `ip` subcommand guard.

## Preflight

After the flash, the first host hardware mutation was `5v_out=on`, followed by
fixed PD 20 V. INA226 reported `5.076250 V / 164.75 mA`; later checks reported
`5.083750 V / 184.25 mA` and `5.078750 V / 183.25 mA`. ADC0/PD values are not
claimed here.

## Task-v2

The empty store started at revision `4` after earlier diagnostics. Two tasks
were written through `POST /api/v1/tasks/mutations` with canonical base64:

- Task A: source present, 148 source bytes, 100 IR bytes.
- Task B: IR-only, 100 IR bytes.
- Full IR: 184 bytes, SHA-256
  `b2298f2e1f8b31e99a63a7916460941eea303dc34dc032b1b66a7e63021c5d3f`.
- Task A source SHA-256:
  `bfd3006056fd18bce3a2b1db4818f96079030b0976d846b040efe2a72ee9e1f1`.

Verified:

- Upsert A changed the revision to `5`; upsert B changed it to `6`.
- `GET /api/v1/tasks/ir` matched the expected A+B full IR byte-for-byte and its
  advertised digest.
- `GET /api/v1/tasks/slots/0/source` matched the 148 source bytes exactly and
  its digest.
- A stale `expected_revision` delete returned HTTP 409 `revision_conflict` and
  left the revision unchanged.
- After `POST /api/v1/reboot`, the store ID, revision `6`, both task records, the
  full IR, and the source bytes were unchanged and byte-exact.
- Delete B produced the exact A-only IR and retained A's source.
- Clear produced an empty store, zero full-IR bytes, and kept `task_store_id`.
- Full-IR import of A+B produced the exact expected IR; both tasks reported
  `source_state:"absent"`.
- Final clear produced revision `10`.
- A missing source returned HTTP 404 `task_source_absent`.
- Legacy `PUT /api/v1/tasks` and `DELETE /api/v1/tasks` returned HTTP 405.
- The first Task mutation initially reset the board. Root cause: the ~700 B
  mutation outcome lived on the 3 KiB HTTP server stack. Moving it to a
  single-owner static object made the same clear/upsert path complete in
  milliseconds; the final image includes that fix.

## Network identity

`GET /api/v1/net` after first boot and after reboot returned:

```json
{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"net","action":"get","mac":"d6:1a:42:5c:5d:b0","origin":"derived","network":"10.123.5.148","prefix":30,"ip":"10.123.5.149","dhcp_base":"10.123.5.150","url":"http://10.123.5.149/","reconnect_required":false}
```

- The MAC is locally administered and unicast.
- The host DHCP client received `10.123.5.150/30`; the board `/30` netmask was
  applied after adding `net_if_ipv4_set_netmask_by_addr`.
- The identity persisted across normal reboot and across a full-chip erase plus
  reflash (the same chip ID rederived the same values).
- `/`, `/assets/app.css`, and `/assets/app.js` returned HTTP 200.
- A live WebSocket session subscribed to `topic:"live"` and received the
  `subscribe` result followed by a `snapshot` containing `5v_otg`.
- CDC ACM `/dev/ttyACM3` returned `ip`, `task show`, `config show`, and
  `task clear`; the low-stack clear path printed `task clear ok` and the board
  remained alive, then `task show` reported `task_count=0`. `ip subnet 204` is
  rejected as `unknown ip subcommand` instead of silently printing identity.
- DHCP option 114 is configured with an empty override, so Zephyr builds the URI
  from the current server address and `/generate_204`; firmware serves that path
  with the capport JSON. The DHCP ACK option bytes were not captured in this
  run; the host lease/netmask was verified.

## OTG API cascade

`5v_otg` is present with `signal:"OTG_USB_EN"` and `gp:23`.

- With `vdd_5v` off, setting `5v_otg=on` reported `on`.
- Turning `vdd_5v` on kept `5v_otg` on.
- Turning `vdd_5v` off cleared `5v_otg` to `off`.
- Setting `5v_usb_hub=on` turned `vdd_5v` on.

## Residual gaps

- The current `/api/v1/power` payload still exposes only the simple
  `state`/`value` pair for `5v_otg`; it does not yet expose
  `desired`/`requested`/`effective`/`backfeed`. The C port-state model exists,
  but the HTTP encoder was not switched to it.
- No DMM/oscilloscope/current-limited-source measurement was performed for the
  Q10 `5V_OTG` path, backfeed through the body diode, or USB VBUS behavior.
- DHCP option 114 URI bytes and OS captive-portal auto-open were not captured on
  the wire in this run.
- `PUT /api/v1/net` and the Web NetworkCard browser flow were not exercised on
  hardware; only GET identity, persistence across reboot/erase, and HTTP asset
  serving were verified.
- Task max-size payloads (4000 B source/IR), four-slot capacity, concurrent
  clients, and power loss during a commit were not re-run on hardware; those
  remain covered by the host-model tests and the A/B manifest design.
- A task HTTP client that disappears without a COMPLETE/ABORTED callback can
  still leave the static task owner busy until reboot; the task resources are
  not covered by the generic fallback-holder reaper. A corrupt task manifest
  similarly requires full-chip erase/reflash; `clear` is rejected.
- Concurrent partial identity updates from HTTP and CDC are still separate
  read-modify-write operations and can roll back the other field. The CLI
  retains its legacy `172.29.203.1` fallback when neither `--url` nor
  `LINKR_BOARD_URL` is set, so new boards require an explicit URL. An unusable
  chip ID with no persisted blob still prevents HTTP startup; there is no
  full-manual bootstrap path for that case.
- The Web UI does not yet render the OTG effective/blocked/backfeed warning or
  the PD-0V rail/logo blink requirement.
- The USB NCM host-side `iMACAddress` string is still the Zephyr compile-time
  `remote-mac-address`; only the board-side link address is derived. A host
  network manager that rejects duplicate host-side iMACAddress values still
  needs a host-side workaround.

## Task storage usage and save gate

`GET /api/v1/tasks` on the final image returned a measured storage object:

```json
{"total_bytes":131072,"used_bytes":5532,"free_bytes":125540,
 "task_bytes":680,"reserve_bytes":8192,"measured":true,
 "save_allowed":true}
```

After Task A upsert, `task_bytes` rose from 680 to 928 (source 148 + IR 100);
`free_bytes` fell to 125209. After clear, a subsequent GET returned
`task_bytes:680` and `free_bytes:125540`; NVS compaction reclaimed the deleted
entries. Firmware rejects `upsert`/`import` when
`free_bytes < reserve_bytes + required_new_bytes`; `delete` and `clear` remain
allowed. The low-space branch is covered by the host-model test that forces
`free=0` and asserts `storage_full` with zero writes. The normal HIL image did
not fill NVS to force a real low-space 507; the healthy board reported
`save_allowed:true` throughout.

The Web TaskCard shows NVS used/free and task payload, and disables Store/Import
when `measured=true` and `save_allowed=false`; this is covered by Web tests.
The CLI `task list` prints the same fields
(`nvs_total=131072 nvs_used=5524 nvs_free=125548 task_bytes=680 reserve=8192
measured=true save_allowed=true`).
