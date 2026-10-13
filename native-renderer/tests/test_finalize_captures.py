"""Synthetic legacy-conversion tests. Retains test evidence; never launches the game."""
from pathlib import Path
import importlib.util
import sys
import unittest
import time
import numpy as np
from PIL import Image
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
spec = importlib.util.spec_from_file_location('finalize_captures', HERE / 'finalize-captures.py')
F = importlib.util.module_from_spec(spec)
spec.loader.exec_module(F)
ROOT = HERE.parent.parent / 'out' / ('task029-conversion-' + str(time.time_ns()))

class Finalize(unittest.TestCase):
    def setUp(self):
        self.run = ROOT / self._testMethodName
        self.run.mkdir(parents=True)
        self.rgb = np.random.default_rng(29).integers(0, 256, (19, 31, 3), dtype=np.uint8)

    def test_lossless_conversion_and_preservation(self):
        source = self.run / 'shot_80.bmp'
        Image.fromarray(self.rgb).save(source)
        original = source.read_bytes()
        result = F.finalize(self.run, ROOT / 'archived')
        self.assertEqual(result['converted'], 1)
        self.assertFalse(source.exists())
        self.assertEqual((Path(result['archived_originals']) / source.name).read_bytes(), original)
        np.testing.assert_array_equal(F.load_image(self.run / 'shot_80.png'), self.rgb)
        self.assertEqual(F.finalize(self.run, ROOT / 'archived')['converted'], 0)

    def test_conflicting_png_preserves_originals(self):
        Image.fromarray(self.rgb).save(self.run / 'shot_80.bmp')
        Image.fromarray(np.zeros_like(self.rgb)).save(self.run / 'shot_80.png')
        with self.assertRaises(ValueError):
            F.finalize(self.run, ROOT / 'archived')
        self.assertTrue((self.run / 'shot_80.bmp').exists())

    def test_userdata_not_converted(self):
        userdata = self.run / 'userdata'
        userdata.mkdir()
        Image.fromarray(self.rgb).save(userdata / 'title.bmp')
        self.assertEqual(F.finalize(self.run, ROOT / 'archived')['converted'], 0)
        self.assertTrue((userdata / 'title.bmp').exists())

if __name__ == '__main__':
    unittest.main()
