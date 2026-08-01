export const TIMELINE_FPS = 30;

export function timelineFrame(seconds: number): number {
  return Number.isFinite(seconds) ? Math.max(0, Math.round(seconds * TIMELINE_FPS)) : 0;
}
