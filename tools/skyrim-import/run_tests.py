"""Run the importer's tests in Python, or all tests in background Blender."""
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
suite = unittest.defaultTestLoader.discover(str(Path(__file__).resolve().parent / "tests"))
result = unittest.TextTestRunner(verbosity=2).run(suite)
if not result.wasSuccessful():
    raise RuntimeError("Importer tests failed")
