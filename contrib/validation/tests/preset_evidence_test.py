#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Synthetic PE -> actual production resolver; never executes vendor code."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
import collect_provider_trials as collect

parser = argparse.ArgumentParser()
parser.add_argument('--probe', type=Path, required=True)
args = parser.parse_args()

def fixture() -> bytes:
    raw = bytearray(0x2400)
    raw[:2] = b'MZ'
    struct.pack_into('<I', raw, 0x3c, 0x80)
    raw[0x80:0x84] = b'PE\x00\x00'
    struct.pack_into('<HHIIIHH', raw, 0x84, 0x8664, 2, 0x12345678, 0, 0, 240, 0x2022)
    opt = 0x98
    struct.pack_into('<H', raw, opt, 0x20b)
    struct.pack_into('<Q', raw, opt + 24, 0x180000000)
    struct.pack_into('<II', raw, opt + 32, 0x1000, 0x200)
    struct.pack_into('<II', raw, opt + 56, 0x3000, 0x400)
    struct.pack_into('<I', raw, opt + 108, 16)
    struct.pack_into('<II', raw, opt + 112 + 3 * 8, 0x2000, 24)
    for offset, name, rva, disk, flags in [(0x188,b'.text',0x1000,0x400,0x60000020),
                                         (0x1b0,b'.pdata',0x2000,0x1400,0x40000040)]:
        raw[offset:offset+len(name)] = name
        struct.pack_into('<IIII', raw, offset + 8, 0x1000, rva, 0x1000, disk)
        struct.pack_into('<I', raw, offset + 36, flags)
    body = bytearray.fromhex('4883ec28 488d542420 b9f11de410 e800000000 84c0 7410 '
                             '8b442420 83f803 7507 b803000000 eb05 b801000000 4883c428 c3')
    struct.pack_into('<i', body, 15, 0x1100 - 0x1013)
    reader = bytes.fromhex('c70203000000 b801000000 c3')
    raw[0x400:0x400+len(body)] = body
    raw[0x500:0x500+len(reader)] = reader
    struct.pack_into('<III', raw, 0x1400, 0x1000, 0x1000+len(body), 0)
    struct.pack_into('<III', raw, 0x140c, 0x1100, 0x1100+len(reader), 0)
    return bytes(raw)

class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.dll = self.root/'unknown-preset-provider.dll'
        self.dll.write_bytes(fixture())

    def tearDown(self): self.temp.cleanup()

    def test_c_consumer_proof_real_resolver_not_gpu(self):
        before = self.dll.read_bytes()
        row = collect.preset_trial(self.dll, args.probe.resolve())
        p = row['preset']
        self.assertTrue(p['reader_resolved'])
        self.assertTrue(p['structural_reader'])
        self.assertFalse(p['observer_resolved'])
        self.assertEqual(p['reader_rva'], 0x1100)
        self.assertEqual(p['selection_returns'], [0x1013])
        self.assertFalse(p['vendor_code_executed'])
        self.assertFalse(p['runtime_tested'])
        self.assertTrue(row['deterministic_two_processes'])
        self.assertEqual(self.dll.read_bytes(), before)
        self.assertEqual(len(p['code_contexts']), 2)
        self.assertTrue(all(c['complete_function'] for c in p['code_contexts']))

    def test_unknown_version_does_not_veto_reader(self):
        raw = bytearray(self.dll.read_bytes())
        struct.pack_into('<I', raw, 0x88, 0xdeadbeef)
        self.dll.write_bytes(raw)
        self.assertTrue(collect.preset_trial(self.dll, args.probe.resolve())['preset']['reader_resolved'])

    def test_wrong_setting_unresolved_not_success(self):
        raw = bytearray(self.dll.read_bytes()); raw[0x40a] ^= 1; self.dll.write_bytes(raw)
        p = collect.preset_trial(self.dll, args.probe.resolve())['preset']
        self.assertFalse(p['reader_resolved'])
        self.assertEqual(p['literal_candidate_count'], 0)

    def test_invalid_abi_not_replaced_by_enum_guess(self):
        raw = bytearray(self.dll.read_bytes()); raw[0x501] = 1; self.dll.write_bytes(raw)
        p = collect.preset_trial(self.dll, args.probe.resolve())['preset']
        self.assertFalse(p['reader_resolved'])
        self.assertGreaterEqual(p['literal_candidate_count'], 1)
        self.assertEqual(len(p['code_contexts']), 2)

    def test_partial_graph_exports_reached_callee_without_claiming_completeness(self):
        raw = bytearray(self.dll.read_bytes())
        # Leave the argument setup and native call intact; corrupt its successor.
        raw[0x413:0x416] = bytes.fromhex('c5f877')
        self.dll.write_bytes(raw)
        p = collect.preset_trial(self.dll, args.probe.resolve())['preset']
        self.assertFalse(p['reader_resolved'])
        self.assertFalse(p['candidate_graphs_complete'])
        self.assertFalse(p['contexts_exhaustive'])
        self.assertFalse(p['contexts_limited'])
        self.assertTrue(any(x['stopped_paths'] > 0 for x in p['candidate_graphs']))
        self.assertEqual({x['start_rva'] for x in p['code_contexts']}, {0x1000, 0x1100})

    def test_stale_probe_rejected(self):
        with mock.patch.object(collect, 'preset_source_id', return_value='wrong'):
            with self.assertRaisesRegex(ValueError, 'stale preset probe'):
                collect.preset_trial(self.dll, args.probe.resolve())

    def test_invalid_file_is_not_a_pass(self):
        self.dll.write_bytes(b'not PE')
        with self.assertRaises(ValueError):
            collect.preset_trial(self.dll, args.probe.resolve())

    def test_existing_collector_cli_produces_evidence_only(self):
        out = self.root/'out'
        command = [sys.executable, str(ROOT/'tools/collect_provider_trials.py'),
                   '--preset-only', '--preset-probe', str(args.probe.resolve()),
                   '--provider', str(self.dll), '--out', str(out)]
        run = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        report = json.loads((out/'provider_trials.json').read_text())
        self.assertFalse(report['base_preparation_tested'])
        self.assertEqual(report['prepared_trials'], 0)
        self.assertEqual(report['preset_readers_resolved'], 1)
        self.assertFalse(report['runtime_gpu_tested'])
        with zipfile.ZipFile(out/'provider_trials_EVIDENCE_ONLY.zip') as z:
            self.assertIsNone(z.testzip())
            self.assertEqual(set(z.namelist()), {'provider_trials.json', 'SHA256SUMS.txt'})
        again = subprocess.run(command, capture_output=True)
        self.assertNotEqual(again.returncode, 0)

unittest.main(argv=[sys.argv[0]], verbosity=2)
