// Injected into the private staged view only. Never shipped in the browser addon.
(() => {
  const bridge = window.osfui;
  if (!bridge) return;
  let runId = "", clicked = false;
  const seen = new Set();
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
      imports: seen.has("imports"), wheel: seen.has("wheel"),
      wheelVisible: visible(document.querySelector(".wheel")),
      clickedTrusted: clicked,
      importsOpen: button?.getAttribute("aria-pressed") === "true",
      target: rect ? { x: (rect.x + rect.width / 2) / innerWidth, y: (rect.y + rect.height / 2) / innerHeight } : null
    });
  }
  for (const name of ["catalog", "library", "imports", "wheel"]) {
    bridge.on(`osf.animation.${name}.data`, () => { seen.add(name); requestAnimationFrame(report); });
  }
  document.addEventListener("click", event => {
    if (event.isTrusted && event.target.closest?.("button.imports-btn")) {
      clicked = true;
      requestAnimationFrame(() => requestAnimationFrame(report));
    }
  }, true);
  bridge.on("osf.animation.smoke.probe", payload => {
    if (runId !== payload.runId) { clicked = false; seen.clear(); }
    runId = payload.runId;
    for (const name of ["catalog", "library", "imports", "wheel"]) bridge.send(`osf.animation.${name}.get`, {});
    requestAnimationFrame(report);
  });
})();
