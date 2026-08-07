import type { TimelineTrackKind, TimelineTrackMark } from "../../model";

export const TIMELINE_FPS = 30;

export const TIMELINE_LANES: readonly { kind: TimelineTrackKind; label: string }[] = [
  { kind: "cue", label: "CUES" },
  { kind: "action", label: "ACTIONS" },
  { kind: "sound", label: "SOUNDS" },
  { kind: "camera", label: "CAMERA" },
];

export function timelineFrame(seconds: number): number {
  return Number.isFinite(seconds) ? Math.max(0, Math.round(seconds * TIMELINE_FPS)) : 0;
}

export function timelineTracks(marks: readonly TimelineTrackMark[]) {
  return TIMELINE_LANES.map((lane) => ({
    ...lane,
    marks: marks.filter((mark) => mark.kind === lane.kind).sort((a, b) => a.at - b.at),
  })).filter((lane) => lane.marks.length > 0);
}

/** Effective rail fraction: an `atFrame` mark re-places itself against the live decoded
 *  duration when one is known — the catalog's fraction was computed from the pack-authored
 *  clip length, which can disagree with what actually decoded. */
export function timelineMarkAt(mark: TimelineTrackMark, duration: number): number {
  if (mark.atSec != null && duration > 0) return Math.max(0, Math.min(1, mark.atSec / duration));
  return mark.at;
}

export function timelineMarkMoment(mark: TimelineTrackMark, duration: number): string {
  if (mark.trackPosition === "unreachable") return `PAST END · F${timelineFrame(mark.atSec ?? 0)}`;
  if (mark.trackPosition !== "fraction") return mark.trackPosition.toUpperCase();
  if (mark.atSec != null) {
    const frame = `F${timelineFrame(mark.atSec)}`;
    return duration > 0 ? `${Math.round(timelineMarkAt(mark, duration) * 100)}% · ${frame}` : frame;
  }
  const percent = `${Math.round(mark.at * 100)}%`;
  return duration > 0 ? `${percent} · F${timelineFrame(mark.at * duration)}` : percent;
}
