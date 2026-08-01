import { describe, expect, it } from "vitest";
import { timelineFrame, timelineMarkMoment, timelineTracks } from "../src/features/browse/timeline";

describe("active timeline", () => {
  it("reports frame numbers on the scrubber's 30 fps clock", () => {
    expect(timelineFrame(0)).toBe(0);
    expect(timelineFrame(1 / 30)).toBe(1);
    expect(timelineFrame(1.25)).toBe(38);
  });

  it("groups authored marks into stable named lanes and sorts them by time", () => {
    const tracks = timelineTracks([
      { kind: "sound", at: 0.8, anchor: "fraction", label: "loud", detail: "", role: "f", repeat: false },
      { kind: "cue", at: 1, anchor: "end", label: "done", detail: "", role: "", repeat: false },
      { kind: "sound", at: 0.2, anchor: "fraction", label: "low", detail: "", role: "f", repeat: true },
    ]);
    expect(tracks.map((track) => [track.kind, track.marks.map((mark) => mark.label)])).toEqual([
      ["cue", ["done"]],
      ["sound", ["low", "loud"]],
    ]);
    expect(timelineMarkMoment(tracks[1].marks[0], 3)).toBe("20% · F18");
    expect(timelineMarkMoment(tracks[0].marks[0], 3)).toBe("END");
  });
});
