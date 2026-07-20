function Header() {
  return (
    <>
      <div class="slate">
        <div class="slate-cell slate-brand">
          <div class="brand-lockup">
            <div class="brand-emblem"><span /></div>
            <div class="brand-meta">
              <div class="brand-title">Animation Browser</div>
              <div class="brand-sub">
                <span id="lamp" class="lamp" data-state="wait" />
                <span id="statusText">connecting…</span>
              </div>
            </div>
          </div>
          <div class="brand-tools">
            <button id="refresh" class="iconbtn" type="button" title="Refresh catalog">⟳</button>
            <div class="debug-wrap">
              <span class="lbl">DEBUG</span>
              <button id="debugToggle" class="switch" type="button" aria-label="Toggle debug mode"><i /></button>
            </div>
            <span class="brand-tool-sep" />
            <button class="iconbtn" type="button" title="Minimize — watch the scene" data-act="minimize">
              <svg class="chev-ico" width="10" height="9" viewBox="0 0 10 9" aria-hidden="true">
                <path d="M1 1l4 3.8L9 1"/><path d="M1.2 8h7.6"/>
              </svg>
            </button>
          </div>
        </div>
        <div class="slate-cell slate-take" id="slateTake" />
      </div>
      <div class="stripe"><span /><span /><span /></div>
    </>
  );
}

function Console() {
  return (
    <div class="console">
      <span class="bracket tl"/><span class="bracket tr"/>
      <span class="bracket bl"/><span class="bracket br"/>
      <div class="grid-overlay"/>
      <Header />
      <div class="director">
        <aside id="rail" class="rail"/>
        <section id="browse" class="browse"/>
      </div>
      <footer id="notice" class="notice" aria-live="polite"/>
    </div>
  );
}

export function App() {
  return (
    <div class="stage">
      <Console />
      <aside id="brief" class="brief"/>
      <div id="livebar" class="livebar"/>
      <div id="wheel" class="wheel"/>
    </div>
  );
}
