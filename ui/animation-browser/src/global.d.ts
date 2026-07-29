interface Window {
  osfui?: {
    postMessage?: (message: string) => void;
    onMessage?: (message: string) => void;
  };
  mockOpenWheel?: (withTarget?: boolean) => void;
  /** Set by the OSF UI CLI harness bootstrap. Absent in game. */
  __osfuiHarness?: unknown;
}
