#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""End-to-end report-only probe against small synthetic PE data, not vendor DLLs."""
from pathlib import Path
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
from compatibility_corpus_test import fake_pe
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tools'))
import collect_provider_trials as tool

ptx=b'.version 9.1\n.target sm_89\n.address_size 64\n.visible .entry sample() { ret; }\0'
payload=bytes([0xf0,len(ptx)-15])+ptx
header=bytearray(64)
struct.pack_into('<HHIQ',header,0,1,0x101,64,(len(payload)+7)&~7)
struct.pack_into('<I',header,16,len(payload));struct.pack_into('<I',header,28,89)
struct.pack_into('<Q',header,40,0x2041);struct.pack_into('<Q',header,56,len(ptx))
fatbin=struct.pack('<IHHQ',0xba55ed50,1,16,64+((len(payload)+7)&~7))+header+payload+bytes((-len(payload))%8)
raw=bytearray(fake_pe(['NVSDK_NGX_GetGPUArchitecture','NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl'],
                      fatbin=fatbin,text=b'\xb8\x90\x01\x00\x00\xc3'))
raw[0x2500:0x250c]=b'dlfg_kernel\0'
with tempfile.TemporaryDirectory() as temporary:
    root=Path(temporary);path=root/'nvngx_dlssg.dll';path.write_bytes(raw)
    before=hashlib.sha256(path.read_bytes()).hexdigest()
    report=tool.trial(path,Path(sys.argv[1]))
    assert report['trial']['trial_prepared']
    assert not report['trial']['exact_registry_evidence']
    assert report['trial']['changed_copy_bytes']>0 and report['trial']['fatbins']==1
    assert report['trial']['restore_byte_identical'] and report['trial']['idempotent']
    assert hashlib.sha256(path.read_bytes()).hexdigest()==before
    out=root/'reports'
    run=subprocess.run([sys.executable,str(ROOT/'tools/collect_provider_trials.py'),
        '--probe',sys.argv[1],'--provider',str(path),'--out',str(out)],capture_output=True,text=True)
    assert run.returncode==0,run.stderr
    report=json.loads((out/'provider_trials.json').read_text())
    assert report['prepared_trials']==1 and report['test_errors']==[]
    import zipfile
    with zipfile.ZipFile(out/'provider_trials_EVIDENCE_ONLY.zip') as z:
        assert set(z.namelist())=={'provider_trials.json','SHA256SUMS.txt'}
    # Old providers may already advertise a Turing minimum (0x160). That is
    # already permissive for an Ampere target and must not be rejected or
    # rewritten merely because later providers use 0x190.
    legacy=bytearray(fake_pe(['NVSDK_NGX_GetGPUArchitecture','NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl'],
                             fatbin=fatbin,text=b'\xb8\x60\x01\x00\x00\xc3'))
    legacy[0x2500:0x250c]=b'dlfg_kernel\0'
    legacy_path=root/'legacy_nvngx_dlssg.dll';legacy_path.write_bytes(legacy)
    legacy_before=hashlib.sha256(legacy_path.read_bytes()).hexdigest()
    legacy_report=tool.trial(legacy_path,Path(sys.argv[1]))['trial']
    assert legacy_report['trial_prepared'],legacy_report
    assert legacy_report['changed_copy_bytes']>0
    assert legacy_report['restore_byte_identical'] and legacy_report['idempotent']
    assert hashlib.sha256(legacy_path.read_bytes()).hexdigest()==legacy_before

    raw[0x200]=0xcc;path.write_bytes(raw)
    rejected=tool.trial(path,Path(sys.argv[1]))['trial']
    assert not rejected['trial_prepared'] and rejected['changed_copy_bytes']==0
    assert rejected['failure_reason']=='unrecognized minimum-architecture export'
    assert rejected['restore_byte_identical']
print('provider_trial_tool_test: end-to-end preparation, restoration, rejection and report-only ZIP PASS')
