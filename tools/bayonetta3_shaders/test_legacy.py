#!/usr/bin/env python3
"""Real-resource regression: legacy control admission must preserve every byte.

Requires the five privately retained original wrappers from unsupported-analysis.
No game content is embedded in this test.
"""
import importlib.util
import struct
import sys
import unittest
from pathlib import Path
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('b3', Path(__file__).resolve().parents[1]/'bayonetta3_shaders.py')
b3 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b3)

class LegacyControl(unittest.TestCase):
    def test_original_records_and_malformed_variants(self):
        source = b3.WORK/'unsupported-analysis'
        wrappers = sorted(source.glob('*.vso')) + sorted(source.glob('*.pso'))
        self.assertEqual(len(wrappers), 5, 'original resources must be available')
        expected = {'ec71abb017c6b21c', '2f6f41fef22c54cb', '6f573d87d0373cb9', 'd6d29346d09f35e4', '937ed53c65588188'}
        actual = set()
        for p in wrappers:
            raw = p.read_bytes()
            _, _, cs, co, bs, bo = struct.unpack_from('<6I', raw)
            row, ctrl, code = b3.shader_record(raw)
            actual.add(row['code_sha1_16'])
            self.assertEqual(ctrl, raw[co:co+cs])
            self.assertEqual(code, raw[bo:bo+bs])
            self.assertEqual(cs, 0x7d1)
            self.assertEqual(struct.unpack_from('<I',ctrl,0x20)[0],cs)
            for offset, value in [(co+8, 10), (co+0x20, 0x880), (co+0x704, bs+1), (co+0x708, bs), (co+0x6fc, bs+1)]:
                modified = bytearray(raw)
                struct.pack_into('<I',modified,offset,value)
                with self.assertRaises(ValueError, msg=f'{p.name} offset={offset:#x}'):
                    b3.shader_record(modified)
        self.assertEqual(actual,expected)

if __name__ == '__main__':
    unittest.main()
