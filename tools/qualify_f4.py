#!/usr/bin/env python3
"""Freeze a source session and bundle explicit F4 build/test evidence.

No test is inferred from a binary, no GPU result from CTest or a return code.
This tool never launches a game, vendor DLL, WSL or a compiler. It reuses the
existing F1 corpus collector and accepts CTest JUnit output from explicit runs.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import re
import sys
import time
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path, PurePosixPath

MAX_FILE = 96 * 1024 * 1024
MAX_ZIP_EXPANDED = 160 * 1024 * 1024
ROOTS = ('src', 'tools', 'contrib')
REQUIRED_NATIVE = {'vulkan_sdk_abi_test', 'small_stack_64', 'small_stack_128'}
REQUIRED_COMMON = {'assertions_enabled_test', 'endpoint_flat_test', 'mfg_gate_test',
                   'ui_alpha_gpu_test', 'passive_metrics_test', 'f4_passive_read_test',
                   'f4_tools_test'}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read(path: Path, cap: int = MAX_FILE) -> bytes:
    if path.is_symlink() or not path.is_file() or path.stat().st_size > cap:
        raise ValueError('not a bounded regular file: ' + str(path))
    with path.open('rb') as f:
        data = f.read(cap + 1)
    if len(data) > cap:
        raise ValueError('file grew beyond limit')
    return data


def normalize_text(data: bytes) -> bytes:
    # Binary resources must never be treated as line-ending-normalized text.
    try:
        data.decode('utf-8')
    except UnicodeDecodeError:
        return data
    return data.replace(b'\r\n', b'\n')


def canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=False) + '\n').encode('utf-8')


def safe_name(name: str) -> str:
    p = PurePosixPath(name)
    if not name or p.is_absolute() or '..' in p.parts or ':' in name or '\\' in name:
        raise ValueError('unsafe relative path: ' + name)
    return p.as_posix()


def source_manifest(project: Path) -> dict:
    records = {}
    for folder in ROOTS:
        root = project / folder
        if not root.is_dir():
            raise ValueError('missing source folder: ' + str(root))
        for p in sorted(root.rglob('*')):
            if '__pycache__' in p.parts or p.suffix in ('.pyc', '.pyo') or not p.is_file():
                continue
            if not p.resolve().is_relative_to(project.resolve()) or p.is_symlink():
                raise ValueError('source escapes repository: ' + str(p))
            data = read(p)
            records[p.relative_to(project).as_posix()] = {
                'bytes': len(data), 'sha256': sha(data),
                'lf_sha256': sha(normalize_text(data))}
    if 'src/addons/mfgunlock/addon.cpp' not in records:
        raise ValueError('core source missing')
    identity = sha(canonical({k: v['lf_sha256'] for k, v in records.items()}))
    runtime = sha(canonical({k: v['lf_sha256'] for k, v in records.items() if k.startswith('src/')}))
    return {'identity_lf_sha256': identity, 'runtime_lf_sha256': runtime, 'files': records}


def junit(data: bytes, required: set[str]) -> dict:
    if len(data) > MAX_FILE or b'<!DOCTYPE' in data or b'<!ENTITY' in data:
        raise ValueError('invalid XML input')
    root = ET.fromstring(data)
    cases = list(root.iter('testcase'))
    names = [e.get('name', '') for e in cases]
    bad = [e.get('name', '') for e in cases if
           e.find('failure') is not None or e.find('error') is not None or e.find('skipped') is not None or
           e.get('status', '').lower() in ('fail', 'failed', 'notrun', 'disabled')]
    declared_bad = any(int(e.get(k, '0')) > 0 for e in root.iter()
                       if e.tag in ('testsuite', 'testsuites') for k in ('errors', 'failures', 'disabled', 'skipped'))
    if root.get('tests') is not None and int(root.get('tests')) != len(cases):
        raise ValueError('JUnit declared total disagrees with test cases')
    duplicates = len(names) != len(set(names))
    missing = sorted(required - set(names))
    okay = bool(cases) and not bad and not declared_bad and not duplicates and not missing
    return {'status': 'PASS' if okay else 'FAIL', 'tests': len(cases),
            'failed_or_skipped': bad, 'missing_required': missing, 'duplicate_names': duplicates,
            'names': names, 'xml_sha256': sha(data), 'scope': 'CTest process results, not GPU execution'}


def inspect_corpus(path: Path, project: Path) -> dict:
    raw = read(path)
    import io
    with zipfile.ZipFile(io.BytesIO(raw)) as z:
        items = z.infolist()
        if len(items) > 10000 or sum(i.file_size for i in items) > MAX_ZIP_EXPANDED:
            raise ValueError('corpus evidence archive exceeds budget')
        names = [safe_name(i.filename) for i in items if not i.is_dir()]
        if len(names) != len(set(names)):
            raise ValueError('duplicate evidence archive member')
        for name in names:
            if Path(name).suffix.lower() in ('.dll', '.exe', '.addon64', '.bin', '.cubin', '.ptx'):
                raise ValueError('not an evidence-only archive: ' + name)
        sums = z.read('SHA256SUMS.txt').decode('ascii').splitlines()
        verified = set()
        for line in sums:
            digest, name = line.split('  ', 1)
            safe_name(name)
            if name in verified or not re.fullmatch('[0-9a-f]{64}', digest):
                raise ValueError('invalid corpus checksum row')
            if sha(z.read(name)) != digest:
                raise ValueError('corpus evidence checksum mismatch: ' + name)
            verified.add(name)
        if verified != set(names) - {'SHA256SUMS.txt'}:
            raise ValueError('corpus manifest does not cover the archive exactly')
        summary = json.loads(z.read('summary.json'))
        stack = json.loads(z.read('stack/validation_result.json'))
        identity = json.loads(z.read('stack/analyzer_source_sha256.json'))
        current = source_manifest(project)['files']
        differences = [k for k, v in identity.items() if safe_name(k) not in current or current[k]['sha256'] != v]
        uncovered = sorted(set(current) - set(identity))
        structural = json.loads(z.read('mfg_gate/summary.json'))
        okay = (summary.get('status') == 'EVIDENCE_COLLECTED' and not summary.get('errors')
                and not summary.get('interrupted') and stack.get('result') == 'PASS'
                and stack.get('full_corpus_current_run') is True and not stack.get('failures')
                and not differences and not uncovered and not structural.get('errors'))
        return {'status': 'PASS' if okay else 'FAIL_OR_STALE', 'archive_sha256': sha(raw),
                'verified_members': len(verified), 'source_mismatches': sorted(differences),
                'source_not_covered': uncovered, 'summary': stack.get('summary'),
                'mfg_gate': structural, 'collector_scope': summary.get('scope_note'),
                'runtime_tested': False}


def begin(project: Path, out: Path) -> dict:
    if out.exists():
        raise ValueError('use a new output directory; do not replace a prior session')
    manifest = source_manifest(project)
    result = {'schema': 'mfg_f4_session_v1', 'started_ns': time.time_ns(),
              'project': str(project.resolve()), 'source': manifest,
              'candidate': 'BASELINE_OR_EXPLICIT_SOURCE_PATCH', 'runtime_status': 'NOT_RUN'}
    out.mkdir(parents=True)
    (out / 'session.json').write_bytes(canonical(result))
    return result


def finish(project: Path, out: Path, tests: dict[str, Path], binary: Path,
           build_log: Path, corpus: Path, extra_logs: list[Path]) -> dict:
    session_bytes = read(out / 'session.json')
    session = json.loads(session_bytes)
    if session.get('schema') != 'mfg_f4_session_v1' or session.get('project') != str(project.resolve()):
        raise ValueError('wrong session or project')
    current = source_manifest(project)
    if current['identity_lf_sha256'] != session['source']['identity_lf_sha256']:
        raise ValueError('sources changed during session; start a separate baseline/candidate run')
    required = {'gcc', 'ubsan', 'native'}
    if set(tests) != required:
        raise ValueError('supply gcc, ubsan and native JUnit reports')
    payloads = {'session.json': session_bytes}
    results = {}
    for label, path in tests.items():
        if path.stat().st_mtime_ns < session['started_ns']:
            raise ValueError('stale test artifact predates session: ' + str(path))
        data = read(path)
        must = REQUIRED_COMMON | (REQUIRED_NATIVE if label == 'native' else {'ui_native_precedence_test'})
        if label == 'gcc':
            must |= {'compatibility_full_corpus_test'}
        results[label] = junit(data, must)
        payloads[f'tests/{label}.xml'] = data
    if binary.suffix.lower() != '.addon64' or not read(binary).startswith(b'MZ'):
        raise ValueError('expected an actual built addon, never packed into the return ZIP')
    if binary.stat().st_mtime_ns < session['started_ns']:
        raise ValueError('addon predates session; not evidence of this build')
    addon_hash = sha(read(binary))
    log = read(build_log)
    log_text = log.decode('utf-16' if log.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig', errors='replace')
    if addon_hash.lower() not in log_text.lower():
        raise ValueError('build log does not record this addon SHA256')
    payloads['logs/build.log'] = log
    for i, path in enumerate(extra_logs):
        if path.suffix.lower() not in ('.log', '.txt'):
            raise ValueError('only explicit text compiler/self-test logs')
        payloads[f'logs/extra_{i:02d}.log'] = read(path, 32 * 1024 * 1024)
    c = inspect_corpus(corpus, project)
    payloads['corpus/MFG_F1_EVIDENCE_ONLY.zip'] = read(corpus)
    okay = all(v['status'] == 'PASS' for v in results.values()) and c['status'] == 'PASS'
    result = {'schema': 'mfg_f4_qualification_v1',
              'status': 'STATIC_BUILD_VALIDATED_RUNTIME_CANDIDATE' if okay else 'VALIDATION_FAILED',
              'source_identity_lf_sha256': current['identity_lf_sha256'],
              'runtime_source_lf_sha256': current['runtime_lf_sha256'],
              'source_unchanged_during_session': True,
              'binary': {'path': str(binary), 'sha256': addon_hash, 'packed': False},
              'tests': results, 'corpus': c,
              'gpu_executed': 'NOT_RUN', 'game_validated': 'NOT_RUN',
              'hdr_image_validated': 'NOT_RUN', 'performance_AB': 'NOT_RUN',
              'release_qualified': False,
              'limits': ['A captured build/log association is not a reproducible binary proof.',
                         'JUnit is test execution evidence, not GPU/driver/HDR evidence.',
                         'Keep separately obtained runtime/capture results with this exact addon hash.']}
    payloads['qualification.json'] = canonical(result)
    payloads['SHA256SUMS.txt'] = ''.join(f'{sha(v)}  {k}\n' for k, v in sorted(payloads.items())).encode('ascii')
    if sum(map(len, payloads.values())) > MAX_ZIP_EXPANDED:
        raise ValueError('combined evidence exceeds budget')
    archive = out / 'F4_QUALIFICATION_EVIDENCE_ONLY.zip'
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED) as z:
        for name, data in sorted(payloads.items()):
            z.writestr(name, data)
    (out / 'qualification.json').write_bytes(canonical(result))
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('action', choices=('begin', 'finish'))
    ap.add_argument('--project', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    for label in ('gcc', 'ubsan', 'native'):
        ap.add_argument(f'--{label}-junit', type=Path)
    ap.add_argument('--binary', type=Path)
    ap.add_argument('--build-log', type=Path)
    ap.add_argument('--corpus', type=Path)
    ap.add_argument('--log', type=Path, action='append', default=[])
    a = ap.parse_args()
    try:
        if a.action == 'begin':
            result = begin(a.project.resolve(), a.out)
            print('SESSION=' + str(a.out));print('SOURCE=' + result['source']['identity_lf_sha256'])
            return 0
        if any(x is None for x in (a.binary, a.build_log, a.corpus, a.gcc_junit, a.ubsan_junit, a.native_junit)):
            raise ValueError('finish requires binary/build-log/corpus and all three JUnit reports')
        result = finish(a.project.resolve(), a.out,
                        {'gcc': a.gcc_junit, 'ubsan': a.ubsan_junit, 'native': a.native_junit},
                        a.binary, a.build_log, a.corpus, a.log)
        print('STATUS=' + result['status'])
        print('RETURN=' + str(a.out / 'F4_QUALIFICATION_EVIDENCE_ONLY.zip'))
        return 0 if result['status'] == 'STATIC_BUILD_VALIDATED_RUNTIME_CANDIDATE' else 2
    except (ValueError, OSError, KeyError, TypeError, zipfile.BadZipFile, ET.ParseError) as e:
        print('F4 CHECKPOINT ERROR: ' + str(e), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
