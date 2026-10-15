"""Check HKX overlapping block assembly without requiring a local addon."""
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from decoder import _wrap_decompressor, load_decoder


class DecoderTests(unittest.TestCase):
    @staticmethod
    def block_decoder(calls):
        def decode(data, tracks, frames, blocks, maximum, offsets, mask=0):
            calls.append((data, tracks, frames, blocks, maximum, offsets, mask))
            if blocks != 1:
                raise AssertionError("Adapter must ask decoder for independent blocks")
            return [SimpleNamespace(
                translations=[[offsets[0] + f, track, 0] for f in range(frames)],
                rotations=[[0, 0, 0, 1] for _ in range(frames)],
                scales=[[1, 1, 1] for _ in range(frames)],
            ) for track in range(tracks)]
        return decode

    def test_three_blocks_share_boundaries_without_duplicate_or_missing_frames(self):
        calls = []
        data = bytes(64)
        tracks = _wrap_decompressor(self.block_decoder(calls))(data, 2, 8, 3, 4, [0, 3, 6], 8)
        self.assertEqual([row[0] for row in tracks[0].translations], list(range(8)))
        self.assertEqual([call[2] for call in calls], [4, 4, 2])
        self.assertTrue(all(call[0] is data for call in calls))
        self.assertTrue(all(call[3] == 1 and call[6] == 8 for call in calls))
        self.assertTrue(all(len(track.rotations) == len(track.scales) == 8 for track in tracks))

    def test_single_block_is_unchanged_including_full_block(self):
        for count in (1, 3, 4):
            with self.subTest(count=count):
                original = self.block_decoder([])
                expected = original(bytes(64), 2, count, 1, 4, [0])
                actual = _wrap_decompressor(original)(bytes(64), 2, count, 1, 4, [0])
                self.assertEqual(actual, expected)

    def test_exact_shared_boundary_does_not_add_an_empty_block(self):
        tracks = _wrap_decompressor(self.block_decoder([]))(bytes(64), 1, 7, 2, 4, [0, 3])
        self.assertEqual([row[0] for row in tracks[0].translations], list(range(7)))

    def test_rejects_inconsistent_parameters_before_decoding(self):
        invalid = [
            {"num_tracks": 0}, {"num_frames": 0}, {"num_blocks": 0},
            {"num_blocks": 2}, {"max_frames_per_block": 1},
            {"block_offsets": []}, {"block_offsets": [64]},
            {"block_offsets": [-1]}, {"block_offsets": [True]},
            {"data_bytes": b""}, {"mask_and_quant_size": -1},
        ]
        for changes in invalid:
            with self.subTest(changes=changes):
                calls = []
                arguments = dict(data_bytes=bytes(64), num_tracks=1, num_frames=4, num_blocks=1,
                                 max_frames_per_block=4, block_offsets=[0])
                arguments.update(changes)
                with self.assertRaises(ValueError):
                    _wrap_decompressor(self.block_decoder(calls))(**arguments)
                self.assertFalse(calls)
        with self.assertRaises(ValueError):
            _wrap_decompressor(self.block_decoder([]))(bytes(64), 1, 7, 2, 4, [3, 0])

    def test_rejects_incomplete_or_nonfinite_decoder_channels(self):
        for mode in ("tracks", "frames", "vector", "nonfinite"):
            with self.subTest(mode=mode):
                def broken(*args):
                    tracks = self.block_decoder([])(*args)
                    if mode == "tracks":
                        return []
                    if mode == "frames":
                        tracks[0].scales.pop()
                    if mode == "vector":
                        tracks[0].translations[0] = [0, 0]
                    if mode == "nonfinite":
                        tracks[0].rotations[0][0] = float("nan")
                    return tracks
                with self.assertRaises(ValueError):
                    _wrap_decompressor(broken)(bytes(64), 1, 4, 1, 4, [0])

    def test_loader_isolates_addon_imports_and_records_source_hashes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            hkx = root / "hkx"
            hkx.mkdir()
            (root / "__init__.py").write_text("raise AssertionError('Do not import Blender addon')\n")
            (hkx / "anim_fo4.py").write_text("def _decompress_spline(*args): return []\n")
            (hkx / "anim_skyrim.py").write_text("from .anim_fo4 import _decompress_spline\n")
            first = load_decoder(root)
            second = load_decoder(root)
            self.assertNotEqual(first.__name__, second.__name__)
            self.assertEqual(first.osf_block_policy, "shared-boundary-v1")
            self.assertEqual(first.osf_decoder_fingerprints, second.osf_decoder_fingerprints)
            self.assertEqual(len(first.osf_decoder_fingerprints), 2)
            self.assertTrue(all(len(item["sha256"]) == 64 for item in first.osf_decoder_fingerprints))
            self.assertFalse((hkx / "__pycache__").exists())


if __name__ == "__main__":
    unittest.main()
