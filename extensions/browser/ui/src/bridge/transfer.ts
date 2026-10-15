import { isRecord } from "./contract";

// Keep in sync with BrowserJsonTransport.h. Envelopes use the original event name;
// only complete documents may cross the bridge boundary into application state.
export const JSON_TRANSFER_LIMITS = {
  chunkBytes: 64 * 1024,
  bytes: 32 * 1024 * 1024,
  chunks: 1024,
  active: 8,
  bufferedBytes: 64 * 1024 * 1024,
  timeoutMs: 30_000,
} as const;

type Result = { kind: "message"; payload: unknown } | { kind: "pending" } | { kind: "error"; message: string };
interface Transfer {
  count: number;
  bytes: number;
  receivedBytes: number;
  receivedCount: number;
  started: number;
  parts: Array<string | undefined>;
}
interface Receipt { id: string; transfer?: Transfer }
const pending: Result = { kind: "pending" };
const encoder = new TextEncoder();
const newer = (left: string, right: string) => left.length > right.length || (left.length === right.length && left > right);

export class JsonTransferDecoder {
  private readonly receipts = new Map<string, Receipt>();

  constructor(private readonly now: () => number = Date.now) {}

  // Keep completed IDs when hidden, so late chunks cannot resurrect canceled data.
  reset(): void {
    for (const receipt of this.receipts.values()) receipt.transfer = undefined;
  }

  consume(type: string, payload: unknown): Result {
    const now = this.now();
    for (const receipt of this.receipts.values()) {
      if (receipt.transfer && now - receipt.transfer.started >= JSON_TRANSFER_LIMITS.timeoutMs) receipt.transfer = undefined;
    }
    let receipt = this.receipts.get(type);
    if (!isRecord(payload) || !("osfAnimationTransfer" in payload)) {
      if (receipt) receipt.transfer = undefined;
      return { kind: "message", payload };
    }
    const { id, index, count, bytes, data } = payload;
    const validId = typeof id === "string" && /^[1-9]\d{0,19}$/.test(id) &&
      (id.length < 20 || id <= "18446744073709551615");
    const fail = (message: string): Result => {
      if (receipt && receipt.id === id) receipt.transfer = undefined;
      return { kind: "error", message: `${type}: ${message}` };
    };
    if (payload.osfAnimationTransfer !== 1 || !validId || !Number.isSafeInteger(index) || !Number.isSafeInteger(count) ||
        index < 0 || count < 1 || count > JSON_TRANSFER_LIMITS.chunks || index >= count ||
        !Number.isSafeInteger(bytes) || bytes < count || bytes > JSON_TRANSFER_LIMITS.bytes || bytes > count * JSON_TRANSFER_LIMITS.chunkBytes ||
        typeof data !== "string" || data.length === 0 || data.length > JSON_TRANSFER_LIMITS.chunkBytes) {
      return fail("invalid JSON transfer envelope");
    }
    const chunkBytes = encoder.encode(data).length;
    if (chunkBytes > JSON_TRANSFER_LIMITS.chunkBytes || chunkBytes > bytes) return fail("invalid JSON transfer chunk size");
    if (receipt && id !== receipt.id && !newer(id, receipt.id)) return pending;
    if (!receipt || id !== receipt.id) {
      // A newer version supersedes an incomplete older version of this event.
      if (receipt) receipt.transfer = undefined;
      receipt = { id };
      this.receipts.set(type, receipt);
      const active = [...this.receipts.values()].filter((value) => value.transfer);
      const reserved = active.reduce((total, value) => total + value.transfer!.bytes, 0);
      if (active.length >= JSON_TRANSFER_LIMITS.active || reserved + bytes > JSON_TRANSFER_LIMITS.bufferedBytes) {
        return fail("JSON transfer buffer limit exceeded");
      }
      receipt.transfer = { count, bytes, receivedBytes: 0, receivedCount: 0, started: now, parts: new Array(count) };
    }
    const transfer = receipt.transfer;
    if (!transfer) return pending; // Complete, timed out, canceled, or rejected ID.
    if (count !== transfer.count || bytes !== transfer.bytes) return fail("JSON transfer metadata changed");
    const previous = transfer.parts[index];
    if (previous !== undefined) return previous === data ? pending : fail("conflicting JSON transfer duplicate");
    if (transfer.receivedBytes + chunkBytes > bytes) return fail("JSON transfer exceeds declared size");
    transfer.parts[index] = data;
    transfer.receivedBytes += chunkBytes;
    transfer.receivedCount++;
    if (transfer.receivedCount !== count) return pending;
    receipt.transfer = undefined;
    if (transfer.receivedBytes !== bytes) return fail("JSON transfer size mismatch");
    try {
      return { kind: "message", payload: JSON.parse(transfer.parts.join("")) };
    } catch {
      return fail("invalid JSON transfer document");
    }
  }
}
