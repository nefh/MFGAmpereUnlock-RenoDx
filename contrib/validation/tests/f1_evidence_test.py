#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import contextlib
import io
import json
from pathlib import Path
import random
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'tools'))
import collect_f1_evidence as f
import validate_compatibility_local as v
from compatibility_corpus_test import fake_pe, envelope


def cuda_elf():
    x = bytearray(256)
    x[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HHI', x, 16, 2, 190, 1)
    struct.pack_into('<Q', x, 40, 64)
    struct.pack_into('<I6H', x, 48, 89, 64, 0, 0, 64, 2, 1)
    names = b'\0.shstrtab\0'
    struct.pack_into('<IIQQQQIIQQ', x, 128, 1, 3, 0, 0, 192, len(names), 0, 0, 1, 0)
    x[192:192+len(names)] = names
    return bytes(x[:192+len(names)])


class F1EvidenceTests(unittest.TestCase):
    def test_interval_operations(self):
        self.assertEqual(f.union([(2,4),(4,9),(3,5),(11,12)]), [(2,9),(11,12)])
        self.assertEqual(f.subtract(0,14,[(2,9),(11,12)]),[(0,2),(9,11),(12,14)])
        for x in [[(-1,1)],[(4,2)]]:
            with self.assertRaises(ValueError): f.union(x)

    def test_interval_random_exact_coverage(self):
        rng = random.Random(42)
        for _ in range(100):
            intervals = [sorted(rng.sample(range(80),2)) for _ in range(10)]
            actual = {i for a,b in f.subtract(7,73,intervals) for i in range(a,b)}
            expected = set(range(7,73)) - {i for a,b in intervals for i in range(a,b)}
            self.assertEqual(actual,expected)

    def test_bounded_elf_metadata(self):
        raw = cuda_elf()
        extent, metadata = f.elf_extent(b'XXXX'+raw, 4)
        self.assertEqual(extent, len(raw)); self.assertEqual(metadata['target_sm'],89)
        for cut in (0,4,63,80,len(raw)-1):
            with self.assertRaises(ValueError): f.elf_extent(raw[:cut],0)

    def test_elf_bad_machine_tables(self):
        for at, fmt, val in ((18,'H',62),(58,'H',0),(60,'H',0xffff),(40,'Q',2**63),(62,'H',3)):
            raw=bytearray(cuda_elf());struct.pack_into('<'+fmt,raw,at,val)
            with self.assertRaises(ValueError): f.elf_extent(raw,0)

    def test_host_capture_excludes_fatbins_and_elf_and_keeps_pdata(self):
        raw=bytearray(fake_pe(fatbin=envelope()))
        elf=cuda_elf();raw[0x1800:0x1800+len(elf)]=elf
        struct.pack_into('<II',raw,0x98+112+3*8,0x2700,12)
        with tempfile.TemporaryDirectory() as td:
            dest=Path(td)/'capture'; f.host_capture(bytes(raw),dest)
            report=json.loads((dest/'host.json').read_text())
            self.assertTrue(report['exception_table_fully_captured'])
            self.assertEqual(len(report['cuda_elfs']),1)
            self.assertFalse(report['full_dlls_included'])
            emitted=set()
            for seg in report['segments']:
                body=(dest/seg['blob']).read_bytes()
                self.assertEqual(f.sha(body),seg['sha256'])
                self.assertEqual(body,raw[seg['raw_offset']:seg['raw_offset']+seg['bytes']])
                emitted.update(range(seg['raw_offset'],seg['raw_offset']+seg['bytes']))
            excluded={i for a,b in report['omitted_file_intervals'] for i in range(a,b)}
            self.assertFalse(emitted & excluded)
            self.assertTrue(set(range(0xd00,0xd0c)) <= emitted)

    def test_host_capture_budget_before_writes(self):
        with tempfile.TemporaryDirectory() as td:
            d=Path(td)/'capture'
            with self.assertRaises(ValueError): f.host_capture(fake_pe(fatbin=envelope()), d, budget=20)
            self.assertFalse(d.exists())

    def test_no_payload_means_no_whole_dll_export(self):
        with tempfile.TemporaryDirectory() as td:
            with self.assertRaises(ValueError): f.host_capture(fake_pe(),Path(td)/'capture')

    def test_corrupt_fatbin_not_exported_as_host(self):
        raw=bytearray(fake_pe(fatbin=envelope()));raw[0x1604]=9
        with tempfile.TemporaryDirectory() as td:
            with self.assertRaises(ValueError): f.host_capture(raw,Path(td)/'capture')

    def test_missing_exception_directory_is_not_coverage_success(self):
        raw=bytearray(fake_pe(fatbin=envelope()))
        struct.pack_into('<II',raw,0x98+112+3*8,0x9000,100)
        with tempfile.TemporaryDirectory() as td:
            with self.assertRaises(ValueError): f.host_capture(raw,Path(td)/'capture')

    def test_seal_never_packages_dll_or_private_mapping(self):
        with tempfile.TemporaryDirectory() as td:
            out=Path(td)
            (out/'private.dll').write_bytes(b'MZ'); (out/'x.image').write_bytes(b'MZ')
            (out/'sneaky.bin').write_bytes(b'MZ'); (out/'summary.json').write_text('{}')
            work=out/'.work-test';work.mkdir();(work/'x.json').write_text('{}')
            with zipfile.ZipFile(f.seal(out)) as z:
                self.assertEqual(set(z.namelist()),{'summary.json','SHA256SUMS.txt'})
                for line in z.read('SHA256SUMS.txt').decode().splitlines():
                    h,name=line.split('  ',1); self.assertEqual(f.sha(z.read(name)),h)

    def test_seal_refuses_symlink_to_unrelated_file(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); out=root/'out'; out.mkdir()
            secret=root/'unrelated.json'; secret.write_text('{}')
            try:
                (out/'leak.json').symlink_to(secret)
            except OSError:
                self.skipTest('symlinks unavailable')
            with self.assertRaisesRegex(ValueError,'symlink'):
                f.seal(out)

    def test_missing_probe_seals_error_not_pass(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            args=SimpleNamespace(out=root/'out', validation_bin=root,corpus_root=None,
                                 provider=[],require_provider=[],inspect_provider=[])
            with contextlib.redirect_stdout(io.StringIO()): code=f.collect(args)
            self.assertEqual(code,2)
            result=json.loads((args.out/'summary.json').read_text())
            self.assertEqual(result['status'],'COLLECTION_ERROR');self.assertFalse(result['f1_complete'])
            self.assertTrue((args.out/'MFG_F1_EVIDENCE_ONLY.zip').exists())

    def test_stale_preparation_probe_is_rejected(self):
        value={'compiled_source_id':'stale'}
        cp=SimpleNamespace(returncode=0,stdout=json.dumps(value).encode(),stderr=b'')
        with mock.patch.object(f.subprocess,'run',return_value=cp):
            with self.assertRaisesRegex(ValueError,'stale'):
                f.prepared_trials(fake_pe(),Path('probe'),Path('unused'))

    def test_old_projection_cannot_deny_unknown_provider(self):
        row=f.corpus.analyze_bytes(fake_pe(fatbin=envelope()),provider_requested=True)
        row.update(path='unknown.dll',file_name='nvngx_dlssg.dll',kind='dlss_g',sdk_version=None)
        raw={'libraries':[row], 'streamline_bundles':[]}
        for known in (False,True):
            row['existing_profile_exact_file_match']=known
            result=v.projection(raw,Path('not-called'),provenance='TEST',source_sha='0'*64)
            p=result['libraries'][0]['runtime_projection']
            self.assertEqual(p['retarget_runtime'],'NOT_RUN_PRODUCTION_PATCHER')
            self.assertEqual(p['host_mfg_gates'],'NOT_RUN_PRODUCTION_RESOLVER')

    def test_collector_has_no_loader_or_wsl_calls(self):
        code=(ROOT/'tools/collect_f1_evidence.py').read_text()
        for forbidden in ('ctypes.', 'WinDLL(', 'shell=True', 'wsl.exe', 'LoadLibrary('):
            self.assertNotIn(forbidden,code)

if __name__=='__main__': unittest.main(verbosity=2)
