import { useCallback, useEffect, useRef, useState } from "react";
import * as api from "@/lib/api";
import type {
  PersistentConfig,
  PersistentConfigApiError,
  PersistentConfigSummary,
} from "@/lib/persistentConfig";
import { PersistentConfigApiError as ConfigError } from "@/lib/persistentConfig";

export type PersistentConfigMutation = "save" | "apply" | "clear";
export type UsePersistentConfigOptions = {
  readonly connected: boolean;
  readonly summary?: PersistentConfigSummary;
  readonly currentStateKey: string;
};
export type UsePersistentConfig = {
  readonly config: PersistentConfig | null;
  readonly error: PersistentConfigApiError | null;
  readonly loading: boolean;
  readonly busy: PersistentConfigMutation | null;
  readonly supported: boolean;
  readonly refresh: () => Promise<void>;
  readonly save: (items: readonly string[], confirm: boolean) => Promise<void>;
  readonly apply: (confirm: boolean) => Promise<void>;
  readonly clear: () => Promise<void>;
};

type Authority = {
  readonly config: PersistentConfig;
  readonly generation: number;
};
type AuthorityWaiter = {
  readonly requiredGeneration: number;
  readonly resolve: () => void;
  readonly reject: (error: PersistentConfigApiError) => void;
};

function sameSummary(summary: PersistentConfigSummary | undefined): string {
  return summary
    ? `${summary.available}:${summary.reason}:${summary.savedCount}:${summary.pendingCount}`
    : "unsupported";
}

function lifecycleError(code: "disconnected" | "unsupported" | "unmounted") {
  const messages = {
    disconnected: "Config board is disconnected",
    unsupported: "Config firmware is unsupported",
    unmounted: "Config hook is unmounted",
  } as const;
  return new ConfigError({ kind: "other", code }, messages[code]);
}

function requestError(caught: unknown): PersistentConfigApiError {
  return caught instanceof ConfigError
    ? caught
    : new ConfigError({ kind: "other", code: null }, "Config request failed");
}

export function usePersistentConfig(options: UsePersistentConfigOptions): UsePersistentConfig {
  const { connected: isConnected, currentStateKey, summary } = options;
  const [authority, setAuthority] = useState<Authority | null>(null);
  const [error, setError] = useState<PersistentConfigApiError | null>(null);
  const [loading, setLoading] = useState(false);
  const [busy, setBusy] = useState<PersistentConfigMutation | null>(null);
  const latestGeneration = useRef(0);
  const acceptedGeneration = useRef(0);
  const authorityWaiters = useRef<AuthorityWaiter[]>([]);
  const mutationQueue = useRef(Promise.resolve());
  const mounted = useRef(true);
  const connected = useRef(isConnected);
  const supported = useRef(summary !== undefined);

  const rejectAllWaiters = useCallback((nextError: PersistentConfigApiError) => {
    const pending = authorityWaiters.current;
    authorityWaiters.current = [];
    for (const waiter of pending) waiter.reject(nextError);
  }, []);

  const rejectWaitersThrough = useCallback((generation: number, nextError: PersistentConfigApiError) => {
    const remaining: AuthorityWaiter[] = [];
    for (const waiter of authorityWaiters.current) {
      if (waiter.requiredGeneration <= generation) waiter.reject(nextError);
      else remaining.push(waiter);
    }
    authorityWaiters.current = remaining;
  }, []);

  const waitForAuthority = useCallback((requiredGeneration: number): Promise<void> => {
    if (acceptedGeneration.current >= requiredGeneration) return Promise.resolve();
    return new Promise<void>((resolve, reject) => {
      authorityWaiters.current.push({ requiredGeneration, resolve, reject });
    });
  }, []);

  const availabilityError = useCallback((): PersistentConfigApiError | null => {
    if (!mounted.current) return lifecycleError("unmounted");
    if (!connected.current) return lifecycleError("disconnected");
    if (!supported.current) return lifecycleError("unsupported");
    return null;
  }, []);

  useEffect(() => {
    if (authority === null) return;
    acceptedGeneration.current = authority.generation;
    const remaining: AuthorityWaiter[] = [];
    for (const waiter of authorityWaiters.current) {
      if (waiter.requiredGeneration <= authority.generation) waiter.resolve();
      else remaining.push(waiter);
    }
    authorityWaiters.current = remaining;
  }, [authority]);

  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
      latestGeneration.current += 1;
      rejectAllWaiters(lifecycleError("unmounted"));
    };
  }, [rejectAllWaiters]);

  const refresh = useCallback((): Promise<void> => {
    const unavailable = availabilityError();
    if (unavailable) return Promise.reject(unavailable);
    const generation = ++latestGeneration.current;
    setLoading(true);
    const completion = waitForAuthority(generation);
    void api.getPersistentConfig().then((next) => {
      if (
        mounted.current && connected.current && supported.current &&
        generation === latestGeneration.current
      ) {
        setAuthority({ config: next, generation });
        setError(null);
        setLoading(false);
      }
    }).catch((caught: unknown) => {
      const nextError = requestError(caught);
      if (
        mounted.current && connected.current && supported.current &&
        generation === latestGeneration.current
      ) {
        setError(nextError);
        setLoading(false);
        rejectWaitersThrough(generation, nextError);
      }
    });
    return completion;
  }, [availabilityError, rejectWaitersThrough, waitForAuthority]);

  const mutate = useCallback((
    activity: PersistentConfigMutation,
    operation: () => Promise<unknown>
  ): Promise<void> => {
    const run = async () => {
      const unavailable = availabilityError();
      if (unavailable) throw unavailable;
      setBusy(activity);
      setError(null);
      try {
        await operation();
        await refresh();
      } catch (caught) {
        const nextError = requestError(caught);
        if (mounted.current) setError(nextError);
        throw nextError;
      } finally {
        if (mounted.current) setBusy(null);
      }
    };
    const queued = mutationQueue.current.then(run, run);
    mutationQueue.current = queued.catch((caught: unknown) => {
      if (!(caught instanceof ConfigError)) throw caught;
    });
    return queued;
  }, [availabilityError, refresh]);

  const summaryKey = sameSummary(summary);
  useEffect(() => {
    connected.current = isConnected;
    supported.current = summaryKey !== "unsupported";
    if (!supported.current) {
      latestGeneration.current += 1;
      rejectAllWaiters(lifecycleError("unsupported"));
      setLoading(false);
      setError(null);
      setAuthority(null);
      return;
    }
    if (!connected.current) {
      latestGeneration.current += 1;
      rejectAllWaiters(lifecycleError("disconnected"));
      setLoading(false);
      return;
    }
    void refresh().catch((caught: unknown) => {
      if (!(caught instanceof ConfigError)) throw caught;
    });
  }, [currentStateKey, isConnected, refresh, rejectAllWaiters, summaryKey]);

  return {
    config: authority?.config ?? null,
    error,
    loading,
    busy,
    supported: summary !== undefined,
    refresh,
    save: (items, confirm) => mutate("save", () => api.savePersistentConfig(items, confirm)),
    apply: (confirm) => mutate("apply", () => api.applyPersistentConfig(confirm)),
    clear: () => mutate("clear", api.clearPersistentConfig),
  };
}
