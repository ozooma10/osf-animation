interface Window {
  osfui?: {
    available?: boolean | (() => boolean);
    ready?: Promise<Record<string, unknown>>;
    send?: (name: string, payload?: Record<string, unknown>) => boolean;
    request?: (name: string, payload?: Record<string, unknown>, opts?: { timeoutMs?: number }) => Promise<unknown>;
    on?: (name: string, listener: (payload: unknown) => void) => () => void;
    state?: {
      get?: (key: string) => unknown;
      on?: (key: string, listener: (value: unknown) => void) => () => void;
    };
  };
  mockOpenWheel?: (withTarget?: boolean) => void;
  /** Set by the OSF UI CLI harness bootstrap. Absent in game. */
  __osfuiHarness?: unknown;
}
