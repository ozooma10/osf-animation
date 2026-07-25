import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import * as THREE from "three";
import { decodeAf } from "../src/afDecoder";

interface Fixture {
  name: string;
  af: string;
  rig: string;
  categories: string[];
}

interface NativeSample {
  ratio: number;
  locals: number[][];
}

interface NativeResult {
  schema: number;
  joints: number;
  duration: number;
  tracks: number;
  samples: NativeSample[];
}

const manifestPath = process.env.OSF_AF_PARITY_FIXTURES;
const nativeTool = process.env.OSF_AF_IMPORT_TEST;
const fixtures: Fixture[] = manifestPath
  ? JSON.parse(readFileSync(manifestPath, "utf8")) as Fixture[]
  : [];
const parity = manifestPath && nativeTool ? describe : describe.skip;

function sampleBrowser(fixture: Fixture, ratios: number[]) {
  const af = readFileSync(fixture.af);
  const rig = readFileSync(fixture.rig);
  const decoded = decodeAf(
    af.buffer.slice(af.byteOffset, af.byteOffset + af.byteLength),
    rig.buffer.slice(rig.byteOffset, rig.byteOffset + rig.byteLength),
    fixture.name,
  );
  const clip = decoded.animations[0];
  const mixer = new THREE.AnimationMixer(decoded.scene);
  const action = mixer.clipAction(clip);
  action.play();
  const bones: THREE.Bone[] = [];
  decoded.scene.traverse((object) => {
    if (object instanceof THREE.Bone) bones.push(object);
  });
  const samples = ratios.map((ratio) => {
    action.time = clip.duration * ratio;
    mixer.update(0);
    decoded.scene.updateMatrixWorld(true);
    return bones.map((bone) => {
      bone.updateMatrix();
      return [...bone.matrix.elements];
    });
  });
  mixer.stopAllAction();
  mixer.uncacheRoot(decoded.scene);
  return { decoded, duration: clip.duration, samples };
}

parity("browser/native AF decoder parity", () => {
  it("covers the stabilization corpus categories", () => {
    const categories = new Set(fixtures.flatMap((fixture) => fixture.categories));
    for (const required of [
      "one-byte-counters",
      "two-byte-counters",
      "skipped-atlas-runs",
      "preamble",
      "partial-body",
      "long",
      "human",
      "creature",
    ]) {
      expect(categories.has(required), `missing local parity category: ${required}`).toBe(true);
    }
  });

  for (const fixture of fixtures) {
    it(fixture.name, () => {
      const native = JSON.parse(execFileSync(nativeTool!, [
        "--json",
        fixture.af,
        fixture.rig,
      ], { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 })) as NativeResult;
      const browser = sampleBrowser(fixture, native.samples.map((sample) => sample.ratio));
      expect(browser.decoded.boneCount).toBe(native.joints);
      expect(browser.duration).toBeCloseTo(native.duration, 5);
      expect(browser.samples).toHaveLength(native.samples.length);
      browser.samples.forEach((sample, sampleIndex) => {
        expect(sample).toHaveLength(native.samples[sampleIndex].locals.length);
        sample.forEach((matrix, jointIndex) => {
          matrix.forEach((value, valueIndex) => {
            expect(value).toBeCloseTo(native.samples[sampleIndex].locals[jointIndex][valueIndex], 3);
          });
        });
      });
    });
  }
});

