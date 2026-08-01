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

export function timelineMarkMoment(mark: TimelineTrackMark, duration: number): string {
  if (mark.anchor !== "fraction") return mark.anchor.toUpperCase();
  const percent = `${Math.round(mark.at * 100)}%`;
  return duration > 0 ? `${percent} · F${timelineFrame(mark.at * duration)}` : percent;
}
