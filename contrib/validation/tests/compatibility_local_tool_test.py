#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import argparse
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile
from compatibility_corpus_test import c, row, fake_pe, MODERN, LEGACY
import validate_compatibility_local as local

PROBE = None

def fixture():
    interposer=row('sl.interposer.dll',MODERN);interposer['file_version']='2.14.1.0'
    common=row('sl.common.dll',['slGetPluginFunction']);common['file_version']='2.14.1.0'
    plugin=row('sl.dlss_g.dll',['slGetPluginFunction']);plugin['file_version']='2.14.1.0'
    result=[interposer,common,plugin]
    for item in result:
        item.update(sdk_version='2.14.1',kind='streamline',path='streamline/2.14.1/'+item['file_name'])
    return result

class LocalToolTests(unittest.TestCase):
    def test_production_contract_oracle(self):
        rows=fixture()
        value=local.contract_projection(rows,PROBE)
        self.assertEqual(value['interposer']['typed_abi'],2)
        self.assertEqual(value['plugin']['gateway'],'AVAILABLE')
        self.assertEqual(value['dynamic_mfg'],'RUNTIME_NEGOTIATION_REQUIRED')
        self.assertFalse(value['runtime_tested'])
        for item in rows:item['file_version']='3.99.0.0'
        value=local.contract_projection(rows,PROBE)
        self.assertEqual(value['interposer']['typed_abi'],2)
        self.assertEqual(value['plugin']['gateway'],'AVAILABLE')

    def test_sdk_label_does_not_authorize_abi(self):
        rows=fixture();rows[0]['file_version']=None
        self.assertEqual(local.contract_projection(rows,PROBE)['interposer']['typed_abi'],2)

    def test_versions_do_not_select_or_veto_contract(self):
        for exports, adapter in ((MODERN, 2), (LEGACY, 1)):
            for version in (None, '0.0.0.0', '1.5.4.0', '2.14.1.0', '99.999.0.0'):
                rows=fixture()
                rows[0]=row('sl.interposer.dll',exports)
                rows[0]['file_version']=version
                rows[2]['file_version']='65535.0.0.0'
                result=local.contract_projection(rows,PROBE)
                self.assertEqual(result['interposer']['typed_abi'],adapter)
                self.assertEqual(result['plugin']['gateway'],'AVAILABLE')

    def test_missing_plugin_preserves_independent_adapter(self):
        rows=fixture()
        value=local.contract_projection(rows[:2],PROBE)
        self.assertEqual(value['interposer']['typed_abi'],2)
        self.assertNotEqual(value['plugin']['gateway'],'AVAILABLE')
        for name in ('sl.common.dll','sl.interposer.dll'):
            value=local.contract_projection([r for r in rows if r['file_name']!=name],PROBE)
            self.assertEqual(value['interposer']['typed_abi'],0)

    def test_changed_sha_drops_old_decision(self):
        rows=fixture()
        raw={'libraries':rows,'streamline_bundles':[{'sdk_version':'2.14.1'}]}
        prior=copy.deepcopy(raw)
        prior['libraries'][0]['sha256']='0'*64
        first=local.projection(raw,PROBE,provenance='SYNTHETIC',source_sha='0'*64,previous=prior)
        second=local.projection(raw,PROBE,provenance='SYNTHETIC',source_sha='0'*64,previous=prior)
        self.assertEqual(c.canonical(first),c.canonical(second))
        self.assertEqual(first['libraries'][0]['identity_change'],'CHANGED_SHA256_RECOMPUTED_NO_PATH_CACHE')
        self.assertFalse(first['evidence']['current_runtime_tested'])
        self.assertFalse(first['evidence']['release_qualified'])

    def test_evidence_zip_contains_only_reports(self):
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp)/'out';out.mkdir()
            (out/'report.json').write_text('{}')
            target=local.seal(out)
            with zipfile.ZipFile(target) as archive:
                self.assertEqual(set(archive.namelist()),{'report.json','SHA256SUMS.txt'})
                self.assertIsNone(archive.testzip())
            (out/'nvngx_dlssg.dll').write_bytes(b'not a DLL')
            with self.assertRaises(ValueError):local.seal(out)

    def test_isolated_worker_on_synthetic_file(self):
        with tempfile.TemporaryDirectory(dir=PROBE.parent) as tmp:
            path=Path(tmp)/'nvngx_dlssg.dll'
            path.write_bytes(fake_pe())
            inspector=local.executable(PROBE.parent, 'inspect_provider')
            image_probe=local.executable(PROBE.parent, 'mapped_image_bounds_test')
            command=[sys.executable,str(Path(local.__file__)), '--worker','--file',str(path),
                     '--provider','--inspector',str(inspector),'--image-probe',str(image_probe)]
            a=local.json_process(command,timeout=30)
            b=local.json_process(command,timeout=30)
            self.assertEqual(c.canonical(a),c.canonical(b))
            self.assertEqual(a['static_qualification']['qualification'],'UNKNOWN')
            self.assertTrue(all(not r['offline_mfg_candidate'] for r in a['cpp_targets'].values()))
            path=Path(tmp)/'sl.dlss_g.dll';path.write_bytes(fake_pe())
            command=[sys.executable,str(Path(local.__file__)), '--worker','--file',str(path),
                     '--inspector',str(inspector),'--image-probe',str(image_probe)]
            result=local.json_process(command,timeout=30)
            self.assertTrue(result['plugin_scan_production_seam']['image_valid'])
            self.assertEqual(result['plugin_scan_production_seam']['planned_store_count'],0)
            self.assertFalse(result['plugin_scan_production_seam']['writes_executed'])

    def test_executable_resolution_and_number_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(FileNotFoundError):local.executable(Path(tmp),'missing')
        self.assertEqual(local.major({'file_version':'broken'}),0)
        self.assertEqual(local.major({'file_version':'2.999.0.0'}),2)
        with self.assertRaises(RuntimeError):
            local.json_process([str(PROBE),'--contract','not-a-major','1','1'],timeout=10)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--probe',required=True,type=Path)
    args,remaining=parser.parse_known_args();PROBE=args.probe
    unittest.main(argv=[sys.argv[0],*remaining])
