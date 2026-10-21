#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Local-only Stage 3 corpus validation and evidence projection.

Reuses compatibility_corpus, qualify_provider, inspect_provider and the production
C++ contract oracle. Never LoadLibrary/execute/modify a corpus DLL. Sequential
isolated workers, bounded input/output and per-file timeout. Evidence ZIP accepts
only reports created here, never corpus bytes, PTX bodies or native executables.
"""
from __future__ import annotations
import argparse
import copy
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import zipfile

import compatibility_corpus as c
import qualify_provider as q

ROOT = Path(__file__).resolve().parents[1]
MAX_OUTPUT = 64 * 1024 * 1024


def executable(directory: Path, name: str) -> Path:
    for path in (directory/name, directory/(name+'.exe'), directory/'Release'/(name+'.exe')):
        if path.is_file():
            return path.resolve()
    raise FileNotFoundError(f'missing validation executable {name} in {directory}')


def json_process(command: list[str], *, timeout: int, accepted=(0,)) -> dict:
    result = subprocess.run(command, capture_output=True, timeout=timeout, check=False)
    if result.returncode not in accepted:
        raise RuntimeError(f'child exit {result.returncode}: '+result.stderr.decode('utf-8', 'replace')[-4096:])
    if len(result.stdout) > MAX_OUTPUT:
        raise RuntimeError('child output budget exceeded')
    value = json.loads(result.stdout)
    if not isinstance(value, dict):
        raise ValueError('child returned non-object JSON')
    return value


def worker(args) -> dict:
    # Protect ordinary Linux/WSL workers from malformed-data expansion. ASan
    # executables reserve a very large virtual shadow range before main(); an
    # RLIMIT_AS cap would prevent their startup, not detect a parser failure.
    # Inspect only our trusted local tool binaries, never a corpus DLL, for the
    # sanitizer runtime symbol. File/decode budgets and timeouts remain active.
    def uses_asan(path):
        if not path:
            return False
        tail = b''
        with path.open('rb') as stream:
            while chunk := stream.read(1024*1024):
                block = tail + chunk
                if b'__asan_init' in block:
                    return True
                tail = block[-32:]
        return False
    sanitized = uses_asan(args.inspector) or uses_asan(args.image_probe)
    if sys.platform.startswith('linux') and not sanitized:
        import resource
        resource.setrlimit(resource.RLIMIT_AS, (2*1024**3, 2*1024**3))
    data = c.read_file(args.file)
    reference = c.make_reference(c.read_file(args.reference)) if args.reference else None
    report = c.analyze_bytes(data, reference=reference, provider_requested=args.provider)
    report['worker_budget'] = ('SANITIZER_SHADOW_EXEMPT_FILE_DECODE_TIMEOUT_BOUNDED' if sanitized
                               else 'LINUX_2G_ADDRESS_SPACE' if sys.platform.startswith('linux')
                               else 'FILE_DECODE_TIMEOUT_BOUNDED')
    if report['pe_status']=='PASS' and (args.file.name.lower()=='sl.dlss_g.dll' or
                                      b'FG1 DLL has been detected' in data):
        pe=c.PEView(data)
        image=bytearray(pe.size)
        image[:pe.headers]=data[:pe.headers]
        for section in pe.sections:
            size=section['raw_size']
            image[section['rva']:section['rva']+size]=data[section['raw_offset']:section['raw_offset']+size]
        result=subprocess.run([str(args.image_probe),'--inspect-stdin'],input=image,
                              capture_output=True,check=True,timeout=90)
        report['plugin_scan_production_seam']=json.loads(result.stdout)
        del image
    if args.provider:
        report['static_qualification'] = q.qualify(data, q.registry())
        report['cpp_targets'] = {}
        for architecture in ('Ampere', 'Turing', 'Ada'):
            try:
                report['cpp_targets'][architecture] = json_process(
                    [str(args.inspector), str(args.file), architecture], timeout=90, accepted=(0, 2))
            except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
                # A rejected/unrecognized transform is valid fail-closed output.
                # A malformed JSON, crash or timeout is explicitly a test error.
                report['cpp_targets'][architecture] = {'validation_status':'ERROR', 'error':str(error)}
    return report


def major(row: dict | None) -> int:
    try:
        return int(str((row or {}).get('file_version') or '').split('.')[0])
    except ValueError:
        return 0  # Never infer the calling ABI from an SDK directory label.


def contract_projection(rows: list[dict], probe: Path) -> dict:
    def one(name):
        matches = [r for r in rows if r['file_name'].lower() == name]
        return matches[0] if len(matches) == 1 and matches[0]['pe_status'] == 'PASS' else None
    interposer, common, plugin = (one(n) for n in ('sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll'))
    symbols = sorted(c.own_exports(interposer['exports'])) if interposer else []
    decision = json_process([str(probe), '--contract', str(major(interposer)),
                             str(int(common is not None)), str(int(interposer is not None)), *symbols], timeout=10)
    parent_major = {1: 1, 2: 2}.get(decision['typed_abi'], 0)
    gateway = json_process([str(probe), '--gateway', str(parent_major), str(major(plugin)),
        str(int(plugin is not None)), str(int(plugin is not None and
        'slGetPluginFunction' in c.own_exports(plugin['exports']))), str(int(plugin is not None))], timeout=10)
    return {'interposer': decision, 'plugin': gateway,
            'admission_scope': 'static adapter eligibility; no native support query was called',
            'dynamic_mfg': 'RUNTIME_NEGOTIATION_REQUIRED' if gateway['gateway']=='AVAILABLE' and parent_major==2 else 'UNQUALIFIED',
            'ui_contract': 'TYPED_TAG_CONTRACT_CANDIDATE' if parent_major==2 else 'UNQUALIFIED',
            'ui_resources': 'RUNTIME_UNOBSERVED',
            'late_attach_recoverability': 'safe retry of current entries; startup and feature-create are not replayed',
            'live_unload_safe': False, 'runtime_tested': False}


def projection(raw: dict, probe: Path, *, provenance: str, source_sha: str, previous=None) -> dict:
    report = copy.deepcopy(raw)
    report['schema'] = 'mfg_universal_compatibility_stage3_v1'
    report['scope'] = 'static-evidence-and-production-contract-projection'
    report['evidence'] = {'corpus_bytes': provenance, 'source_sha256': source_sha,
                          'current_runtime_tested': False, 'release_qualified': False,
                          'warning': 'structural evidence and static eligibility do not authorize a private patch'}
    bundles = {}
    for b in report['streamline_bundles']:
        rows = [r for r in report['libraries'] if r['sdk_version']==b['sdk_version']]
        current = contract_projection(rows, probe)
        b['current_contract'] = current
        b['missing_component_checks'] = {}
        for name in ('sl.common.dll', 'sl.interposer.dll', 'sl.dlss_g.dll'):
            missing = contract_projection([r for r in rows if r['file_name'].lower()!=name], probe)
            if name == 'sl.interposer.dll' and missing['interposer']['admission']=='AVAILABLE':
                raise ValueError('missing module incorrectly admitted: '+name)
            if name=='sl.dlss_g.dll' and missing['plugin']['gateway']=='AVAILABLE':
                raise ValueError('missing plugin incorrectly admitted')
            b['missing_component_checks'][name] = missing
        bundles[b['sdk_version']] = current
    old = {r['path']:r for r in previous['libraries']} if previous else {}
    for row in report['libraries']:
        sig = row.get('recognized_signatures', {})
        qualifier = row.get('static_qualification', {})
        structural = lambda key: sig.get(key, {}).get('state', 'UNKNOWN')
        row['validation_evidence'] = provenance
        known_layout = bool(row.get('existing_profile_exact_file_match'))
        # This historical metadata report does not execute the production
        # preparer/MG1. Unknown exact anchors must not be projected as a runtime
        # refusal after the production path became structural.
        private_candidate = 'NOT_RUN_PRODUCTION_PATCHER'
        row['runtime_projection'] = {
            'provider_recognition': ('Structural' if row.get('existing_profile_exact_file_match') else
                                    'Partial' if row.get('provider_identity_observed') else 'Unknown-safe'),
            'profile_file_qualification': qualifier.get('qualification', 'NOT_REEXECUTED'),
            'retarget_payload': structural('retarget_payload_multiset'),
            'retarget_runtime': private_candidate,
            'temporal_kernels': {k:v.get('state','UNKNOWN') for k,v in sig.get('quality_kernels', {}).items()},
            'temporal_runtime': private_candidate,
            'preset_observer': structural('preset'),
            'preset_runtime': private_candidate,
            'validated_warp': sig.get('quality_kernels', {}).get('warp', {}).get('state','UNKNOWN'),
            'warp_runtime': private_candidate,
            'host_mfg_gates': 'NOT_RUN_PRODUCTION_RESOLVER',
            'legacy_exact_mfg_anchor_observation': structural('mfg_gates'),
            'legacy_exact_profile_match': known_layout,
            'endpoint_code': structural('endpoint_code'),
            'ngx_d3d12_export': row['capabilities'].get('ngx_d3d12_entry', {}).get('state','UNKNOWN'),
            'ngx_vulkan_export': row['capabilities'].get('ngx_vulkan_entry', {}).get('state','UNKNOWN'),
            'unsafe_unknown_behavior': 'unverified feature stays closed; no global addon-disable decision',
            'streamline_assembly': bundles.get(row['sdk_version']),
            'current_runtime_tested': False}
        previous_row = old.get(row['path'])
        row['identity_change'] = ('NO_PREVIOUS_EVIDENCE' if previous_row is None else
                                  'UNCHANGED_SHA256' if previous_row['sha256']==row['sha256'] else
                                  'CHANGED_SHA256_RECOMPUTED_NO_PATH_CACHE')
    return report


def write_projection(report: dict, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out/'compatibility_matrix.json').write_bytes(c.canonical(report))
    columns = ['kind','path','sha256','file_version','family','pe_status','sdk_version','validation_evidence',
               'identity_change','fatbin_count','runtime_projection','exports','sections']
    with (out/'compatibility_matrix.csv').open('w', encoding='utf-8', newline='') as file:
        writer=csv.DictWriter(file,columns,lineterminator='\n');writer.writeheader()
        for row in report['libraries']:
            values = {}
            for key in columns:
                item = row.get(key)
                values[key] = json.dumps(item,sort_keys=True,ensure_ascii=True) if isinstance(item,(dict,list)) else item
            writer.writerow(values)
    lines = ['# Compatibility matrix - Stage 3 candidate','',
        '**Evidence: '+report['evidence']['corpus_bytes']+'. Runtime tested: NO. Release qualified: NO.**','',
        'Full identity, exports, sections, fatbins, signatures, decisions and their scope are in JSON/CSV.',
        'This report projects static contracts; current MG1/preparation execution is reported separately by collect_f1_evidence.py.', '',
        '## Standalone providers', '',
        '| File version | SHA256 prefix | Family | Fatbins | Retarget payload | Preset evidence | Warp kernel |',
        '|---|---|---|---:|---|---|---|']
    for row in report['libraries']:
        if row['kind'] != 'dlss_g': continue
        pr=row['runtime_projection']
        lines.append(f'| {row["file_version"]} | `{row["sha256"][:16]}` | {row["family"]} | {row.get("fatbin_count",0)} | {pr["retarget_payload"]} | {pr["preset_observer"]} | {pr["validated_warp"]} |')
    lines += ['', '## Streamline assemblies', '',
        '| SDK directory label | Export family | Typed adapter | Static admission | Plugin gateway | Dynamic MFG |',
        '|---|---|---|---|---|---|']
    for bundle in report['streamline_bundles']:
        pr=bundle['current_contract']; inter=pr['interposer']; plugin=pr['plugin']
        lines.append(f'| {bundle["sdk_version"]} | {inter["surface"]} | {inter["typed_abi"]} | {inter["admission"]} | {plugin["gateway"]} | {pr["dynamic_mfg"]} |')
    lines += ['', 'Adapter IDs: 0 unknown-safe, 1 legacy, 2 modern. Version major comes from the DLL resource, not the directory label.',
              'Late attach retries current entries; missing startup is not replayed. Unobserved same-address unload/reload is NOT qualified.',
              'The historical 310.9.1 provider is an optional known-good configuration for layout-specific features, not a global requirement.', '']
    (out/'compatibility_matrix.md').write_text('\n'.join(lines), encoding='utf-8', newline='\n')


def seal(out: Path) -> Path:
    allowed = {'.json', '.csv', '.md', '.txt', '.log'}
    files = [p for p in sorted(out.rglob('*')) if p.is_file() and p.name!='SHA256SUMS.txt']
    for path in files:
        if path.suffix.lower() not in allowed:
            raise ValueError('refusing non-report in evidence ZIP: '+path.name)
    sums = ''.join(c.sha(p.read_bytes())+'  '+p.relative_to(out).as_posix()+'\n' for p in files)
    (out/'SHA256SUMS.txt').write_text(sums, encoding='ascii')
    target = out.with_name(out.name+'_EVIDENCE_ONLY.zip')
    with zipfile.ZipFile(target,'w',zipfile.ZIP_DEFLATED) as archive:
        for path in files+[out/'SHA256SUMS.txt']:
            archive.write(path, path.relative_to(out).as_posix())
    return target


def main() -> int:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--corpus-root', type=Path)
    ap.add_argument('--validation-bin', type=Path)
    ap.add_argument('--out', type=Path)
    ap.add_argument('--historical', type=Path, help='project existing metadata only; never claim a fresh DLL run')
    ap.add_argument('--previous', type=Path, help='identity change comparison; never used as an authorization cache')
    ap.add_argument('--timeout', type=int, default=180)
    ap.add_argument('--worker', action='store_true')
    ap.add_argument('--file', type=Path);ap.add_argument('--reference', type=Path)
    ap.add_argument('--provider', action='store_true');ap.add_argument('--inspector', type=Path)
    ap.add_argument('--image-probe', type=Path)
    args=ap.parse_args()
    if args.worker:
        sys.stdout.buffer.write(c.canonical(worker(args)));return 0
    if not args.validation_bin or not args.out or (not args.corpus_root and not args.historical):
        ap.error('--validation-bin --out and --corpus-root or --historical are required')
    if args.out.exists() and any(args.out.iterdir()):
        ap.error('output directory must be new or empty (preserve prior evidence)')
    args.out.mkdir(parents=True,exist_ok=True)
    failures=[];unique_checks=0
    run={'result':'IN_PROGRESS','full_corpus_current_run':not bool(args.historical),'runtime_tested':False}
    if args.historical:
        run['historical_input_sha256'] = c.sha(args.historical.read_bytes())
    try:
        probe=executable(args.validation_bin,'compatibility_contract_probe')
        inspector=executable(args.validation_bin,'inspect_provider')
        surface_probe=executable(args.validation_bin,'compatibility_foundation_test')
        image_probe=executable(args.validation_bin,'mapped_image_bounds_test')
        source_identity = {p.relative_to(ROOT).as_posix():c.sha(p.read_bytes())
            for folder in ('src', 'tools', 'contrib')
            for p in sorted((ROOT/folder).rglob('*')) if p.is_file() and '__pycache__' not in p.parts}
        (args.out/'analyzer_source_sha256.json').write_bytes(c.canonical(source_identity))
        if args.historical:
            raw=c.load_json(args.historical)
            provenance='HISTORICAL_STAGE1_METADATA_REPROJECTED_NOT_A_CURRENT_DLL_RUN'
        else:
            def analyze(path, reference, requested):
                nonlocal unique_checks
                command=[sys.executable,str(Path(__file__).resolve()),'--worker','--file',str(path),'--inspector',str(inspector),'--image-probe',str(image_probe)]
                if reference: command += ['--reference',str(reference)]
                if requested: command += ['--provider']
                try:
                    a=json_process(command,timeout=args.timeout)
                    b=json_process(command,timeout=args.timeout)
                    if c.canonical(a)!=c.canonical(b):raise ValueError('non-deterministic worker result')
                    if any(v.get('validation_status')=='ERROR' for v in a.get('cpp_targets',{}).values()):
                        raise ValueError('native target inspection error')
                    unique_checks += 1
                    a['deterministic_two_process_replay']=True
                    return a
                except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
                    failures.append({'file':str(path),'error':str(error)})
                    value=c.analyze_bytes(b'',provider_requested=requested)
                    value.update(sha256=c.sha(c.read_file(path)), file_bytes=path.stat().st_size,
                                 pe_status='ANALYSIS_ERROR',analysis_error=str(error),deterministic_two_process_replay=False)
                    return value
            root=args.corpus_root
            raw=c.collect(root/'dlss_g',root/'streamline',root/'meta',analyzer=analyze)
            provenance='CURRENT_LOCAL_DLL_BYTES_TWO_ISOLATED_RUNS_PER_UNIQUE_SHA256'
        (args.out/'raw_matrix.json').write_bytes(c.canonical(raw))
        for row in raw['libraries']:
            names=sorted(c.own_exports(row['exports']))
            result=subprocess.run([str(surface_probe),'--surface',*names],capture_output=True,check=True,text=True,timeout=10)
            if result.stdout.strip()!=c.export_surface(row['exports']):raise ValueError('C++/Python surface mismatch: '+row['path'])
        prior=c.load_json(args.previous) if args.previous else None
        result=projection(raw,probe,provenance=provenance,source_sha=c.sha(c.canonical(source_identity)),previous=prior)
        write_projection(result,args.out)
        # Projecting the same bytes again must be byte-identical as well.
        if c.canonical(result)!=c.canonical(projection(raw,probe,provenance=provenance,
                source_sha=c.sha(c.canonical(source_identity)),previous=prior)):
            raise ValueError('non-deterministic contract projection')
        summary=raw['summary']
        coverage=(summary['dlssg_corpus_records']>=35 and summary['streamline_records']>=194 and summary['streamline_sdk_versions']>=15)
        passed=not failures and coverage and summary['pe_pass']==summary['dll_records']
        run.update(result=('METADATA_REPLAY_PASS' if args.historical else 'PASS') if passed else 'FAIL',
                   summary=summary, current_unique_two_process_checks=unique_checks,
                   cpp_python_surface_comparisons=len(raw['libraries']),
                   missing_component_tests=3*len(raw['streamline_bundles']),
                   projection_deterministic=True, failures=failures,
                   corpus_integrity=('HISTORICAL_NOT_RECHECKED' if args.historical else 'CURRENT_SHA256_AND_SIZE_VERIFIED'),
                   target_preparation_scope='C++ offline fatbin transforms only; no mapped descriptor binding or GPU execution')
        code=0 if passed else 2
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        run.update(result='ERROR',error=str(error),failures=failures);code=1
    (args.out/'validation_result.json').write_bytes(c.canonical(run))
    archive=seal(args.out)
    print(json.dumps(run,indent=2));print('EVIDENCE_ONLY_ZIP='+str(archive.resolve()))
    return code

if __name__=='__main__':
    raise SystemExit(main())
