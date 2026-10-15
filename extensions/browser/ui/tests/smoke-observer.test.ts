import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";
import { describe, expect, it } from "vitest";

const source = readFileSync(new URL("../../../../test/smoke/browser-observer.js", import.meta.url), "utf8");

function observer() {
  const handlers = new Map<string, (payload: unknown) => void>();
  const reports: Record<string, unknown>[] = [];
  const footer = { textContent: "1 scene fits 1 actor · + animations" };
  const element = {
    getBoundingClientRect: () => ({ x: 10, y: 20, width: 100, height: 30 }),
    getAttribute: () => "false",
  };
  runInNewContext(source, {
    window: { osfui: {
      on: (name: string, callback: (payload: unknown) => void) => handlers.set(name, callback),
      send: (name: string, payload: unknown) => {
        if (name === "osf.animation.smoke.observed") reports.push(JSON.parse(JSON.stringify(payload)));
      },
    } },
    document: {
      querySelector: (selector: string) => selector === ".step-foot" ? footer : element,
      addEventListener() {},
    },
    requestAnimationFrame: (callback: () => void) => callback(),
    getComputedStyle: () => ({ visibility: "visible" }),
    innerWidth: 1920, innerHeight: 1080, TextEncoder,
  });
  const emit = (name: string, value: unknown) => handlers.get(`osf.animation.${name}`)?.(value);
  emit("smoke.probe", { runId: "test-run" });
  return { emit, footer, last: () => reports.at(-1)! };
}

function pieces(text: string, id = "1") {
  const split = Math.floor(text.length / 2);
  return [text.slice(0, split), text.slice(split)].map((data, index) => ({
    osfAnimationTransfer: 1, id, index, count: 2, bytes: new TextEncoder().encode(text).length, data,
  }));
}

describe("private live browser observer", () => {
  it("requires a complete decoded library and separate rendered application evidence", () => {
    const probe = observer();
    const chunks = pieces(JSON.stringify([{ id: "large-library", name: "Café" }]));
    probe.emit("library.data", chunks[1]);
    expect(probe.last().library).toBe(false);
    probe.emit("library.data", chunks[0]);
    expect(probe.last()).toMatchObject({ library: true, libraryRendered: false, feedCounts: { library: 1 } });
    probe.footer.textContent = "1 scene fits 1 actor · animations 774";
    probe.emit("smoke.probe", { runId: "test-run" });
    expect(probe.last().libraryRendered).toBe(true);
  });

  it("never counts truncated, inconsistent or invalid-JSON transfers as received feeds", () => {
    for (const invalid of [
      [{ ...pieces("[{}]")[0], count: 2048 }],
      [{ ...pieces("[{}]")[0], bytes: 64 * 1024 * 1024 }],
      [pieces("[{}]")[0], { ...pieces("[{}]")[1], bytes: 999 }],
      pieces("not-json"),
      pieces("{}"),
    ]) {
      const probe = observer();
      for (const part of invalid) probe.emit("library.data", part);
      probe.emit("smoke.probe", { runId: "test-run" });
      expect(probe.last().library).toBe(false);
    }
  });

  it("ignores duplicate parts and stale transfers without mixing their contents", () => {
    const probe = observer();
    const old = pieces('[{"id":"old"}]', "1");
    const fresh = pieces('[{"id":"new"},{"id":"two"}]', "2");
    probe.emit("library.data", fresh[0]);
    probe.emit("library.data", fresh[0]);
    for (const part of old) probe.emit("library.data", part);
    expect(probe.last().library).toBe(false);
    probe.emit("library.data", fresh[1]);
    expect(probe.last()).toMatchObject({ library: true, feedCounts: { library: 2 } });
    probe.emit("smoke.probe", { runId: "new-run" });
    expect(probe.last()).toMatchObject({ runId: "new-run", library: false });
  });
});
