#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Production host gate, collector integrity and integration contract regression."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'tools'))
import collect_mfg_gate_evidence as c

args, unknown = argparse.ArgumentParser().parse_known_args()
p = argparse.ArgumentParser()
p.add_argument('--probe', type=Path, required=True)
p.add_argument('--fixture', type=Path, required=True)
args = p.parse_args()

class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.dll = self.root/'unknown-provider.dll'
        subprocess.run([str(args.fixture),'--fixture',str(self.dll)],check=True)

    def tearDown(self):
        self.temp.cleanup()

    def run_collect(self, dll=None, required=None):
        path = dll or self.dll
        out = self.root/'out'
        with contextlib.redirect_stdout(io.StringIO()):
            rc = c.collect(args.probe.resolve(), [path], required or set(), out)
        return rc, out

    def test_unknown_identity_three_targets_real_production_path(self):
        before = self.dll.read_bytes()
        rc, out = self.run_collect(required={self.dll})
        self.assertEqual(rc, 0)
        records = json.loads((out/'compatibility_matrix.json').read_text())
        self.assertEqual(len(records),3)
        self.assertTrue(all(r['qualification']=='STRUCTURAL_VERIFIED' and r['ceiling']==3 and r['applied_copy'] for r in records))
        self.assertEqual(self.dll.read_bytes(), before)
        with zipfile.ZipFile(out/'MFG_GATE_STRUCTURAL_EVIDENCE_ONLY.zip') as z:
            self.assertIsNone(z.testzip())
            self.assertFalse(any(n.endswith(('.dll','.image','.exe','.bin')) for n in z.namelist()))
            for line in z.read('SHA256SUMS.txt').decode().splitlines():
                h, name = line.split('  ',1)
                self.assertEqual(c.digest(z.read(name)),h)

    def test_stale_probe_is_error_not_corpus_pass(self):
        with mock.patch.object(c,'compiled_id',return_value='not-current'):
            rc,out=self.run_collect()
        self.assertEqual(rc,2)
        self.assertEqual(json.loads((out/'summary.json').read_text())['target_records'],0)

    def test_unresolved_required_regression_fails_without_whitelist(self):
        raw=bytearray(self.dll.read_bytes()); raw[0x5100]=0
        self.dll.write_bytes(raw)
        rc,out=self.run_collect(required={self.dll})
        self.assertEqual(rc,2)
        m=json.loads((out/'compatibility_matrix.json').read_text())
        self.assertEqual(len(m),3)
        self.assertTrue(all(r['qualification']=='UNRESOLVED' and not r['applied_copy'] for r in m))

    def test_unresolved_not_mislabelled_mfg_success(self):
        raw=bytearray(self.dll.read_bytes());raw[0x5100]=0;self.dll.write_bytes(raw)
        rc,out=self.run_collect()
        self.assertEqual(rc,0)
        summary=json.loads((out/'summary.json').read_text())
        self.assertEqual(summary['structurally_qualified'],0)
        self.assertFalse(summary['runtime_tested'])

    def test_non_dll_input_cannot_pass(self):
        self.dll.write_bytes(b'not a PE')
        rc,out=self.run_collect()
        self.assertEqual(rc,2)
        self.assertEqual(json.loads((out/'summary.json').read_text())['target_records'],0)

    def test_output_is_not_clobbered(self):
        (self.root/'out').mkdir()
        with self.assertRaises(FileExistsError):
            self.run_collect()

    def test_package_ignores_transient_vendor_image(self):
        out=self.root/'package';out.mkdir()
        (out/'test.dll').write_bytes(b'MZ')
        (out/'.work-mapped').mkdir()
        (out/'.work-mapped/private.json').write_text('not evidence')
        (out/'summary.json').write_text('{}')
        with zipfile.ZipFile(c.package(out)) as z:
            self.assertEqual(set(z.namelist()),{'summary.json','SHA256SUMS.txt'})

    def test_header_truncation_and_budget(self):
        raw=self.dll.read_bytes()
        for n in range(0,512,7):
            with self.assertRaises(ValueError): c.map_pe(raw[:n])
        with mock.patch.object(c,'MAX_FILE',100):
            with self.assertRaises(ValueError): c.map_pe(raw)
        with mock.patch.object(c,'MAX_IMAGE',100):
            with self.assertRaises(ValueError): c.map_pe(raw)

    def test_runtime_does_not_use_oracle_for_gate_authorization(self):
        code=(ROOT/'src/addons/mfgunlock/mfg_gate.hpp').read_text()
        for forbidden in ('kMfgGates', 'kProfiles', 'kDlssg31091', 'profiles::Find', '310.7', '0x16864', '0x35e50'):
            self.assertNotIn(forbidden,code)
        addon=(ROOT/'src/addons/mfgunlock/addon.cpp').read_text()
        gate=addon[addon.index('void PatchArchGatesInModule('):addon.index('void TryPatchDlssgArchGate(')]
        self.assertIn('FindMfgGatePlan',gate)
        self.assertIn('ApplyMfgGatePlan',gate)
        self.assertNotIn('QualifiedNativeInventory',gate)
        self.assertNotIn('g_gate_rejected_modules',addon)
        self.assertIn('g_provider_mfg_ceiling',addon)
        self.assertNotIn('kDlssg31091.options_version',addon)
        self.assertNotIn('kDlssg31091.state_version',addon)

    def test_unknown_target_rejected_by_probe(self):
        cp=subprocess.run([str(args.probe),str(self.dll),'SM999'],capture_output=True)
        self.assertNotEqual(cp.returncode,0)

    def test_cli_has_no_vendor_execution_calls(self):
        code=(ROOT/'tools/collect_mfg_gate_evidence.py').read_text()
        self.assertNotIn('ctypes.',code)
        self.assertNotIn('WinDLL(',code)
        self.assertNotIn('shell=True',code)

unittest.main(argv=[sys.argv[0]],verbosity=2)
