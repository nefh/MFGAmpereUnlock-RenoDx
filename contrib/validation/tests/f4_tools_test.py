#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import copy
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[3]
def load(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/'tools'/(name+'.py'))
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
q=load('qualify_f4');ab=load('evaluate_f4_ab')

class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
    def tearDown(self):self.tmp.cleanup()
    def project(self):
        for name in ('src/addons/mfgunlock','tools','contrib/validation'):(self.root/name).mkdir(parents=True,exist_ok=True)
        # Byte fixtures must not depend on the host text-mode newline policy.
        (self.root/'src/addons/mfgunlock/addon.cpp').write_bytes(b'int x;\n')
        return self.root
    def corpus(self,changed=False,tampered=False):
        manifest=q.source_manifest(self.root)['files']
        ids={k:v['sha256'] for k,v in manifest.items()}
        if changed:ids[next(iter(ids))]='a'*64
        payload={
          'summary.json':q.canonical({'status':'EVIDENCE_COLLECTED','errors':[]}),
          'stack/validation_result.json':q.canonical({'result':'PASS','full_corpus_current_run':True,'failures':[],'summary':{'dll_records':229}}),
          'stack/analyzer_source_sha256.json':q.canonical(ids),
          'mfg_gate/summary.json':q.canonical({'errors':[]})}
        payload['SHA256SUMS.txt']=''.join(f'{q.sha(v)}  {k}\n' for k,v in payload.items()).encode()
        if tampered:payload['summary.json']=b'{}'
        out=self.root/'corpus.zip'
        with zipfile.ZipFile(out,'w') as z:
            for k,v in payload.items():z.writestr(k,v)
        return out
    def test_native_requirements_match_registered_stack_variants(self):
        cmake=(ROOT/'contrib/validation/CMakeLists.txt').read_text()
        sizes=re.search(r'foreach\(kib IN ITEMS ([0-9 ]+)\)',cmake)
        self.assertIsNotNone(sizes)
        self.assertIn('add_test(NAME small_stack_${kib} COMMAND small_stack_test ${kib})',cmake)
        expected={'small_stack_'+x for x in sizes.group(1).split()}
        self.assertEqual({x for x in q.REQUIRED_NATIVE if x.startswith('small_stack')},expected)
        for name in q.REQUIRED_NATIVE-expected:
            self.assertIn('add_test(NAME '+name+' ',cmake)
    def test_binary_source_bytes_not_text_normalized(self):
        self.assertEqual(q.normalize_text(b'\xff\xd8\r\n\xfe'),b'\xff\xd8\r\n\xfe')
    def test_zero_tests_not_pass(self):self.assertEqual(q.junit(b'<testsuite/>',set())['status'],'FAIL')
    def test_skipped_not_pass(self):self.assertEqual(q.junit(b'<testsuite><testcase name="x"><skipped/></testcase></testsuite>',{'x'})['status'],'FAIL')
    def test_failure_not_pass(self):self.assertEqual(q.junit(b'<testsuite><testcase name="x"><failure/></testcase></testsuite>',{'x'})['status'],'FAIL')
    def test_duplicate_test_name(self):self.assertEqual(q.junit(b'<testsuite><testcase name="x"/><testcase name="x"/></testsuite>',{'x'})['status'],'FAIL')
    def test_missing_required(self):self.assertEqual(q.junit(b'<testsuite><testcase name="x"/></testsuite>',{'y'})['status'],'FAIL')
    def test_complete_test(self):self.assertEqual(q.junit(b'<testsuite><testcase name="x"/></testsuite>',{'x'})['status'],'PASS')
    def test_xml_entities(self):
        with self.assertRaises(ValueError):q.junit(b'<!DOCTYPE x><testsuite/>',set())
    def test_paths_reject_escape(self):
        for s in ('../x','/x','a\\b','C:/x'):
            with self.assertRaises(ValueError):q.safe_name(s)
    def test_manifest_determinism_and_mutation(self):
        self.project();a=q.source_manifest(self.root)
        self.assertEqual(a,q.source_manifest(self.root))
        (self.root/'src/addons/mfgunlock/addon.cpp').write_text('int y;\n')
        self.assertNotEqual(a['identity_lf_sha256'],q.source_manifest(self.root)['identity_lf_sha256'])
    def test_manifest_crlf_identity(self):
        self.project()
        rel='src/addons/mfgunlock/addon.cpp'
        source=self.root/rel
        self.assertEqual(source.read_bytes(),b'int x;\n')
        a=q.source_manifest(self.root)
        source.write_bytes(b'int x;\r\n')
        self.assertEqual(source.read_bytes(),b'int x;\r\n')
        b=q.source_manifest(self.root)
        self.assertEqual(a['files'][rel]['bytes'],7)
        self.assertEqual(b['files'][rel]['bytes'],8)
        self.assertNotEqual(a['files'][rel]['sha256'],b['files'][rel]['sha256'])
        self.assertEqual(a['files'][rel]['lf_sha256'],b['files'][rel]['lf_sha256'])
        self.assertEqual(a['identity_lf_sha256'],b['identity_lf_sha256']);self.assertNotEqual(a['files'],b['files'])
    def test_begin_no_overwrite(self):
        self.project();out=self.root/'dist/session';q.begin(self.root,out)
        with self.assertRaises(ValueError):q.begin(self.root,out)
    def test_corpus_hashes(self):
        self.project();self.assertEqual(q.inspect_corpus(self.corpus(),self.root)['status'],'PASS')
    def test_stale_corpus_rejected(self):
        self.project();self.assertEqual(q.inspect_corpus(self.corpus(changed=True),self.root)['status'],'FAIL_OR_STALE')
    def test_corrupt_corpus_rejected(self):
        self.project()
        with self.assertRaises(ValueError):q.inspect_corpus(self.corpus(tampered=True),self.root)
    def test_finish_no_binary_in_archive(self):
        self.project();out=self.root/'dist/session';q.begin(self.root,out)
        tests={}
        for name in ('gcc','ubsan','native'):
            required=q.REQUIRED_COMMON | (q.REQUIRED_NATIVE if name=='native' else {'ui_native_precedence_test'})
            if name=='gcc':required|={'compatibility_full_corpus_test'}
            p=out/(name+'.xml');p.write_text('<testsuite>'+''.join('<testcase name="'+x+'"/>' for x in required)+'</testsuite>');tests[name]=p
        binary=out/'test.addon64';binary.write_bytes(b'MZtest fixture, no real program')
        log=out/'build.log';log.write_text(q.sha(binary.read_bytes()))
        result=q.finish(self.root,out,tests,binary,log,self.corpus(),[])
        self.assertEqual(result['status'],'STATIC_BUILD_VALIDATED_RUNTIME_CANDIDATE')
        self.assertFalse(result['release_qualified']);self.assertEqual(result['gpu_executed'],'NOT_RUN')
        with zipfile.ZipFile(out/'F4_QUALIFICATION_EVIDENCE_ONLY.zip') as z:
            self.assertFalse(any(x.endswith(('.dll','.exe','.addon64')) for x in z.namelist()))
    def test_no_AB_is_not_run(self):
        self.assertEqual(ab.evaluate({'schema':'mfg_f4_ab_v1','runs':[]},self.root)['status'],'NOT_RUN')
    def measurements(self):
        trace=self.root/'trace.txt';trace.write_bytes(b'test-only profiler fixture; NOT runtime evidence')
        ident={'path':'trace.txt','sha256':q.sha(trace.read_bytes())}
        runs=[]
        for n,phase in enumerate('ABBA'):
            groups=[{'source_frame_id':i,'generated_count':3,'gpu_active_sum_ns':1000 if phase=='A' else 900,'source_interval_ns':10000000,'own_cpu_ns':200 if phase=='A' else 100} for i in range(40)]
            ctx={x:'synthetic' for x in ('game_exe_sha256','provider_sha256','streamline_identity','gpu','driver','api','output','settings_id','capture_method')}
            runs.append({'phase':phase,'pair':str(0 if n<2 else 1),'addon_sha256':('a' if phase=='A' else 'b')*64,
              'coverage':'ALL_DLSSG_GPU_OPERATIONS_PER_SOURCE_FRAME','context':ctx,'trace':ident,'warmup_excluded':True,
              'after_recreate':n>=2,'groups':groups,'delivered_rate_source':'OBSERVED_OUTPUT_CAPTURE','delivered_hz':400})
        return {'schema':'mfg_f4_ab_v1','candidate':'TEST_FIXTURE_NOT_CAPTURE','runs':runs,
                'quality_review':{'result':'PASS','method':'synthetic tests only','scenes':['synthetic'],'generated_and_rendered_frames_captured':True,'capture':ident}}
    def test_positive_AB_not_release(self):
        value=ab.evaluate(self.measurements(),self.root);self.assertEqual(value['status'],'ELIGIBLE_FOR_MANUAL_REVIEW');self.assertFalse(value['release_qualified']);self.assertEqual(value['runtime_default'],'OFF')
    def test_kernel_only_rejected_as_incomplete(self):
        doc=self.measurements();doc['runs'][0]['coverage']='ONE_KERNEL';self.assertEqual(ab.evaluate(doc,self.root)['status'],'INCOMPLETE')
    def test_recreate_required(self):
        doc=self.measurements()
        for r in doc['runs']:r['after_recreate']=False
        self.assertIn('post-recreate',' '.join(ab.evaluate(doc,self.root)['missing']))
    def test_duplicate_source(self):
        doc=self.measurements();doc['runs'][0]['groups'][1]['source_frame_id']=0
        with self.assertRaises(ValueError):ab.evaluate(doc,self.root)
    def test_nan_rejected(self):
        doc=self.measurements();doc['runs'][0]['groups'][0]['gpu_active_sum_ns']=float('nan')
        with self.assertRaises(ValueError):ab.evaluate(doc,self.root)
    def test_provider_changed_rejected(self):
        doc=self.measurements();doc['runs'][1]['context']['provider_sha256']='other'
        self.assertEqual(ab.evaluate(doc,self.root)['status'],'REJECTED')
    def test_same_binary_not_AB(self):
        doc=self.measurements()
        for r in doc['runs']:r['addon_sha256']='a'*64
        self.assertEqual(ab.evaluate(doc,self.root)['status'],'REJECTED')
    def test_quality_not_run(self):
        doc=self.measurements();doc['quality_review']['result']='NOT_RUN';self.assertEqual(ab.evaluate(doc,self.root)['status'],'INCOMPLETE')
    def test_quality_fail_rejects(self):
        doc=self.measurements();doc['quality_review']['result']='FAIL';self.assertEqual(ab.evaluate(doc,self.root)['status'],'REJECTED')
    def test_slow_candidate_rejects(self):
        doc=self.measurements()
        for r in doc['runs']:
            if r['phase']=='B':
                for g in r['groups']:g['gpu_active_sum_ns']=1200
        self.assertEqual(ab.evaluate(doc,self.root)['status'],'REJECTED')
    def test_trace_missing(self):
        doc=self.measurements();(self.root/'trace.txt').unlink();self.assertEqual(ab.evaluate(doc,self.root)['status'],'INCOMPLETE')
    def test_dynamic_mix_not_pooled(self):
        doc=self.measurements();doc['runs'][1]['groups'][0]['generated_count']=2
        self.assertEqual(ab.evaluate(doc,self.root)['status'],'INCOMPLETE')
    def test_cpu_benefit_needs_same_group_weights(self):
        doc=self.measurements();doc['benefit_metric']='own_cpu'
        doc['runs'][1]['groups'].pop()
        self.assertEqual(ab.evaluate(doc,self.root)['status'],'INCOMPLETE')
    def test_zero_cpu_baseline_not_qualified(self):
        doc=self.measurements();doc['benefit_metric']='own_cpu'
        for r in doc['runs']:
            for g in r['groups']:g['own_cpu_ns']=0
        self.assertEqual(ab.evaluate(doc,self.root)['status'],'INCOMPLETE')
    def test_delivered_regression_rejects(self):
        doc=self.measurements()
        for r in doc['runs']:
            if r['phase']=='B':r['delivered_hz']=200
        self.assertEqual(ab.evaluate(doc,self.root)['status'],'REJECTED')
    def test_declared_count_mismatch(self):
        with self.assertRaises(ValueError):q.junit(b'<testsuite tests="9"><testcase name="x"/></testsuite>',{'x'})
    def test_changed_session_source_rejected(self):
        self.project();out=self.root/'dist/session';q.begin(self.root,out)
        (self.root/'src/addons/mfgunlock/addon.cpp').write_text('int changed;')
        with self.assertRaisesRegex(ValueError,'sources changed'):
            q.finish(self.root,out,{},Path('missing'),Path('missing'),Path('missing'),[])
    def test_source_cannot_escape_symlink(self):
        self.project();outside=self.root/'outside';outside.write_text('int x;')
        alias=self.root/'src/linked.cpp'
        try:alias.symlink_to(outside)
        except (OSError,NotImplementedError):self.skipTest('symlinks unavailable')
        with self.assertRaises(ValueError):q.source_manifest(self.root)
    def test_no_hidden_tools(self):
        for path in (ROOT/'tools/qualify_f4.py',ROOT/'tools/evaluate_f4_ab.py'):
            text=path.read_text();self.assertNotIn('import subprocess',text);self.assertNotIn('ctypes',text)

if __name__=='__main__':unittest.main(verbosity=2)
