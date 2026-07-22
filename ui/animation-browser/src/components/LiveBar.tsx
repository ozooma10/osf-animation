export interface LiveBarProps {
  running: boolean;
  handle: number;
  title: string;
  stage: { current: number; total: number; name: string; nextName?: string } | null;
  canAdvance: boolean;
  onAdvance(): void;
  onStop(): void;
  onExpand(): void;
}

function BrowserChevron() {
  return (
    <svg class="chev-ico" width="10" height="6" viewBox="0 0 10 6" aria-hidden="true">
      <path d="M1 5l4-3.8L9 5"/>
    </svg>
  );
}

export function LiveBar({ running, handle, title, stage, canAdvance, onAdvance, onStop, onExpand }: LiveBarProps) {
  const nextTitle = stage?.nextName ? `Next: ${stage.nextName} (Space)` : "Next animation (Space)";
  return (
    <>
      <div class={`take-chip ${running ? "live" : ""}`}>
        {running ? (
          <>
            <span class="live-dot"/>
            <div class="take-body">
              <span class="lbl">RUNNING · #{handle}</span>
              <strong>{title}</strong>
              {stage && (
                <span class="livebar-stage mono">
                  <span class="stage-idx">{stage.current}/{stage.total}</span>
                  <span class="stage-name">{stage.name.toUpperCase()}</span>
                </span>
              )}
            </div>
          </>
        ) : (
          <div class="take-body"><span class="lbl">STANDBY</span><strong>No scene running</strong></div>
        )}
        {running && canAdvance && <button class="next-mini" onClick={onAdvance} title={nextTitle}>NEXT ▸</button>}
        {running && <button class="stop-mini" onClick={onStop} title="Stop the running scene">■ STOP</button>}
        <button class="expand-mini" onClick={onExpand} title="Bring the browser back"><BrowserChevron/>BROWSER</button>
      </div>
      <div class="livebar-hint mono">
        {running && canAdvance ? "SPACE NEXT · " : ""}
        DRAG/RS ORBIT · WHEEL/LB-RB ZOOM · LT-RT HEIGHT
        {running ? " · MMB/R3 FREECAM" : ""}
      </div>
    </>
  );
}
