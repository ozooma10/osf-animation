// Injected into the private staged view only. Never shipped in the browser addon.
(() => {
  const bridge = window.osfui;
  if (!bridge) return;
  let runId = "", clicked = false;
  const seen = new Set();
  const transfers = new Map();
  const counts = {};
  const utf8 = new TextEncoder();
  function observe(name, payload) {
    if (payload?.osfAnimationTransfer === 1) {
      const { id, index, count, bytes, data } = payload;
      if (typeof id !== "string" || !/^[1-9]\d{0,19}$/.test(id) ||
          !Number.isInteger(index) || !Number.isInteger(count) || count < 1 || count > 1024 || index < 0 || index >= count ||
          !Number.isInteger(bytes) || bytes < 1 || bytes > 32 * 1024 * 1024 || typeof data !== "string") return;
      const size = utf8.encode(data).length;
      if (size > 64 * 1024) return;
      let transfer = transfers.get(name);
      if (transfer && BigInt(id) < BigInt(transfer.id)) return;
      if (!transfer || transfer.id !== id) {
        transfer = { id, count, bytes, chunks: new Map(), size: 0, complete: false };
        transfers.set(name, transfer);
      }
      if (transfer.complete || transfer.count !== count || transfer.bytes !== bytes) return;
      if (transfer.chunks.has(index)) {
        if (transfer.chunks.get(index) !== data) transfer.complete = true;
        return;
      }
      transfer.chunks.set(index, data); transfer.size += size;
      if (transfer.size > bytes) { transfer.complete = true; return; }
      if (transfer.chunks.size !== count) return;
      transfer.complete = true;
      if (transfer.size !== bytes) return;
      try { payload = JSON.parse(Array.from({ length: count }, (_, i) => transfer.chunks.get(i)).join("")); }
      catch { return; }
    }
    if ((name === "catalog" || name === "library") && !Array.isArray(payload)) return;
    if (!payload || typeof payload !== "object") return;
    counts[name] = Array.isArray(payload) ? payload.length : null;
    seen.add(name);
    requestAnimationFrame(report);
  }
  const visible = element => !!element && element.getBoundingClientRect().width > 0 &&
    element.getBoundingClientRect().height > 0 && getComputedStyle(element).visibility !== "hidden";
  function report() {
    if (!runId) return;
    const button = document.querySelector("button.imports-btn");
    const rect = button?.getBoundingClientRect();
    bridge.send("osf.animation.smoke.observed", {
      runId,
      documentReady: visible(document.querySelector(".console")),
      catalog: seen.has("catalog"), library: seen.has("library"),
      libraryRendered: /animations\s+[1-9]\d*/.test(document.querySelector(".step-foot")?.textContent || ""),
      feedCounts: counts,
      imports: seen.has("imports"), wheel: seen.has("wheel"),
      wheelVisible: visible(document.querySelector(".wheel")),
      clickedTrusted: clicked,
      importsOpen: button?.getAttribute("aria-pressed") === "true",
      target: rect ? { x: (rect.x + rect.width / 2) / innerWidth, y: (rect.y + rect.height / 2) / innerHeight } : null
    });
  }
  for (const name of ["catalog", "library", "imports", "wheel"]) {
    bridge.on(`osf.animation.${name}.data`, payload => observe(name, payload));
  }
  document.addEventListener("click", event => {
    if (event.isTrusted && event.target.closest?.("button.imports-btn")) {
      clicked = true;
      requestAnimationFrame(() => requestAnimationFrame(report));
    }
  }, true);
  bridge.on("osf.animation.smoke.probe", payload => {
    if (runId !== payload.runId) { clicked = false; seen.clear(); transfers.clear(); }
    runId = payload.runId;
    for (const name of ["catalog", "library", "imports", "wheel"]) bridge.send(`osf.animation.${name}.get`, {});
    requestAnimationFrame(report);
  });
})();
