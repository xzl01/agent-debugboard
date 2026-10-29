// Normalized current contract; historical HIL measurements are intentionally
// not generated from these values. The tests compare this data to firmware.
export const SNAPSHOT_CONTRACT = Object.freeze({
  version: 1, headerBytes: 12, entryBytes: 4,
  powerItems: 6, switchItems: 2, gpioItems: 11, nvsSectors: 32,
});
const { version, headerBytes, entryBytes, powerItems, switchItems, gpioItems } = SNAPSHOT_CONTRACT;
export const ITEM_COUNT = powerItems + switchItems + gpioItems;
export const MAX_ENCODED_BYTES = headerBytes + ITEM_COUNT * entryBytes;
export const HEADER_SUMMARY = `${headerBytes}B-header;byte4-version=${version};byte7-zero;max-${MAX_ENCODED_BYTES}B`;
export const SIZE_FACT = `size is ${MAX_ENCODED_BYTES} bytes for the ${headerBytes}-byte header and ${ITEM_COUNT} entries of ${entryBytes} bytes each`;
export const CATALOG_FACT = `catalog is exactly ${ITEM_COUNT} items: ${powerItems} power outputs, ${switchItems} switches, and ${gpioItems} safe GPIOs`;
export const NVS_FACT = `CONFIG_SETTINGS_NVS_SECTOR_COUNT=${SNAPSHOT_CONTRACT.nvsSectors}`;

export function summaryTable(rows) {
  return ["| Contract ID | Frozen literal |", "| --- | --- |",
    ...rows.map(([id, value]) => `| \`${id}\` | \`${value.replaceAll("|", "\\|")}\` |`)].join("\n");
}
