import { describe, expect, it } from "vitest";
import { timelineFrame } from "../src/features/browse/timeline";

describe("active timeline", () => {
  it("reports frame numbers on the scrubber's 30 fps clock", () => {
    expect(timelineFrame(0)).toBe(0);
    expect(timelineFrame(1 / 30)).toBe(1);
    expect(timelineFrame(1.25)).toBe(38);
  });
});
