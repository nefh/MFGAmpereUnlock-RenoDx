#!/usr/bin/env python3
from pathlib import Path
import hashlib
import importlib.util
import json
import tempfile
import unittest
import zipfile
ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('collector', ROOT/'tools/collect_f3_runtime.py')
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
class Tests(unittest.TestCase):
    def test_native_return_is_not_gpu(self):
        rows=c.parse_events('INFO F3Vulkan api=Vulkan generation=2 create_ok=1 evaluate_ok=60 r0_known=1 r0_image=12\n')
        summary=c.classify(rows)
        self.assertTrue(summary['native_evaluate_success_observed'])
        self.assertFalse(summary['runtime_qualified'])
        self.assertEqual(summary['scanout'],'UNKNOWN')
        self.assertEqual(summary['known_nonnull_images'],1)
    def test_no_vulkan_from_loader_name(self):
        self.assertEqual(c.parse_events('Loaded vulkan-1.dll'),[])
        self.assertEqual(c.parse_events('F3Vulkan api=D3D12 evaluate_ok=60'),[])
    def test_malformed_counts_not_success(self):
        s=c.classify([{'evaluate_ok':'garbage','create_ok':'-1','generation':'9'*40}])
        self.assertFalse(s['native_evaluate_success_observed'])
        self.assertFalse(s['native_create_success_observed'])
    def test_bounded_events(self):
        rows=c.parse_events(('F3Vulkan api=Vulkan generation=1\n')*1000)
        self.assertEqual(len(rows),c.MAX_EVENTS)
    def test_zip_allowlist_hashes(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);log=root/'ReShade.log';log.write_text('F3Vulkan api=Vulkan evaluate_ok=9')
            addon=root/'core.addon64';addon.write_bytes(b'MZ-never-included')
            archive=c.collect([log],root/'out','SDR', [addon])
            with zipfile.ZipFile(archive) as z:
                self.assertEqual(set(z.namelist()),{'summary.json','SHA256SUMS.txt','logs/log_00.log'})
                for row in z.read('SHA256SUMS.txt').decode().splitlines():
                    digest,name=row.split('  ',1)
                    self.assertEqual(digest,hashlib.sha256(z.read(name)).hexdigest())
                self.assertFalse(json.loads(z.read('summary.json'))['vendor_dlls_included'])
    def test_no_overwrite(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);log=root/'a.log';log.write_text('')
            with self.assertRaises(ValueError): c.collect([log],root,'test')
    def test_reject_vendor_binary_disguised(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);log=root/'fake.log';log.write_bytes(b'MZ-binary')
            with self.assertRaises(ValueError): c.collect([log],root/'out','test')
    def test_utf16_no_event(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);log=root/'a.txt';log.write_text('build only',encoding='utf-16')
            archive=c.collect([log],root/'out','test')
            with zipfile.ZipFile(archive) as z:
                self.assertEqual(json.loads(z.read('summary.json'))['status'],'COLLECTED_NO_VULKAN_EVENT')
    def test_limit(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);log=root/'a.log';log.write_bytes(b'x'*20)
            old=c.MAX_LOG_BYTES;c.MAX_LOG_BYTES=10
            try:
                with self.assertRaises(ValueError): c.collect([log],root/'out','test')
            finally: c.MAX_LOG_BYTES=old
if __name__=='__main__':unittest.main(verbosity=2)
