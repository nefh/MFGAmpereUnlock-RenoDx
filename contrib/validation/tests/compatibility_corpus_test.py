#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline foundation regression tests; no downloaded or native DLL execution."""
from pathlib import Path
import argparse
import copy
import json
import random
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
import compatibility_corpus as c

CORE = ['slInit', 'slShutdown', 'slSetTag', 'slEvaluateFeature']
MODERN = CORE + ['slGetFeatureFunction', 'slGetFeatureRequirements', 'slGetFeatureVersion']
LEGACY = CORE + ['slGetFeatureSettings', 'slSetFeatureConstants']


def fake_pe(names=(), *, fatbin=b'', text=b'\x90\xc3', forwarder=False):
    """Small complete PE32+ fixture with two nonoverlapping file-backed sections."""
    data = bytearray(0x2600)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', data, 0x84, 0x8664, 2, 0x12345678, 0, 0, 0xf0, 0x2022)
    opt = 0x98
    struct.pack_into('<H', data, opt, 0x20b)
    struct.pack_into('<Q', data, opt+24, 0x180000000)
    struct.pack_into('<II', data, opt+32, 0x1000, 0x200)
    struct.pack_into('<II', data, opt+56, 0x5000, 0x200)
    struct.pack_into('<I', data, opt+108, 16)
    sec = opt + 0xf0
    for i, (name, rva, raw, size, flags) in enumerate([
        (b'.text', 0x1000, 0x200, 0x400, 0x60000020), (b'.rdata', 0x2000, 0x600, 0x2000, 0x40000040)]):
        at = sec + 40*i
        data[at:at+len(name)] = name
        struct.pack_into('<IIII', data, at+8, size, rva, size, raw)
        struct.pack_into('<I', data, at+36, flags)
    data[0x200:0x200+len(text)] = text
    if names:
        struct.pack_into('<II', data, opt+112, 0x2000, 0x700)
        n = len(names)
        struct.pack_into('<IIHHIIIIIII', data, 0x600, 0, 0, 0, 0, 0x2300, 1, n, n, 0x2040, 0x20c0, 0x2140)
        data[0x900:0x906] = b'x.dll\0'
        at = 0x980
        for i, name in enumerate(names):
            struct.pack_into('<I', data, 0x640+4*i, 0x2340 if forwarder else 0x1000)
            struct.pack_into('<I', data, 0x6c0+4*i, 0x2000+at-0x600)
            struct.pack_into('<H', data, 0x740+2*i, i)
            s = name.encode('ascii')+b'\0'; data[at:at+len(s)] = s; at += len(s)
        data[0x940:0x94d] = b'other.symbol\0'
    if fatbin:
        if len(fatbin) > 0x1000:
            raise ValueError('fixture too large')
        data[0x1600:0x1600+len(fatbin)] = fatbin
    return bytes(data)


def envelope(body=b'.version 8.0\n.target sm_89\n.address_size 64\n.visible .entry sample() {}\0', header=64):
    padded = body + b'\0' * ((-len(body)) % 8)
    entry = bytearray(header + len(padded))
    struct.pack_into('<HHIQ', entry, 0, 1, 0x101, header, len(padded))
    struct.pack_into('<I', entry, 28, 89)
    struct.pack_into('<Q', entry, 40, 0x41)
    entry[header:] = padded
    return struct.pack('<IHHQ', 0xba55ed50, 1, 16, len(entry)) + entry


def row(name, exports):
    value = c.analyze_bytes(fake_pe(exports))
    value['file_name'] = name
    return value


class FoundationTests(unittest.TestCase):
    def test_valid_metadata_absent_version(self):
        a = c.analyze_bytes(fake_pe(MODERN))
        self.assertEqual(a['pe_status'], 'PASS'); self.assertIsNone(a['file_version'])
        self.assertEqual(a['family'], 'modern-export-surface')
        self.assertFalse(a['runtime_authorized'])
        self.assertEqual(a['capabilities']['streamline_call_abi']['state'], 'UNKNOWN')

    def test_deterministic(self):
        data = fake_pe(MODERN, fatbin=envelope())
        self.assertEqual(c.canonical(c.analyze_bytes(data, provider_requested=True)), c.canonical(c.analyze_bytes(bytes(bytearray(data)), provider_requested=True)))

    def test_malformed_and_truncated(self):
        good = fake_pe(MODERN)
        for n in (0, 1, 255, 256, 0x100, 0x200, 0x601, len(good)-1):
            self.assertEqual(c.analyze_bytes(good[:n])['pe_status'], 'INVALID', n)
        self.assertEqual(c.analyze_bytes(b'bad'*300)['pe_status'], 'INVALID')

    def test_invalid_directory_count(self):
        data = bytearray(fake_pe(MODERN)); struct.pack_into('<I', data, 0x98+108, 17)
        self.assertEqual(c.analyze_bytes(bytes(data))['pe_status'], 'INVALID')

    def test_invalid_export_ordinal(self):
        data = bytearray(fake_pe(MODERN)); struct.pack_into('<H', data, 0x740, 999)
        self.assertIn('ordinal', c.analyze_bytes(bytes(data))['structural_validation']['error'])

    def test_named_null_export(self):
        data = bytearray(fake_pe(MODERN)); struct.pack_into('<I', data, 0x640, 0)
        self.assertIn('null_export', c.analyze_bytes(bytes(data))['structural_validation']['error'])

    def test_duplicate_exports(self):
        self.assertEqual(c.analyze_bytes(fake_pe(['slInit','slInit']))['pe_status'], 'INVALID')

    def test_forwarder_not_typed_support(self):
        data = c.analyze_bytes(fake_pe(MODERN, forwarder=True))
        self.assertEqual(data['pe_status'], 'PASS')
        self.assertEqual(data['family'], 'unknown-safe')
        self.assertTrue(all(e['kind']=='forwarder' for e in data['exports']))

    def test_invalid_export_name(self):
        data = bytearray(fake_pe(MODERN)); struct.pack_into('<I', data, 0x6c0, 0xfffffff0)
        self.assertEqual(c.analyze_bytes(bytes(data))['pe_status'], 'INVALID')

    def test_zero_many_and_unique_anchors(self):
        body = b'\x91\x92\x93\x94'
        t = [{'bytes':4, 'sha256':c.sha(body), '_body':body}]
        self.assertEqual(c.match_code(c.PEView(fake_pe()), t)['state'], 'UNAVAILABLE')
        self.assertEqual(c.match_code(c.PEView(fake_pe(text=body)), t)['state'], 'STRUCTURAL_MATCH')
        self.assertEqual(c.match_code(c.PEView(fake_pe(text=body+body)), t)['state'], 'AMBIGUOUS')
        self.assertFalse(c.match_code(c.PEView(fake_pe(text=body)), t)['runtime_authorized'])

    def test_payload_multiplicity(self):
        a,b = {'bytes':1,'sha256':'a'}, {'bytes':2,'sha256':'b'}
        self.assertEqual(c.identity_multiset([a,b,a]), c.identity_multiset([b,a,a]))
        self.assertNotEqual(c.identity_multiset([a,b,a]), c.identity_multiset([a,b]))
        self.assertIsNone(c.identity_multiset([]))

    def test_extended_fatbin_header(self):
        data = c.analyze_bytes(fake_pe(fatbin=envelope(header=344)), provider_requested=True)
        self.assertEqual(data['fatbin_count'],1)
        self.assertEqual(data['readable_ptx_entries'],1)
        self.assertEqual(data['fatbin_errors'],[])

    def test_opaque_body_preserved(self):
        data = c.analyze_bytes(fake_pe(fatbin=envelope(body=b'opaque binary contents',header=344)), provider_requested=True)
        self.assertEqual(data['opaque_kind1_entries'],1)
        self.assertEqual(data['fatbin_count'],1)
        self.assertEqual(data['readable_ptx_entries'],0)
        self.assertTrue(data['provider_payload_identity'])
        self.assertEqual(data['capabilities']['retarget_runtime']['state'],'UNKNOWN')

    def test_invalid_fatbin_header_closed(self):
        bad = bytearray(envelope()); struct.pack_into('<I',bad,20,8192)
        data = c.analyze_bytes(fake_pe(fatbin=bad),provider_requested=True)
        self.assertIsNone(data['provider_payload_identity'])
        self.assertTrue(data['fatbin_errors'])
        self.assertFalse(data['runtime_authorized'])

    def test_relocation_independent_payload(self):
        data = bytearray(fake_pe(fatbin=envelope()))
        one = c.analyze_bytes(bytes(data),provider_requested=True)
        struct.pack_into('<I',data,0x98+0xf0+40+12,0x3000)
        two = c.analyze_bytes(bytes(data),provider_requested=True)
        self.assertEqual(one['provider_payload_identity'],two['provider_payload_identity'])
        self.assertNotEqual(one['sha256'],two['sha256'])

    def test_modified_payload_changes_identity(self):
        data = bytearray(fake_pe(fatbin=envelope(body=b'opaque')))
        a=c.analyze_bytes(bytes(data),provider_requested=True)
        data[0x1600+16+64] ^= 1
        b=c.analyze_bytes(bytes(data),provider_requested=True)
        self.assertNotEqual(a['provider_payload_identity'],b['provider_payload_identity'])

    def test_missing_common_and_interposer(self):
        i=row('sl.interposer.dll',MODERN); p=row('sl.dlss_g.dll',['slGetPluginFunction'])
        common=row('sl.common.dll',['slGetPluginFunction'])
        self.assertEqual(c.classify_bundle([i,p])['assembly_family'],'unknown-safe')
        self.assertEqual(c.classify_bundle([common,p])['assembly_family'],'unknown-safe')
        self.assertEqual(c.classify_bundle([i,common])['assembly_family'],'modern-export-surface')
        self.assertFalse(c.classify_bundle([i,common])['dlssg_plugin_present'])

    def test_duplicate_interposer(self):
        i=row('sl.interposer.dll',MODERN); co=row('sl.common.dll',['slGetPluginFunction'])
        self.assertEqual(c.classify_bundle([i,i,co])['assembly_family'],'unknown-safe')

    def test_independent_typed_operation_surfaces(self):
        self.assertEqual(row('sl.interposer.dll',LEGACY)['family'],'legacy-export-surface')
        self.assertEqual(row('sl.interposer.dll',MODERN+LEGACY[-2:])['family'],'modern-export-surface')
        self.assertEqual(row('sl.interposer.dll',CORE+['slGetFeatureFunction'])['family'],'modern-export-surface')
        self.assertEqual(row('sl.interposer.dll',['slGetFeatureFunction'])['family'],'modern-export-surface')

    def test_unknown_provider_no_authorization(self):
        data = c.analyze_bytes(fake_pe(),provider_requested=True)
        self.assertFalse(data['provider_identity_observed'])
        self.assertEqual(data['capabilities']['provider_identity']['state'],'UNAVAILABLE')
        self.assertEqual(data['capabilities']['retarget_runtime']['state'],'UNKNOWN')
        self.assertFalse(data['runtime_authorized'])

    def test_historical_sm75_profiles_are_validation_only(self):
        observations = c.transform_profile_observations([])
        self.assertEqual(len(observations['kPackedHalfProfiles']), 5)
        self.assertEqual(len(observations['kTuringMmaProfiles']), 27)

    def test_temporal_inventory_uses_local_role_shape(self):
        text = (b'.version 8.7\n.target sm_999\n.visible .entry main_kernel(\n'
                b'.param .align 16 .b8 actual_name[144]) {\n.reg .f32 %v<2>;\n'
                b'renamed_join:\n' + b'mul.ftz.f32 %v0, %v1, 0f3F000000;\n' * 104 + b'ret;\n}\n'
                b'// mul.ftz.f32 %v0, %v1, 0f3F000000;\n'
                b'.entry unrelated() {mul.ftz.f32 %v0, %v1, 0f3F000000;}\n')
        entry = {'_text': text, 'architecture': 999, 'declared_raw_bytes': len(text),
                 'normalized_sha256': c.sha(text)}
        result = c.temporal_observations([{'rva': 0x2000, 'entries': [entry]}])
        role = result['profiles'][0]
        self.assertEqual(role['candidates'][0]['midpoint_count'], 104)
        self.assertEqual(role['candidates'][0]['parameter_symbol'], 'actual_name')
        self.assertTrue(role['candidates'][0]['local_shape_observed'])
        self.assertEqual(role['status']['state'], 'OBSERVED')
        self.assertEqual(result['transform_execution'], 'NOT_RUN')
        self.assertFalse(result['runtime_authorized'])
        entry['_text'] = text.replace(b'[144]', b'[140]')
        result = c.temporal_observations([{'rva': 0x2000, 'entries': [entry]}])
        self.assertFalse(result['profiles'][0]['candidates'][0]['local_shape_observed'])

    def test_random_bounded_header_mutations(self):
        rng=random.Random(20260929); base=fake_pe(MODERN,fatbin=envelope())
        for _ in range(256):
            data=bytearray(base)
            for _ in range(4):
                at=rng.randrange(0x200); data[at]^=rng.randrange(1,256)
            result=c.analyze_bytes(bytes(data),provider_requested=True)
            self.assertIn(result['pe_status'],('PASS','PARTIAL','INVALID'))
            self.assertFalse(result['runtime_authorized'])

    def test_empty_corpus_not_success(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            (root/'compatibility_library_manifest.json').write_text('[]')
            (root/'streamline_manifest.json').write_text('[]')
            with self.assertRaisesRegex(ValueError,'empty_or_invalid'):
                c.collect(root,root,root)


def validate_real_corpus(args):
    report=c.collect(args.dlssg_root,args.streamline_root,args.streamline_meta)
    libs=report['libraries']; bundles=report['streamline_bundles']
    def check(test,message):
        if not test: raise AssertionError(message)
    check(report['summary']['pe_pass']==len(libs),'all DLLs must parse')
    check(report['summary']['dlssg_corpus_records']==35,'reference corpus provider count changed')
    check(report['summary']['streamline_records']==194,'reference corpus SDK count changed')
    check(len(bundles)==15,'reference corpus SDK versions changed')
    check(all(not x['runtime_authorized'] for x in libs),'runtime authorization leaked')
    check(all(not x.get('fatbin_errors') for x in libs),'invalid envelope in reference corpus')
    comparisons=0
    if args.probe:
        for row_ in libs:
            names=sorted(c.own_exports(row_['exports']))
            observed=subprocess.run([str(args.probe),'--surface',*names],capture_output=True,text=True,check=True,timeout=10).stdout.strip()
            check(observed==c.export_surface(row_['exports']),'C++/Python export family mismatch:'+row_['path'])
            comparisons+=1
    for sdk in bundles:
        rows=[r for r in libs if r['sdk_version']==sdk['sdk_version']]
        no_common=c.classify_bundle([r for r in rows if r['file_name'].lower()!='sl.common.dll'])
        no_interposer=c.classify_bundle([r for r in rows if r['file_name'].lower()!='sl.interposer.dll'])
        check(no_common['assembly_family']=='unknown-safe','missing common fallback')
        check(no_interposer['assembly_family']=='unknown-safe','missing interposer fallback')
    if args.compare_matrix:
        check(c.canonical(report)==args.compare_matrix.read_bytes(),'matrix deterministic replay mismatch')
    check(report['reference_sha256'] is not None,'reference absent')
    by_version={r['file_version']:r for r in libs if r['kind']=='dlss_g'}
    x,y=by_version['310.9.0.0'],by_version['310.9.1.0']
    check(x['provider_payload_identity']==y['provider_payload_identity'],'reference payload relationship changed')
    check(x['recognized_signatures']['preset']['state']!='STRUCTURAL_MATCH','preset test no longer negative')
    check(y['recognized_signatures']['preset']['state']=='STRUCTURAL_MATCH','known preset anchor regression')
    results={'result':'PASS','dll_records':len(libs),'standalone_providers':35,'sdk_bundles':len(bundles),
             'cpp_python_family_comparisons':comparisons,'missing_component_tests':2*len(bundles),
             'matrix_deterministic_replay':bool(args.compare_matrix),'runtime_tested':False}
    if args.out: args.out.write_bytes(c.canonical(results))
    print(json.dumps(results,indent=2))


if __name__=='__main__':
    ap=argparse.ArgumentParser()
    ap.add_argument('--dlssg-root',type=Path);ap.add_argument('--streamline-root',type=Path)
    ap.add_argument('--streamline-meta',type=Path);ap.add_argument('--probe',type=Path)
    ap.add_argument('--compare-matrix',type=Path);ap.add_argument('--out',type=Path)
    args,rest=ap.parse_known_args()
    if args.dlssg_root:
        if rest or not args.streamline_root or not args.streamline_meta: ap.error('incomplete corpus arguments')
        validate_real_corpus(args)
    else:
        unittest.main(argv=[sys.argv[0],*rest])
