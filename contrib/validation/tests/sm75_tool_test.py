#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import types
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tools'))
import validate_sm75_corpus as tool
import compatibility_corpus_test as fixtures

PROBE=None

class Tools(unittest.TestCase):
    def test_elf(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'test.cubin';data=bytearray(64);data[:6]=b'\x7fELF\x02\x01'
            struct.pack_into('<H',data,18,190);struct.pack_into('<I',data,48,75)
            p.write_bytes(data);self.assertTrue(tool.elf_target(p,75));self.assertFalse(tool.elf_target(p,86))
            p.write_bytes(data[:32]);self.assertFalse(tool.elf_target(p,75))

    def test_process_return(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'log'
            self.assertEqual(tool.process([sys.executable,'-c','print("visible result")'],p),0)
            self.assertIn('visible result',p.read_text())
            self.assertEqual(tool.process([sys.executable,'-c','raise SystemExit(7)'],p),7)

    def test_process_timeout(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(tool.process([sys.executable,'-c','import time;time.sleep(10)'],Path(d)/'log',timeout=1),124)

    def test_manifest(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);folder=root/'dlss_g';folder.mkdir();dll=folder/'nvngx_dlssg.dll';dll.write_bytes(b'fixture')
            manifest=folder/'compatibility_library_manifest.json'
            entry={'relative_path':'dlss_g/nvngx_dlssg.dll','bytes':7,'sha256':tool.sha(dll)}
            manifest.write_text(json.dumps([entry]));args=types.SimpleNamespace(provider=None,corpus_root=root)
            self.assertEqual(tool.inputs(args),[dll])
            for entries in ([],[entry,entry],[dict(entry,sha256='0'*64)],[dict(entry,relative_path='../escape.dll')]):
                manifest.write_text(json.dumps(entries))
                with self.assertRaises((ValueError,OSError)):tool.inputs(args)

    def test_address_normalization(self):
        a={'prepared':True,'ptx_files':['a.ptx'],'changed_bytes':110,
           'address_cells':2,'changed_non_address_bytes':98,'external_storage_bytes':12000}
        b=dict(a,ptx_files=[],changed_bytes=111)
        self.assertEqual(tool.comparable(a),tool.comparable(b))
        for key,value in (('changed_non_address_bytes',97),('address_cells',3),
                          ('external_storage_bytes',12008),('prepared',False)):
            self.assertNotEqual(tool.comparable(a),tool.comparable(dict(b,**{key:value})))
        a=dict(a,address_cells=0);b=dict(a,changed_bytes=111)
        self.assertNotEqual(tool.comparable(a),tool.comparable(b))

    def test_synthetic_worker_and_no_dll_archive(self):
        self.assertIsNotNone(PROBE)
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);dll=root/'synthetic.dll';dll.write_bytes(fixtures.fake_pe(["NotAProvider"]))
            out=root/'out'
            run=subprocess.run([sys.executable,str(ROOT/'tools/validate_sm75_corpus.py'),
                '--probe',str(PROBE),'--provider',str(dll),'--out',str(out)],capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr+(out/'results.json').read_text())
            result=json.loads((out/'results.json').read_text())[0]
            self.assertFalse(result['targets']['Turing']['prepared'])
            self.assertFalse(result['targets']['Turing']['endpoint_selected'])
            self.assertTrue(result['targets']['Turing']['deterministic_two_processes'])
            self.assertEqual(result['targets']['Turing']['compiled_source_id'],tool.source_id())
            with zipfile.ZipFile(out/'SM75_T2_VALIDATION_NO_DLL.zip') as z:
                self.assertIsNone(z.testzip())
                self.assertFalse(any(n.endswith(('.dll','.ptx','.cubin','.exe')) for n in z.namelist()))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--probe',type=Path,required=True)
    args,rest=p.parse_known_args();PROBE=args.probe
    unittest.main(argv=[sys.argv[0],*rest])
