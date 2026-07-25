import * as THREE from "three";

const AF_FPS = 30;
const SQRT_HALF = Math.SQRT1_2;

class Cursor {
  readonly view: DataView;
  offset = 0;

  constructor(buffer: ArrayBuffer) {
    this.view = new DataView(buffer);
  }

  align(alignment: number) {
    if (alignment > 1 && this.offset % alignment) {
      this.offset += alignment - (this.offset % alignment);
    }
  }

  ensure(size: number) {
    if (this.offset + size > this.view.byteLength) throw new Error("File is truncated.");
  }

  skip(size: number) {
    this.ensure(size);
    this.offset += size;
  }

  u8() { this.ensure(1); return this.view.getUint8(this.offset++); }
  i8() { this.ensure(1); return this.view.getInt8(this.offset++); }
  u16(alignment = 1) {
    this.align(alignment); this.ensure(2);
    const value = this.view.getUint16(this.offset, true); this.offset += 2; return value;
  }
  i16(alignment = 1) {
    this.align(alignment); this.ensure(2);
    const value = this.view.getInt16(this.offset, true); this.offset += 2; return value;
  }
  u32(alignment = 1) {
    this.align(alignment); this.ensure(4);
    const value = this.view.getUint32(this.offset, true); this.offset += 4; return value;
  }
  f32(alignment = 1) {
    this.align(alignment); this.ensure(4);
    const value = this.view.getFloat32(this.offset, true); this.offset += 4; return value;
  }
  u64() {
    this.ensure(8);
    const value = Number(this.view.getBigUint64(this.offset, true)); this.offset += 8; return value;
  }
  bytes(size: number, alignment = 1) {
    this.align(alignment); this.ensure(size);
    const value = new Uint8Array(this.view.buffer, this.offset, size); this.offset += size; return value;
  }
  counter(size: number) {
    return size === 2 ? this.u16(2) : this.u8();
  }
}

interface RigBone {
  name: string;
  parent: number;
  rotation: THREE.Quaternion;
  translation: THREE.Vector3;
}

interface RigData {
  bones: RigBone[];
  animatedBoneCount: number;
  lowPrecision: number;
  highPrecision: number;
}

interface RotationPrefix {
  first: number;
  second: number;
  third: number;
  firstFlag: boolean;
  secondFlag: boolean;
  thirdFlag: boolean;
  count: number;
  missing: number;
}

interface TranslationPrefix {
  x: number;
  y: number;
  z: number;
  count: number;
}

interface AnimationBlock {
  rotationKeys: number[];
  translationKeys: number[];
  rotationEntries: Array<[number, number, number]>;
  translationEntries: Array<[number, number, number]>;
  rotationPrefixes: RotationPrefix[];
  translationPrefixes: TranslationPrefix[];
}

export interface DecodedAf {
  scene: THREE.Group;
  animations: THREE.AnimationClip[];
  boneCount: number;
  frameCount: number;
}

function readCString(bytes: Uint8Array, offset: number): string {
  if (!Number.isSafeInteger(offset) || offset < 0 || offset >= bytes.length) {
    throw new Error("skeleton.rig contains an invalid bone-name offset.");
  }
  let end = offset;
  while (end < bytes.length && bytes[end] !== 0) end += 1;
  return new TextDecoder().decode(bytes.subarray(offset, end));
}

function parseRig(buffer: ArrayBuffer): RigData {
  const cursor = new Cursor(buffer);
  cursor.skip(48);
  const lowPrecision = cursor.f32();
  const highPrecision = cursor.f32();
  const boneCount = cursor.u16();
  const animatedBoneCount = cursor.u16();
  cursor.skip(20);
  if (cursor.offset !== 80 || !boneCount || boneCount > 4096) {
    throw new Error("skeleton.rig has an invalid header.");
  }

  const partial: Array<Omit<RigBone, "name"> & { nameOffset: number }> = [];
  for (let index = 0; index < boneCount; index += 1) {
    const w = cursor.f32();
    const x = cursor.f32();
    const y = cursor.f32();
    const z = cursor.f32();
    cursor.skip(16);
    const translation = new THREE.Vector3(cursor.f32(), cursor.f32(), cursor.f32());
    cursor.u32();
    const nameOffset = cursor.u64();
    const parent = cursor.view.getInt32(cursor.offset, true);
    cursor.offset += 4;
    cursor.skip(28);
    cursor.f32();
    cursor.skip(4);
    partial.push({
      nameOffset,
      parent,
      rotation: new THREE.Quaternion(x, y, z, w).normalize(),
      translation,
    });
  }
  const bytes = new Uint8Array(buffer);
  const bones = partial.map((bone) => ({
    name: readCString(bytes, bone.nameOffset),
    parent: bone.parent,
    rotation: bone.rotation,
    translation: bone.translation,
  }));
  return { bones, animatedBoneCount, lowPrecision, highPrecision };
}

function signed7(value: number) {
  const payload = value & 0x7f;
  return payload >= 64 ? payload - 128 : payload;
}

function readBlock(cursor: Cursor, counterSize: number, keySize: number): AnimationBlock {
  const rotationCount = cursor.counter(counterSize);
  const rotationPrefixCount = cursor.counter(counterSize);
  const translationCount = cursor.counter(counterSize);
  const translationPrefixCount = cursor.counter(counterSize);
  const scalarCount = cursor.counter(counterSize);
  const priorityCount = cursor.counter(counterSize);
  const rotationKeys = Array.from({ length: rotationCount }, () => cursor.counter(keySize));
  const translationKeys = Array.from({ length: translationCount }, () => cursor.counter(keySize));
  for (let index = 0; index < scalarCount + priorityCount; index += 1) cursor.counter(keySize);

  const rotationEntries = Array.from({ length: rotationCount }, () => {
    const value = cursor.bytes(3);
    return [new DataView(value.buffer, value.byteOffset, 3).getInt8(0),
      new DataView(value.buffer, value.byteOffset, 3).getInt8(1),
      new DataView(value.buffer, value.byteOffset, 3).getInt8(2)] as [number, number, number];
  });
  const rotationPrefixes = Array.from({ length: rotationPrefixCount }, () => {
    const value = cursor.bytes(4);
    return {
      first: signed7(value[0]), second: signed7(value[1]), third: signed7(value[2]),
      firstFlag: Boolean(value[0] >> 7), secondFlag: Boolean(value[1] >> 7),
      thirdFlag: Boolean(value[2] >> 7), count: value[3] & 0x3f, missing: value[3] >> 6,
    };
  });
  const translationEntries = Array.from({ length: translationCount }, () => {
    const value = cursor.bytes(3);
    const view = new DataView(value.buffer, value.byteOffset, 3);
    return [view.getInt8(0), view.getInt8(1), view.getInt8(2)] as [number, number, number];
  });
  const translationPrefixes = Array.from({ length: translationPrefixCount }, () => {
    const value = cursor.bytes(8, 2);
    const view = new DataView(value.buffer, value.byteOffset, 8);
    return { x: view.getInt16(0, true), y: view.getInt16(2, true), z: view.getInt16(4, true), count: view.getUint16(6, true) };
  });
  for (let index = 0; index < scalarCount; index += 1) cursor.bytes(2, 2);
  for (let index = 0; index < priorityCount; index += 1) cursor.bytes(1);
  return { rotationKeys, translationKeys, rotationEntries, translationEntries, rotationPrefixes, translationPrefixes };
}

function parseAf(buffer: ArrayBuffer) {
  const cursor = new Cursor(buffer);
  cursor.skip(36);
  const flags = cursor.bytes(4);
  const counterSize = 1 + ((flags[0] >> 1) & 1);
  const keySize = 1 + ((flags[0] >> 2) & 1);
  cursor.i16();
  const boneCount = cursor.u16();
  const frameCount = cursor.u16();
  const atlasLength = cursor.u16();
  const fillCount = cursor.u16();
  const preambleLength = cursor.u16();
  cursor.skip(12);
  if (cursor.offset !== 64 || !boneCount || atlasLength < 2) {
    throw new Error(".af has an invalid header.");
  }
  if (preambleLength > 0) {
    const count = cursor.u32(4);
    for (let entry = 0; entry < count; entry += 1) {
      const size = cursor.u16(2);
      for (let index = 0; index < size * 2; index += 1) cursor.f32(4);
      for (let index = 0; index < frameCount; index += 1) cursor.f32(4);
      for (let index = 0; index < size; index += 1) cursor.i16(2);
      for (let index = 0; index < size; index += 1) cursor.i8();
    }
  }
  for (let index = 0; index < fillCount; index += 1) cursor.f32(4);
  cursor.align(4);
  const atlas = cursor.bytes(atlasLength);
  if (atlas.reduce((sum, value) => sum + value, 0) !== boneCount) {
    throw new Error(".af animation atlas does not match its bone count.");
  }

  const blocks = new Map<number, AnimationBlock>();
  let run = 0;
  let runOffset = 0;
  for (let bone = 0; bone < boneCount; bone += 1) {
    while (run < atlas.length && runOffset >= atlas[run]) {
      run += 1;
      runOffset = 0;
    }
    if (run >= atlas.length) break;
    if (run % 2 === 1) blocks.set(bone, readBlock(cursor, counterSize, keySize));
    runOffset += 1;
  }
  return { boneCount, frameCount, blocks };
}

function dequantRotation(prefix: RotationPrefix, entry: [number, number, number]) {
  const low = SQRT_HALF / 64;
  const high = SQRT_HALF / 16384;
  const values = [
    low * prefix.first + high * entry[0] * (1 << (3 * Number(prefix.firstFlag))),
    low * prefix.second + high * entry[1] * (1 << (3 * Number(prefix.secondFlag))),
    low * prefix.third + high * entry[2] * (1 << (3 * Number(prefix.thirdFlag))),
  ];
  const missing = Math.sqrt(Math.max(0, 1 - values.reduce((sum, value) => sum + value * value, 0)));
  values.splice(prefix.missing, 0, missing);
  return new THREE.Quaternion(values[0], values[1], values[2], values[3]).normalize();
}

function unfold<T extends { count: number }>(values: T[], extra: number) {
  return values.flatMap((value) => Array.from({ length: value.count + extra }, () => value));
}

export function decodeAf(afBuffer: ArrayBuffer, rigBuffer: ArrayBuffer, name = "AF Animation"): DecodedAf {
  const rig = parseRig(rigBuffer);
  const af = parseAf(afBuffer);
  if (af.boneCount > rig.animatedBoneCount || af.boneCount > rig.bones.length) {
    throw new Error(`.af expects ${af.boneCount} animated bones, but this rig supports ${rig.animatedBoneCount}.`);
  }

  const scene = new THREE.Group();
  scene.name = "AF_Root";
  const objects = rig.bones.map((bone, index) => {
    const object = new THREE.Bone();
    object.name = THREE.PropertyBinding.sanitizeNodeName(bone.name || `Bone_${index}`);
    object.position.copy(bone.translation);
    object.quaternion.copy(bone.rotation);
    return object;
  });
  objects.forEach((object, index) => {
    const parent = rig.bones[index].parent;
    if (parent >= 0 && parent < objects.length) objects[parent].add(object);
    else scene.add(object);
  });

  const tracks: THREE.KeyframeTrack[] = [];
  let duration = Math.max(af.frameCount / AF_FPS, 1 / AF_FPS);
  af.blocks.forEach((block, boneIndex) => {
    const bone = rig.bones[boneIndex];
    const object = objects[boneIndex];
    if (!bone || !object) return;
    const rotationPrefixes = unfold(block.rotationPrefixes, 1);
    const rotationCount = Math.min(block.rotationKeys.length, block.rotationEntries.length, rotationPrefixes.length);
    if (rotationCount) {
      const times: number[] = [];
      const values: number[] = [];
      for (let index = 0; index < rotationCount; index += 1) {
        const relative = dequantRotation(rotationPrefixes[index], block.rotationEntries[index]);
        const absolute = bone.rotation.clone().multiply(relative).normalize();
        const time = block.rotationKeys[index] / AF_FPS;
        times.push(time);
        values.push(absolute.x, absolute.y, absolute.z, absolute.w);
        duration = Math.max(duration, time);
      }
      tracks.push(new THREE.QuaternionKeyframeTrack(`${object.name}.quaternion`, times, values));
    }

    const translationPrefixes = unfold(block.translationPrefixes, 0);
    const translationCount = Math.min(block.translationKeys.length, block.translationEntries.length, translationPrefixes.length);
    if (translationCount) {
      const times: number[] = [];
      const values: number[] = [];
      for (let index = 0; index < translationCount; index += 1) {
        const prefix = translationPrefixes[index];
        const entry = block.translationEntries[index];
        const relative = new THREE.Vector3(
          prefix.x * rig.lowPrecision + entry[0] * rig.highPrecision,
          prefix.y * rig.lowPrecision + entry[1] * rig.highPrecision,
          prefix.z * rig.lowPrecision + entry[2] * rig.highPrecision,
        ).applyQuaternion(bone.rotation);
        const absolute = bone.translation.clone().add(relative);
        const time = block.translationKeys[index] / AF_FPS;
        times.push(time);
        values.push(absolute.x, absolute.y, absolute.z);
        duration = Math.max(duration, time);
      }
      tracks.push(new THREE.VectorKeyframeTrack(`${object.name}.position`, times, values));
    }
  });

  const clip = new THREE.AnimationClip(name.replace(/\.af$/i, ""), duration, tracks);
  return { scene, animations: [clip], boneCount: rig.bones.length, frameCount: af.frameCount };
}
