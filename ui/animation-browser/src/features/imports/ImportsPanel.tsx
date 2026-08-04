import type { BrowserCommands } from "../../app/commands";
import { formatBytes, formatMillis, importGroups, importOutcome, importOutcomeCounts, importResult, importSeverity, type ImportSeverity } from "../../app/selectors";
import type { BrowserState, ImportFilter } from "../../app/state";
import { importProblemKey, type ImportFile, type ImportProblem } from "../../model";
import { Empty } from "../shared/Shared";

// The per-file import report: one row per *.osf.json the engine scanned, whether or not it
// produced anything. A pack that failed outright has no scene to appear as in the catalog, so
// this listing is the only surface that can answer "why is my pack missing".

const SEVERITY_TITLE: Record<ImportSeverity, string> = {
  error: "Rejected content — something in this file did not load",
  warn: "Loaded with warnings",
  note: "Loaded, but contributed nothing",
  ok: "Loaded cleanly",
};

function Readout({ label, value, tone = "" }: { label: string; value: string; tone?: "" | "warn" | "error" }) {
  return <div class={`imp-readout ${tone}`}>
    <span class="lbl">{label}</span>
    <strong class="mono">{value}</strong>
  </div>;
}

function Summary({ state }: { state: BrowserState }) {
  const totals = state.importTotals;
  return <div class="imp-summary">
    <Readout label="Files" value={String(totals.files)}/>
    <Readout label="Authored scenes" value={String(totals.declaredScenes)}/>
    <Readout label="Scenes" value={String(totals.scenes)}/>
    <Readout label="Rejected scenes" value={String(totals.rejectedScenes)} tone={totals.rejectedScenes ? "error" : ""}/>
    <Readout label="Routes" value={String(totals.routes)}/>
    <Readout label="Authored routes" value={String(totals.declaredRoutes)}/>
    <Readout label="Rejected routes" value={String(totals.rejectedRoutes)} tone={totals.rejectedRoutes ? "error" : ""}/>
    <Readout label="Clip entries" value={String(totals.clipEntries)}/>
    <Readout label="Errors" value={String(totals.errors)} tone={totals.errors ? "error" : ""}/>
    <Readout label="Warnings" value={String(totals.warnings)} tone={totals.warnings ? "warn" : ""}/>
    <Readout label="Hidden" value={String(totals.hidden)} tone={totals.hidden ? "warn" : ""}/>
    <Readout label="Missing clips" value={String(totals.missingClips)} tone={totals.missingClips ? "warn" : ""}/>
    <Readout label="Load time" value={formatMillis(totals.parseMs)}/>
    <Readout label="On disk" value={formatBytes(totals.bytes)}/>
  </div>;
}

/** The one-line trait strip — what an author scans down the list looking for. */
function traits(file: ImportFile): string {
  const parts: string[] = [];
  parts.push(`${file.scenes} scene${file.scenes === 1 ? "" : "s"}`);
  if (file.routes) parts.push(`${file.routes} route${file.routes === 1 ? "" : "s"}`);
  if (file.clipEntries) parts.push(`${file.clipEntries} clip entr${file.clipEntries === 1 ? "y" : "ies"}`);
  if (file.stages) parts.push(`${file.stages} stage${file.stages === 1 ? "" : "s"}`);
  if (file.distinctClips) parts.push(`${file.distinctClips} clip${file.distinctClips === 1 ? "" : "s"}`);
  if (file.missingClips) parts.push(`${file.missingClips} missing`);
  if (file.hidden) parts.push(`${file.hidden} hidden`);
  return parts.join(" · ");
}

function Detail({ file }: { file: ImportFile }) {
  // Only the fields that carry information for this file: a grid of zeros teaches nothing.
  const cells: { label: string; value: string }[] = [
    { label: "Schema", value: file.schema ? String(file.schema) : "—" },
    { label: "Size", value: formatBytes(file.bytes) },
    { label: "Load", value: formatMillis(file.parseMs) },
    { label: "Scenes", value: String(file.scenes) },
    { label: "Routes", value: String(file.routes) },
    { label: "Nodes", value: String(file.nodes) },
    { label: "Stages", value: String(file.stages) },
    { label: "Roles", value: String(file.roles) },
    { label: "Clip slots", value: String(file.clips) },
    { label: "Distinct clips", value: String(file.distinctClips) },
  ];
  if (file.missingClips) cells.push({ label: "Missing clips", value: String(file.missingClips) });
  if (file.hidden) cells.push({ label: "Hidden scenes", value: String(file.hidden) });
  if (file.unlisted) cells.push({ label: "Unlisted", value: String(file.unlisted) });
  if (file.anchored) cells.push({ label: "Anchor-bound", value: String(file.anchored) });
  if (file.rejectedScenes) cells.push({ label: "Rejected scenes", value: String(file.rejectedScenes) });
  if (file.rejectedRoutes) cells.push({ label: "Rejected routes", value: String(file.rejectedRoutes) });
  if (file.clipEntries) cells.push({ label: "Clip entries", value: String(file.clipEntries) });
  if (file.cues) cells.push({ label: "Cues", value: String(file.cues) });
  if (file.actions) cells.push({ label: "Actions", value: String(file.actions) });
  if (file.sounds) cells.push({ label: "Sounds", value: String(file.sounds) });
  if (file.cameras) cells.push({ label: "Cameras", value: String(file.cameras) });
  if (file.species.length) cells.push({ label: "Species", value: file.species.join(", ") });

  const hiddenProblems = file.problemCount - file.problems.length;
  return <div class="imp-detail">
    {!!file.missingClipExamples.length && <div class="imp-missing">
      <span class="lbl">Missing clip examples</span>
      <div>{file.missingClipExamples.map((clip) => <code key={clip}>{clip}</code>)}</div>
    </div>}
    <details class="imp-technical">
      <summary>Technical details</summary>
    <div class="imp-grid">
      {cells.map((cell) => <div class="imp-cell" key={cell.label}>
        <span class="lbl">{cell.label}</span><span class="mono">{cell.value}</span>
      </div>)}
    </div>
    </details>
    {!!file.problems.length && <ul class="imp-problems">
      {file.problems.map((problem, index) => {
        const context = problemContext(problem);
        return <li class={`imp-problem ${problem.severity}`} key={`${problem.code}-${index}`}>
          <div class="imp-problem-line">
            <span class="imp-problem-tag mono">{problem.severity}</span>
            <code class="imp-problem-code">{problem.code}</code>
            <span class="imp-problem-text">{problem.message}</span>
          </div>
          {!!context.length && <div class="imp-problem-context">
            {context.map((value) => <span class="mono" key={value}>{value}</span>)}
          </div>}
          {problem.hint && <p class="imp-hint"><strong>Next:</strong> {problem.hint}</p>}
        </li>;
      })}
      {hiddenProblems > 0 && <li class="imp-problem more mono">
        + {hiddenProblems} more available through COPY REPORT and OSFAdvanced.GetSceneLoadErrors()
      </li>}
    </ul>}
  </div>;
}

function problemContext(problem: ImportProblem): string[] {
  const fields = [
    problem.scene && `scene ${problem.scene}`,
    problem.node && `node ${problem.node}`,
    problem.role && `role ${problem.role}`,
    problem.clip && `clip ${problem.clip}`,
  ];
  return fields.filter(Boolean) as string[];
}

function Row({ state, file, commands }: { state: BrowserState; file: ImportFile; commands: BrowserCommands }) {
  const severity = importSeverity(file);
  const crossFile = !file.path;
  const key = file.path || "\0cross-file";
  const open = state.importsExpanded.has(key);
  const problems = file.errors + file.warnings;
  const outcome = importOutcome(file);
  const newProblems = file.problems.filter((problem) => state.importReload.newProblemKeys.has(importProblemKey(file.path, problem))).length;
  return <div class={`imp-row ${severity} ${open ? "open" : ""}`}>
    <div class="imp-head">
    <button class="imp-main" aria-expanded={open} onClick={() => commands.toggleImportFile(key, !open)}>
      <span class="chev">{open ? "▾" : "▸"}</span>
      <span class={`dot ${severity}`} title={SEVERITY_TITLE[severity]}/>
      <span class="imp-name">
        <span class="imp-file">
          {crossFile ? "Registry-wide problems" : file.file}
          {/* A chip, not another dot-separated word: pack labels contain "·" themselves. */}
          {file.library && <span class="imp-lane mono">LIBRARY</span>}
        </span>
        <span class="imp-path mono">{crossFile
          ? "not attributable to one file"
          : `${file.path}${file.pack ? ` · ${file.pack}` : ""}`}</span>
        <span class="imp-result">{importResult(file)}</span>
      </span>
      {!crossFile && <span class="imp-traits mono">{traits(file)}</span>}
      <span class={`imp-badge outcome ${severity}`}>{outcome}</span>
      {!!problems && <span class={`imp-badge ${file.errors ? "error" : "warn"}`}>{problems}</span>}
      {!!newProblems && <span class="imp-badge fresh">{newProblems} NEW</span>}
    </button>
    <div class="imp-row-actions">
      {!crossFile && file.scenes + file.clipEntries > file.hidden &&
        <button type="button" onClick={() => commands.viewImportContent(file.path)}>VIEW CONTENT</button>}
      <button type="button" onClick={() => commands.copyImportReport(file.path)}>COPY REPORT</button>
    </div>
    </div>
    {open && <Detail file={file}/>}
  </div>;
}

export function ImportsPanel({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  const groups = importGroups(state);
  const counts = importOutcomeCounts(state.imports);
  const filters: { id: ImportFilter; label: string; count: number }[] = [
    { id: "attention", label: "Needs attention", count: counts.attention },
    { id: "rejected", label: "Rejected", count: counts.rejected },
    { id: "partial", label: "Partial", count: counts.partial },
    { id: "missing", label: "Missing assets", count: counts.missing },
    { id: "empty", label: "Empty", count: counts.empty },
    { id: "all", label: "All files", count: counts.all },
  ];
  const reload = state.importReload;
  const delta = reload.delta;
  const changedFiles = delta.changedFiles + delta.addedFiles + delta.removedFiles;
  return <section class="settings-panel imports-panel" aria-label="Scene file imports">
    <div class="settings-head">
      <div><p class="eb">Author diagnostics</p><h2>Import workshop</h2></div>
      <div class="imp-head-tools">
        <button class="iconbtn" type="button" title="Re-read the import report" onClick={commands.refreshImports}>⟳</button>
        <button class="iconbtn" type="button" title="Close imports" aria-label="Close imports"
          onClick={() => commands.toggleImports(false)}>×</button>
      </div>
    </div>
    <div class="settings-scroll">
      <Summary state={state}/>
      <div class="imp-workflow">
        <div>
          <p class="eb">Edit → reload → compare</p>
          <strong>Repair packs without leaving the browser</strong>
          <p>Save your JSON or assets, reload all registries, then review exactly what changed.</p>
        </div>
        <button class="primary" type="button" disabled={reload.status === "running"} onClick={commands.reloadImports}>
          {reload.status === "running" ? "RELOADING…" : "RELOAD PACKS"}
        </button>
      </div>
      {reload.status === "running" && <div class="imp-reload running">Reloading scenes, sounds, gear, and clip caches…</div>}
      {reload.status === "error" && <div class="imp-reload error"><strong>Reload failed.</strong> {reload.error}</div>}
      {reload.status === "success" && <div class={`imp-reload ${delta.newProblems.length ? "warn" : "ok"}`}>
        <strong>{reload.scenes} scenes loaded in {formatMillis(reload.durationMs)}.</strong>
        <span>{changedFiles ? `${changedFiles} file${changedFiles === 1 ? "" : "s"} changed.` : "No import outcomes changed."}</span>
        {!!delta.resolvedProblems.length && <span class="resolved">{delta.resolvedProblems.length} resolved</span>}
        {!!delta.newProblems.length && <span class="introduced">{delta.newProblems.length} new</span>}
      </div>}
      {reload.status === "success" && !!(delta.resolvedProblems.length || delta.newProblems.length) &&
        <div class="imp-delta">
          {!!delta.resolvedProblems.length && <details class="resolved">
            <summary>{delta.resolvedProblems.length} resolved diagnostic{delta.resolvedProblems.length === 1 ? "" : "s"}</summary>
            <ul>{delta.resolvedProblems.slice(0, 8).map((problem) =>
              <li key={problem.key}><strong>{problem.file || "Registry-wide"}</strong><code>{problem.code}</code>{problem.message}</li>)}
            </ul>
          </details>}
          {!!delta.newProblems.length && <details class="introduced" open>
            <summary>{delta.newProblems.length} new diagnostic{delta.newProblems.length === 1 ? "" : "s"}</summary>
            <ul>{delta.newProblems.slice(0, 8).map((problem) =>
              <li key={problem.key}><strong>{problem.file || "Registry-wide"}</strong><code>{problem.code}</code>{problem.message}</li>)}
            </ul>
          </details>}
        </div>}
      <div class="imp-outcomes" role="group" aria-label="Filter imports by outcome">
        {filters.map((filter) => <button type="button" key={filter.id}
          class={state.importsFilter === filter.id ? "on" : ""}
          aria-pressed={state.importsFilter === filter.id}
          onClick={() => commands.setImportFilter(filter.id)}>
          {filter.label}<strong>{filter.count}</strong>
        </button>)}
      </div>
      <div class="imp-filterbar">
        <div class="search-field grow">
          <input type="text" value={state.importsSearch} placeholder="⌕ file · pack · code · scene · clip"
            autocomplete="off" spellcheck={false}
            onInput={(event) => commands.setImportSearch(event.currentTarget.value)}/>
        </div>
      </div>
      {!state.importsReceived
        ? <Empty>Reading the import report…</Empty>
        : !state.imports.length
          ? <Empty>No *.osf.json files were found under Data/OSF. No scene pack is installed, or OSF is not deployed where the game runs.</Empty>
          : !groups.length
            ? <Empty>No file matches the current filter.</Empty>
            : <div class="imp-list">{groups.map((group) => <section class="imp-group" key={group.key}>
              <h3>
                <span>{group.label}</span>
                <span class={`imp-group-state ${group.severity}`}>{group.files.length} files · {group.problems} diagnostics</span>
              </h3>
              {group.files.map((file) =>
                <Row key={file.path || "cross-file"} state={state} file={file} commands={commands}/>)}
            </section>)}</div>}
    </div>
  </section>;
}
