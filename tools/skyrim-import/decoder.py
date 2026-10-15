"""Isolated PyNifly loading and shared-boundary HKX block assembly.

PyNifly's current upstream decoder treats consecutive spline blocks as sharing
one boundary frame. Some installed versions instead concatenate disjoint
blocks. Decode each block independently so either implementation produces the
same frame sequence, without modifying or copying the installed addon.

Reference: BadDogSkyrim/PyNifly, io_scene_nifly/hkx/anim_fo4.py,
_decompress_spline (first_frame = block_idx * (max_frames_per_block - 1)).
"""
from __future__ import annotations

import hashlib
import importlib.util
import math
from numbers import Real
from pathlib import Path
import sys
from types import ModuleType
from uuid import uuid4

sys.dont_write_bytecode = True


def _integer(value, name, minimum):
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise ValueError(f"{name} must be an integer >= {minimum}")


def _validate_tracks(tracks, num_tracks, num_frames):
    if len(tracks) != num_tracks:
        raise ValueError("HKX decoder returned the wrong number of tracks")
    for track_index, track in enumerate(tracks):
        for name, width in (("translations", 3), ("rotations", 4), ("scales", 3)):
            values = getattr(track, name, None)
            if values is None or len(values) != num_frames:
                raise ValueError(f"HKX track {track_index} has incomplete {name}")
            for sample in values:
                if len(sample) != width or any(
                    isinstance(value, bool) or not isinstance(value, Real) or not math.isfinite(value)
                    for value in sample
                ):
                    raise ValueError(f"HKX track {track_index} has malformed {name}")


def _wrap_decompressor(original):
    def decompress(data_bytes, num_tracks, num_frames, num_blocks,
                   max_frames_per_block, block_offsets, mask_and_quant_size=0):
        for value, name, minimum in (
            (num_tracks, "num_tracks", 1), (num_frames, "num_frames", 1),
            (num_blocks, "num_blocks", 1), (max_frames_per_block, "max_frames_per_block", 1),
            (mask_and_quant_size, "mask_and_quant_size", 0),
        ):
            _integer(value, name, minimum)
        if not isinstance(data_bytes, (bytes, bytearray, memoryview)) or not data_bytes:
            raise ValueError("HKX spline data must be a nonempty byte buffer")
        if len(block_offsets) != num_blocks:
            raise ValueError("HKX block offset count does not match num_blocks")
        previous = -1
        for offset in block_offsets:
            _integer(offset, "block offset", 0)
            if offset <= previous or offset >= len(data_bytes):
                raise ValueError("HKX block offsets must be increasing and inside the spline buffer")
            previous = offset
        if max_frames_per_block == 1:
            expected_blocks = 1 if num_frames == 1 else 0
        else:
            step = max_frames_per_block - 1
            expected_blocks = max(1, (num_frames - 1 + step - 1) // step)
        if num_blocks != expected_blocks:
            raise ValueError("HKX frame count is inconsistent with overlapping spline blocks")

        result = None
        for index, offset in enumerate(block_offsets):
            first_frame = index * (max_frames_per_block - 1)
            count = min(max_frames_per_block, num_frames - first_frame)
            # Keep the entire byte buffer: compressed data alignment is relative
            # to that buffer, so slicing at an arbitrary block offset is unsafe.
            tracks = original(data_bytes, num_tracks, count, 1,
                              max_frames_per_block, [offset], mask_and_quant_size)
            _validate_tracks(tracks, num_tracks, count)
            if result is None:
                result = tracks
            else:
                for destination, source in zip(result, tracks):
                    for name in ("translations", "rotations", "scales"):
                        getattr(destination, name).extend(getattr(source, name)[1:])
        _validate_tracks(result, num_tracks, num_frames)
        return result

    return decompress


def load_decoder(pynifly_path: str | Path) -> ModuleType:
    """Load the installed decoder into a private package and normalize blocks.

    ``pynifly_path`` is the io_scene_nifly addon directory. The return value
    exposes PyNifly's public ``load_skyrim_animation``/``load_skyrim_skeleton``
    functions plus ``osf_decoder_fingerprints`` for the import receipt.
    """
    directory = Path(pynifly_path).resolve() / "hkx"
    sources = [directory / "anim_fo4.py", directory / "anim_skyrim.py"]
    for path in sources:
        if not path.is_file():
            raise ValueError(f"Missing PyNifly decoder: {path}")
    fingerprints = [{"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                    for path in sources]
    # A fresh synthetic package avoids importing the Blender addon __init__, and
    # prevents a different source path or previous import from poisoning imports.
    package_name = "_osf_hkx_" + uuid4().hex
    package = ModuleType(package_name)
    package.__path__ = [str(directory)]
    sys.modules[package_name] = package
    name = package_name + ".anim_skyrim"
    spec = importlib.util.spec_from_file_location(name, sources[1])
    if spec is None or spec.loader is None:
        raise ValueError("Could not create a PyNifly decoder module")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    try:
        spec.loader.exec_module(module)
        module._decompress_spline = _wrap_decompressor(module._decompress_spline)
        module.osf_decoder_fingerprints = fingerprints
        module.osf_block_policy = "shared-boundary-v1"
    except Exception:
        for key in tuple(sys.modules):
            if key == package_name or key.startswith(package_name + "."):
                del sys.modules[key]
        raise
    return module
