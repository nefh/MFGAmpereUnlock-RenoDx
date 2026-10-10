#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""F1: current MG1, preparation and Streamline evidence, serial and read-only.

Reuses the existing C++ probes and corpus tools. Optional --inspect-provider
captures HOST bytes, excluding CUDA fatbins and standalone ELF payloads.
This fills missing .pdata/host metadata in E2, not a provider implementation.
No vendor DLL is loaded, no GPU is used and no ptxas work is repeated.
"""
from __future__ import annotations
import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
from types import SimpleNamespace
import zipfile

import compatibility_corpus as corpus
import collect_mfg_gate_evidence as mg
import validate_sm75_corpus as sm75
from validate_compatibility_local import executable

ROOT = Path(__file__).resolve().parents[1]
MAX_FILE = 96 * 1024 * 1024
MAX_HOST = 8 * 1024 * 1024
MAX_TOTAL = 128 * 1024 * 1024


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write(path: Path, obj: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def union(ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    result: list[tuple[int, int]] = []
    for a, b in sorted(ranges):
        if a < 0 or b < a:
            raise ValueError('invalid interval')
        if a == b:
            continue
        if result and a <= result[-1][1]:
            result[-1] = (result[-1][0], max(result[-1][1], b))
        else:
            result.append((a, b))
    return result


def subtract(a: int, b: int, omitted: list[tuple[int, int]]) -> list[tuple[int, int]]:
    if not 0 <= a <= b:
        raise ValueError('invalid host interval')
    result = []
    for x, y in union(omitted):
        if y <= a:
            continue
        if x >= b:
            break
        if x > a:
            result.append((a, x))
        a = max(a, y)
    if a < b:
        result.append((a, b))
    return result


def elf_extent(data: bytes, start: int) -> tuple[int, dict]:
    """Bounded ELF64 metadata reader; exports no instruction/constant bytes."""
    def take(at: int, size: int) -> bytes:
        if at < 0 or size < 0 or at + size > len(data) - start:
            raise ValueError('ELF table/section exceeds file-backed section')
        return data[start + at:start + at + size]
    h = take(0, 64)
    if h[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<H', h, 18)[0] != 190:
        raise ValueError('unknown embedded ELF; host capture not safe')
    ph, sh = struct.unpack_from('<QQ', h, 32)
    eh, pe, pn, se, sn, names = struct.unpack_from('<6H', h, 52)
    if eh != 64 or se != 64 or not 0 < sn <= 256 or names >= sn or pn > 64:
        raise ValueError('ELF metadata budgets/layout')
    if pn and pe != 56:
        raise ValueError('ELF program-header layout')
    table = take(sh, se * sn)
    extent = max(64, sh + len(table))
    sections = []
    for n in range(sn):
        name, typ, flags, addr, off, size, link, info, align, entry = struct.unpack_from('<IIQQQQIIQQ', table, n*64)
        if typ != 8 and size:
            take(off, size)
            extent = max(extent, off + size)
        sections.append({'name_offset': name, 'type': typ, 'flags': flags,
                         'offset': off, 'bytes': size})
    if pn:
        programs = take(ph, pe * pn)
        extent = max(extent, ph + len(programs))
        for n in range(pn):
            off = struct.unpack_from('<Q', programs, n*pe + 8)[0]
            size = struct.unpack_from('<Q', programs, n*pe + 32)[0]
            take(off, size)
            extent = max(extent, off + size)
    st = sections[names]
    if st['type'] != 3 or st['bytes'] > 256*1024:
        raise ValueError('ELF string table bounds')
    strings = take(st['offset'], st['bytes'])
    for s in sections:
        at = s.pop('name_offset')
        end = strings.find(b'\0', at)
        if at >= len(strings) or end < at or end-at > 512:
            raise ValueError('ELF section name bounds')
        s['name'] = strings[at:end].decode('ascii', 'strict')
    return extent, {'bytes': extent, 'target_sm': struct.unpack_from('<I', h, 48)[0] & 255,
                    'sha256': sha(take(0, extent)), 'sections': sections}


def host_capture(raw: bytes, output: Path, *, budget: int = MAX_HOST) -> dict:
    """All file-backed host bytes including exception/reloc tables, NOT a DLL.

    Missing GPU intervals are explicit; never synthesize zeros as evidence.
    Malformed CUDA ranges abort rather than accidentally exporting GPU code.
    """
    if len(raw) > MAX_FILE:
        raise ValueError('provider file budget exceeded')
    pe = corpus.PEView(raw)
    containers, errors = corpus.read_envelopes(pe)
    if errors:
        raise ValueError('fatbin parser incomplete; refusing host export: ' + str(errors[:2]))
    omitted: list[tuple[int, int]] = []
    for c in containers:
        s = next(s for s in pe.sections if s['rva'] <= c['rva'] < s['rva']+s['raw_size'])
        off = s['raw_offset'] + c['rva'] - s['rva']
        omitted.append((off, off+c['bytes']))
    omitted = union(omitted)
    elves = []
    for section in pe.sections:
        begin = section['raw_offset']; end = begin + min(section['raw_size'], section['virtual_size'])
        for a, b in subtract(begin, end, omitted):
            at = a
            while (at := raw.find(b'\x7fELF', at, b)) >= 0:
                if len(elves) >= 512:
                    raise ValueError('embedded ELF count budget')
                size, elf = elf_extent(raw[:b], at)
                elf['rva'] = section['rva'] + at - begin
                elf['raw_offset'] = at
                elves.append(elf)
                omitted.append((at, at+size))
                at += size
    omitted = union(omitted)
    if not omitted:
        raise ValueError('no GPU payload to exclude; refusing whole DLL export')
    wanted = [(0, pe.headers, 0, 'PE_HEADERS')]
    for s in pe.sections:
        begin = s['raw_offset']; end = begin + min(s['raw_size'], s['virtual_size'])
        for a, b in subtract(begin, end, omitted):
            wanted.append((a, b, s['rva']+a-begin, s['name']))
    total = sum(b-a for a,b,_,_ in wanted)
    if total > budget:
        raise ValueError(f'host capture budget exceeded: {total} > {budget}')
    if subtract(0, pe.headers, omitted) != [(0, pe.headers)]:
        raise ValueError('GPU/PE header overlap')
    directories = [{'index': n, 'rva': pe.directory(n)[0], 'bytes': pe.directory(n)[1]}
                   for n in range(pe.directory_count)]
    for n in (0, 3, 5):
        rva, size = pe.directory(n)
        if size and not any(r <= rva and rva+size <= r+(b-a) for a,b,r,_ in wanted):
            raise ValueError(f'required host directory {n} not completely captured')
    output.mkdir(parents=True, exist_ok=False)
    segments = []
    for a, b, rva, name in wanted:
        for off in range(a, b, 256*1024):
            body = raw[off:min(b, off+256*1024)]
            digest = sha(body)
            blob = 'blobs/' + digest + '.bin'
            dst = output / blob; dst.parent.mkdir(exist_ok=True)
            if dst.exists() and dst.read_bytes() != body:
                raise ValueError('blob collision')
            dst.write_bytes(body)
            segments.append({'raw_offset': off, 'rva': rva+off-a, 'bytes': len(body),
                             'sha256': digest, 'blob': blob, 'section': name})
    report = {'schema': 'mfg_f1_host_evidence_v1', 'provider_sha256': sha(raw),
              'file_bytes': len(raw), 'preferred_image_base': pe.base,
              'image_bytes': pe.size, 'versions': pe.versions(), 'sections': pe.sections,
              'directories': directories, 'exports': pe.exports(), 'segments': segments,
              'fatbins': corpus.strip_private(containers), 'cuda_elfs': elves,
              'host_bytes': total, 'omitted_file_intervals': omitted,
              'exception_table_fully_captured': bool(pe.directory(3)[1]),
              'data_scope': 'FILE_BACKED_HOST_ONLY_GPU_PAYLOADS_OMITTED',
              'missing_intervals': 'NOT_CAPTURED_NOT_ZERO', 'full_dlls_included': False,
              'vendor_code_executed': False, 'gpu_execution': 'NOT_RUN'}
    write(output/'host.json', report)
    return {'provider_sha256': report['provider_sha256'], 'host_bytes': total,
            'exception_table_fully_captured': report['exception_table_fully_captured'],
            'path': output.name + '/host.json'}


def prepared_trials(raw: bytes, probe: Path, destination: Path) -> dict:
    image, _ = mg.map_pe(raw)
    result = {}
    for arch in ('Turing', 'Ampere'):
        cmd = [str(probe), '--provider', arch]
        values = []
        for _ in range(2):
            cp = subprocess.run(cmd, input=image, capture_output=True, timeout=120)
            if cp.returncode or len(cp.stdout) > 8*1024*1024:
                raise ValueError('preparation probe failure: '+cp.stderr.decode(errors='replace')[:2000])
            value = json.loads(cp.stdout)
            if value.get('compiled_source_id') != sm75.source_id():
                raise ValueError('stale preparation probe')
            if not value.get('idempotent') or not value.get('rollback_identical') or value.get('vendor_code_executed'):
                raise ValueError('preparation transaction invariant failure')
            values.append(value)
        if sm75.comparable(values[0]) != sm75.comparable(values[1]):
            raise ValueError('nondeterministic preparation')
        write(destination/(arch+'.json'), {'first': values[0], 'repeat': values[1]})
        value = values[0]
        result[arch] = {k: value[k] for k in ('prepared', 'endpoint_identified', 'endpoint_selected',
                     'endpoint_payloads', 'endpoint_reason', 'planner', 'failure_reason')}
        result[arch].update(deterministic=True, assembly='NOT_RERUN_HOST_ONLY_CHANGE', gpu='NOT_RUN')
        print(f"  {arch}: prepared={value['prepared']} endpoint={value['endpoint_selected']} reason={value['failure_reason'] or value['endpoint_reason']}", flush=True)
    return result


def seal(out: Path) -> Path:
    selected = []
    for p in sorted(out.rglob('*')):
        if p.is_symlink():
            raise ValueError('refusing symlink in evidence output')
        if not p.is_file() or any(x.startswith('.work-') for x in p.parts):
            continue
        if p.suffix in ('.json', '.log', '.md', '.csv') or (
            p.suffix == '.bin' and p.parent.name == 'blobs' and 'host_capture' in p.parts):
            selected.append(p)
    if any(p.stat().st_size > 32*1024*1024 for p in selected) or sum(p.stat().st_size for p in selected) > MAX_TOTAL:
        raise ValueError('return evidence exceeds budget')
    manifest = out/'SHA256SUMS.txt'
    manifest.write_text(''.join(f'{sha(p.read_bytes())}  {p.relative_to(out).as_posix()}\n' for p in selected), encoding='ascii')
    archive = out/'MFG_F1_EVIDENCE_ONLY.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
        for p in selected+[manifest]:
            z.write(p, p.relative_to(out).as_posix())
    with zipfile.ZipFile(archive) as z:
        if z.testzip() is not None:
            raise ValueError('return ZIP CRC error')
    return archive


def collect(args) -> int:
    out = args.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    errors = []; rows = []; captures = []; interrupted = False
    stack = {'status': 'NOT_RUN', 'runtime_tested': False}
    try:
        gate_probe = executable(args.validation_bin, 'probe_mfg_gate')
        prep_probe = executable(args.validation_bin, 'probe_provider_capability')
        paths = list(args.provider) + list(args.require_provider) + list(args.inspect_provider)
        if args.corpus_root:
            paths += sm75.inputs(SimpleNamespace(provider=None, corpus_root=args.corpus_root))
        paths = sorted({p.resolve() for p in paths})
        required_hashes = {sha(corpus.read_file(p)) for p in args.require_provider}
        if not paths or len(paths) > 256 or len(args.inspect_provider) > 4:
            raise ValueError('need 1..256 inputs, at most 4 explicit host captures')
        code = mg.collect(gate_probe, paths, {p.resolve() for p in args.require_provider}, out/'mfg_gate')
        if code:
            errors.append({'stage': 'mfg_gate', 'error': 'required regression or invariant failure; inspect mfg_gate/summary.json'})
        gates = json.loads((out/'mfg_gate/compatibility_matrix.json').read_text())
        seen = set()
        for i, path in enumerate(paths, 1):
            print(f'PREPARATION [{i}/{len(paths)}] {path}', flush=True)
            try:
                if path.stat().st_size > MAX_FILE: raise ValueError('file budget')
                raw = path.read_bytes(); identity = sha(raw)
                if identity in seen: continue
                seen.add(identity)
                targets = prepared_trials(raw, prep_probe, out/'preparation'/identity)
                if identity in required_hashes and not targets['Ampere']['prepared']:
                    errors.append({'required_provider_sha256': identity,
                                   'error': 'required Ampere preparation did not pass'})
                for gate in (r for r in gates if r['provider_sha256'] == identity):
                    arch = gate['architecture']
                    prep = targets.get(arch, {'prepared': None, 'endpoint_selected': None,
                        'endpoint_reason': 'NOT_APPLICABLE_NATIVE_SM89', 'planner': {}})
                    rows.append({'version': gate['version'], 'provider_sha256': identity,
                        'architecture': arch, 'input_scope': 'FULL_DLL_LOCAL',
                        'preparation': prep, 'mfg_gate': gate,
                        'gpu_executed': False, 'game_validated': False})
                if sha(path.read_bytes()) != identity: raise ValueError('DLL changed during read-only trial')
            except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
                errors.append({'input': str(path), 'error': str(exc)})
        for path in args.inspect_provider:
            path = path.resolve()
            try:
                if path.stat().st_size > MAX_FILE: raise ValueError('file budget')
                raw = path.read_bytes(); identity = sha(raw)
                dest = out/'host_capture'/identity
                if not dest.exists(): captures.append(host_capture(raw, dest))
                if sha(path.read_bytes()) != identity: raise ValueError('DLL changed during host capture')
            except (OSError, ValueError, KeyError, struct.error) as exc:
                errors.append({'host_capture': str(path), 'error': str(exc)})
        if args.corpus_root:
            print('===== EXISTING FULL CORPUS / STREAMLINE CONTRACT PROJECTION =====', flush=True)
            cmd = [sys.executable, str(ROOT/'tools/validate_compatibility_local.py'),
                   '--validation-bin', str(args.validation_bin), '--corpus-root', str(args.corpus_root),
                   '--out', str(out/'stack')]
            print(subprocess.list2cmdline(cmd), flush=True)
            rc = subprocess.run(cmd, timeout=1800).returncode
            file = out/'stack/validation_result.json'
            validation = json.loads(file.read_text()) if file.exists() else {'status': 'MISSING_REPORT'}
            matrix_file = out/'stack/compatibility_matrix.json'
            stack_matrix = json.loads(matrix_file.read_text()) if matrix_file.exists() else {}
            stack = {'validation': validation, 'runtime_tested': False,
                     'bundles': stack_matrix.get('streamline_bundles', [])}
            if rc: errors.append({'stage': 'stack_contract', 'exit': rc})
    except KeyboardInterrupt:
        interrupted = True; errors.append({'error': 'interrupted'})
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        errors.append({'error': str(exc)})
    write(out/'compatibility_matrix.json', {'providers': rows, 'streamline': stack,
          'scope': 'CURRENT_FULL_LOCAL_BYTES_CPU_PROBES_NOT_GPU', 'runtime_tested': False})
    fields = ['version','provider_sha256','architecture','input_scope','preparation','mfg_gate','gpu_executed','game_validated']
    with (out/'compatibility_matrix.csv').open('w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fields); writer.writeheader()
        for row in rows:
            writer.writerow({k: json.dumps(v, sort_keys=True) if isinstance(v,dict) else v for k,v in row.items()})
    text = '| Provider | Target | Prepared | SM75 selection | MFG gate | GPU/game |\n|---|---|---|---|---|---|\n'
    for row in rows:
        p, g = row['preparation'], row['mfg_gate']
        text += f"| {row['version']} | {row['architecture']} | {p.get('prepared')} | {p.get('endpoint_selected') if row['architecture']=='Turing' else 'N/A'} | {g['qualification']} | NOT_RUN |\n"
    text += '\n## Streamline: static contract projection, runtime NOT_RUN\n'
    for b in stack.get('bundles', []):
        text += '\n- ' + str(b['sdk_version']) + ': ' + json.dumps(b.get('current_contract', {}), sort_keys=True)
    (out/'compatibility_matrix.md').write_text(text, encoding='utf-8')
    write(out/'summary.json', {'schema': 'mfg_f1_evidence_v1', 'status': 'COLLECTION_ERROR' if errors else 'EVIDENCE_COLLECTED',
          'errors': errors, 'interrupted': interrupted, 'provider_target_records': len(rows), 'host_captures': captures,
          'full_corpus_attempted': bool(args.corpus_root), 'collector_sha256': sha(Path(__file__).read_bytes()),
          'full_dlls_included': False, 'vendor_code_executed': False, 'gpu_executed': False, 'game_validated': False,
          'f1_complete': False, 'scope_note': 'EVIDENCE_COLLECTED is not compatible/ready/runtime PASS. Open SM75 contracts require review.'})
    archive = seal(out)
    print('RETURN='+str(archive)); print('SHA256='+sha(archive.read_bytes()))
    return 2 if errors else 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--validation-bin', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--corpus-root', type=Path)
    p.add_argument('--provider', type=Path, action='append', default=[])
    p.add_argument('--require-provider', type=Path, action='append', default=[])
    p.add_argument('--inspect-provider', type=Path, action='append', default=[])
    args = p.parse_args()
    if not (args.corpus_root or args.provider or args.require_provider):
        p.error('provide corpus-root or provider/require-provider')
    try:
        return collect(args)
    except (OSError, ValueError) as exc:
        print('COLLECTOR ERROR: '+str(exc), file=sys.stderr); return 2


if __name__ == '__main__':
    raise SystemExit(main())
