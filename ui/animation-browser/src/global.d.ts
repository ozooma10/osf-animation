interface Window {
  osfui?: {
    postMessage?: (message: string) => void;
    onMessage?: (message: string) => void;
  };
  mockOpenWheel?: (withTarget?: boolean) => void;
}
