import { createContext, useContext, useMemo, type ReactNode } from "react";
import { powerRailCatalog } from "@/lib/power";
import type { AdcReading, PowerOutput } from "@/lib/types";

const CatalogContext = createContext<ReturnType<typeof powerRailCatalog>>({ switchable: [], current: [], measurements: [] });

/** The current firmware descriptors are shared with nested workflow inspectors. */
export function PowerRailCatalogProvider({ outputs, readings, children }: {
  outputs: readonly PowerOutput[];
  readings: readonly AdcReading[];
  children: ReactNode;
}) {
  const catalog = useMemo(() => powerRailCatalog(outputs, readings), [outputs, readings]);
  return <CatalogContext.Provider value={catalog}>{children}</CatalogContext.Provider>;
}

export const usePowerRailCatalog = () => useContext(CatalogContext);
