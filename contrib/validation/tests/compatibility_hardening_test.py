#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded negative tests using the production offline parser and fixtures."""
import hashlib
import random
import struct
import unittest
from compatibility_corpus_test import c, fake_pe, envelope, MODERN, LEGACY, row

class HardeningTests(unittest.TestCase):
    def safe(self, data):
        first = c.analyze_bytes(data, provider_requested=True)
        self.assertEqual(c.canonical(first), c.canonical(c.analyze_bytes(data, provider_requested=True)))
        self.assertFalse(first['runtime_authorized'])
        self.assertTrue(all(not item.get('runtime_authorized', False) for item in first['capabilities'].values()))
        return first

    def test_truncated_section_table_and_invalid_headers(self):
        good = fake_pe(MODERN)
        for cut in range(0x188, 0x1d9):
            self.assertEqual(self.safe(good[:cut])['pe_status'], 'INVALID')
        for offset, value in [(0x3c, 0xffffffff), (0x98+56, 0xffffffff),
                              (0x98+60, 0x190), (0x188+12, 0xfffffff0),
                              (0x188+40+12, 0x1000)]:
            data = bytearray(good); struct.pack_into('<I', data, offset, value)
            self.assertEqual(self.safe(bytes(data))['pe_status'], 'INVALID')

    def test_corrupted_export_tables(self):
        good = fake_pe(MODERN)
        for offset, value in [(0x98+112, 0xffffff00), (0x98+116, 0xffffffff),
                              (0x600+20, 0x10001), (0x600+24, 0x10001),
                              (0x600+28, 0xfffffff0), (0x600+32, 0xfffffff0),
                              (0x600+36, 0xfffffff0)]:
            data = bytearray(good); struct.pack_into('<I', data, offset, value)
            self.assertEqual(self.safe(bytes(data))['pe_status'], 'INVALID')

    def test_zero_one_duplicate_signatures(self):
        body = b'\x31\xc0\x90\xc3'
        template = [{'bytes': len(body), 'sha256': c.sha(body), '_body': body}]
        for count, state in [(0, 'UNAVAILABLE'), (1, 'STRUCTURAL_MATCH'), (2, 'AMBIGUOUS'), (3, 'AMBIGUOUS')]:
            found = c.match_code(c.PEView(fake_pe(text=body*count)), template)
            self.assertEqual(found['state'], state)
            self.assertFalse(found['runtime_authorized'])

    def test_modified_fatbin_and_unknown_ptx(self):
        original = fake_pe(fatbin=envelope())
        mutated = bytearray(original); mutated[0x1600+16+64+10] ^= 1
        a, b = self.safe(original), self.safe(bytes(mutated))
        self.assertNotEqual(a['sha256'], b['sha256'])
        self.assertNotEqual(a['provider_payload_identity'], b['provider_payload_identity'])
        for body in (b'unknown opaque layout\0', b'.version 99.9\n.target sm_999\nunknown;\0'):
            report = self.safe(fake_pe(fatbin=envelope(body)))
            self.assertFalse(report['capabilities']['retarget_runtime']['runtime_authorized'])
        corrupt = bytearray(original); struct.pack_into('<Q', corrupt, 0x1600+8, 0xffffffffffffffff)
        self.assertTrue(self.safe(bytes(corrupt))['fatbin_errors'])

    def test_missing_modules_and_replacement(self):
        interposer = row('sl.interposer.dll', MODERN)
        common = row('sl.common.dll', ['slGetPluginFunction'])
        plugin = row('sl.dlss_g.dll', ['slGetPluginFunction'])
        full = [interposer, common, plugin]
        for missing in ('sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll'):
            result = c.classify_bundle([item for item in full if item['file_name'] != missing])
            self.assertFalse(result['runtime_authorized'])
            if missing != 'sl.dlss_g.dll':
                self.assertEqual(result['assembly_family'], 'unknown-safe')
            else:
                self.assertFalse(result['dlssg_plugin_present'])
                self.assertEqual(result['assembly_family'], 'modern-export-surface')
        # Changed file identity with unchanged PE timestamp/image size.
        replacement = row('sl.interposer.dll', LEGACY)
        self.assertEqual(interposer['pe_timestamp'], replacement['pe_timestamp'])
        self.assertEqual(interposer['size_of_image'], replacement['size_of_image'])
        self.assertNotEqual(interposer['sha256'], replacement['sha256'])
        for _ in range(32):
            self.assertEqual(c.classify_bundle([replacement, common])['assembly_family'], 'legacy-export-surface')
            self.assertEqual(c.classify_bundle([common])['assembly_family'], 'unknown-safe')
            self.assertEqual(c.classify_bundle(full)['assembly_family'], 'modern-export-surface')

    def test_seeded_structural_mutations(self):
        rng = random.Random(0x4d464733)
        source = fake_pe(MODERN, fatbin=envelope())
        # Bounded, reproducible fuzz-like corpus. Not a claim of exhaustive fuzzing.
        for _ in range(2048):
            data = bytearray(source)
            for _ in range(rng.randrange(1, 5)):
                at = rng.choice([rng.randrange(0x200), rng.randrange(0x600, 0xa00),
                                 rng.randrange(0x1600, 0x1700)])
                data[at] ^= rng.randrange(1, 256)
            self.safe(bytes(data))

if __name__ == '__main__':
    unittest.main()
