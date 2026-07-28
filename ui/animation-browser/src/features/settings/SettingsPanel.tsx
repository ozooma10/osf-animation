import type { BrowserCommands } from "../../app/commands";
import type { BrowserPreferences, BrowserState } from "../../app/state";

interface Choice<T extends string> {
  value: T;
  label: string;
  title?: string;
}

function ChoiceRow<T extends string>({ label, hint, value, choices, onChange }: {
  label: string;
  hint: string;
  value: T;
  choices: readonly Choice<T>[];
  onChange(value: T): void;
}) {
  return <div class="setting-row">
    <div class="setting-copy"><strong>{label}</strong><span>{hint}</span></div>
    <div class="setting-choices">
      {choices.map((choice) => <button key={choice.value} class={value === choice.value ? "on" : ""}
        type="button" title={choice.title} aria-pressed={value === choice.value}
        onClick={() => onChange(choice.value)}>{choice.label}</button>)}
    </div>
  </div>;
}

function ToggleRow({ label, hint, enabled, onChange }: {
  label: string;
  hint: string;
  enabled: boolean;
  onChange(enabled: boolean): void;
}) {
  return <div class="setting-row">
    <div class="setting-copy"><strong>{label}</strong><span>{hint}</span></div>
    <button class={`settings-switch ${enabled ? "on" : ""}`} type="button" role="switch"
      aria-checked={enabled} onClick={() => onChange(!enabled)}>
      <i/><span>{enabled ? "ON" : "OFF"}</span>
    </button>
  </div>;
}

export function SettingsPanel({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  const set = <K extends keyof BrowserPreferences>(key: K, value: BrowserPreferences[K]) => commands.setPreference(key, value);
  const preferences = state.preferences;
  return <section class="settings-panel" aria-label="Animation Browser settings">
    <div class="settings-head">
      <div><p class="eb">Browser configuration</p><h2>Settings</h2></div>
      <button class="iconbtn" type="button" title="Close settings" aria-label="Close settings" onClick={() => commands.toggleSettings(false)}>×</button>
    </div>
    <div class="settings-scroll">
      <section class="settings-group">
        <h3>Browser</h3>
        <ChoiceRow label="After launching a scene" hint="Choose what happens to this view after a successful launch."
          value={preferences.afterLaunch} onChange={(value) => set("afterLaunch", value)}
          choices={[
            { value: "minimize", label: "Live controls" },
            { value: "stay", label: "Stay open" },
            { value: "close", label: "Close" },
          ]}/>
        <ChoiceRow label="Open browser to" hint="Active falls back to Scenes when nothing is running."
          value={preferences.openTo} onChange={(value) => set("openTo", value)}
          choices={[
            { value: "last", label: "Last used" },
            { value: "scenes", label: "Scenes" },
            { value: "library", label: "Animations" },
            { value: "active", label: "Active" },
          ]}/>
        <ToggleRow label="Remember browsing state" hint="Keep search, filters, and expanded folders between openings in this game session."
          enabled={preferences.rememberBrowsing} onChange={(value) => set("rememberBrowsing", value)}/>
      </section>

      <section class="settings-group">
        <h3>Library</h3>
        <ChoiceRow label="Animation detail" hint="Keep transitions and animation layers folded away, or show every installed clip."
          value={preferences.libraryDetail} onChange={(value) => set("libraryDetail", value)}
          choices={[{ value: "curated", label: "Poses & loops" }, { value: "full", label: "Full library" }]}/>
        <ChoiceRow label="Content source" hint="Vanilla animations can be hidden without affecting custom packs."
          value={preferences.librarySource} onChange={(value) => set("librarySource", value)}
          choices={[{ value: "all", label: "Custom + vanilla" }, { value: "custom", label: "Custom only" }]}/>
        <ChoiceRow label="Unavailable scenes" hint="Scenes that need a different cast or furniture can remain optional, always visible, or hidden."
          value={preferences.unavailableScenes} onChange={(value) => set("unavailableScenes", value)}
          choices={[
            { value: "ask", label: "On request" },
            { value: "show", label: "Always below" },
            { value: "hide", label: "Hide" },
          ]}/>
      </section>

      <section class="settings-group">
        <h3>Scene defaults</h3>
        <p class="settings-group-note">These initialize Start Overrides. “Scene” respects each animation pack’s authored policy.</p>
        <ChoiceRow label="Strip actors" hint="Default apparel behavior for newly launched scenes."
          value={preferences.strip} onChange={(value) => set("strip", value)}
          choices={[{ value: "-1", label: "Scene" }, { value: "1", label: "Always" }, { value: "0", label: "Never" }]}/>
        <ChoiceRow label="Lock player controls" hint="Default input lock when the player participates."
          value={preferences.lock} onChange={(value) => set("lock", value)}
          choices={[{ value: "-1", label: "Scene" }, { value: "1", label: "Always" }, { value: "0", label: "Never" }]}/>
        <ChoiceRow label="Camera" hint="Default camera policy; individual launches can still override it."
          value={preferences.camera} onChange={(value) => set("camera", value)}
          choices={[
            { value: "", label: "Scene" },
            { value: "thirdperson_hold", label: "3rd person" },
            { value: "scene_orbit", label: "Orbit" },
            { value: "freefly", label: "Free fly" },
            { value: "vanity_orbit", label: "Vanity" },
          ]}/>
        <ChoiceRow label="Playback speed" hint="Default clock multiplier for browser launches."
          value={preferences.speed} onChange={(value) => set("speed", value)}
          choices={["0.5", "0.75", "1", "1.25", "1.5", "2"].map((value) => ({ value, label: `${value}×` }))}/>
      </section>

      <section class="settings-group">
        <h3>Advanced</h3>
        <ToggleRow label="Show author details" hint="Reveal scene IDs, source files, diagnostics, and generated or unlisted entries."
          enabled={preferences.authorDetails} onChange={(value) => set("authorDetails", value)}/>
        <p class="settings-footnote">Stage-transition popups and log verbosity remain in OSF UI’s OSF Animation settings card.</p>
      </section>
    </div>
  </section>;
}
