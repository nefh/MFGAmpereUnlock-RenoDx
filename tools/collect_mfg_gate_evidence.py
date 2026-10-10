#!/usr/bin/env python3
"""MG1: serial host-gate qualification using the production C++ probe.

No vendor code/GPU execution; no DLLs in the return ZIP. The mapped private copy
is transient. Unresolved gates are not counted as MFG PASS. Required regression
inputs must actually qualify and pass the mock transaction on all three targets.
"""
from __future__ import annotations
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import zipfile
import sys
from compatibility_corpus import PEView

ROOT = Path(__file__).resolve().parents[1]
MAX_FILE = 96 * 1024 * 1024
MAX_IMAGE = 128 * 1024 * 1024
ARCHES = ('Ampere', 'Turing', 'Ada')

def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def compiled_id(root: Path = ROOT) -> str:
    paths = list((root/'src/addons/mfgunlock').rglob('*.hpp'))
    for ext in ('*.h', '*.hpp'):
        paths += list((root/'contrib/validation/tests/provider_mocks').rglob(ext))
    paths.append(root/'contrib/validation/tools/probe_mfg_gate.cpp')
    lines = ''.join(f'{p.relative_to(root).as_posix()}|{digest(p.read_bytes())}\n'
                    for p in sorted(set(paths), key=lambda p:p.as_posix()))
    return digest(lines.encode('utf-8'))

def map_pe(raw: bytes) -> tuple[bytes, dict]:
    if len(raw)>MAX_FILE:
        raise ValueError('file budget exceeded')
    pe = PEView(raw)
    if not 0 < pe.headers <= pe.size <= MAX_IMAGE:
        raise ValueError('image budget exceeded')
    mapped = bytearray(pe.size)
    mapped[:pe.headers] = raw[:pe.headers]
    for s in pe.sections:
        n = min(s['raw_size'], s['virtual_size'])
        mapped[s['rva']:s['rva']+n] = pe.take(s['raw_offset'], n)
    return bytes(mapped), {'versions':pe.versions(), 'sections':pe.sections,
                           'timestamp':pe.timestamp, 'image_bytes':pe.size}

def write_json(path: Path, data: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True)+'\n', encoding='utf-8')

def package(out: Path) -> Path:
    files = sorted(p for p in out.rglob('*') if p.is_file() and
                   p.suffix in ('.json', '.csv', '.md', '.log') and not any(part.startswith('.work-') for part in p.parts))
    if any(p.stat().st_size>16*1024*1024 for p in files):
        raise ValueError('return file budget exceeded')
    manifest = out/'SHA256SUMS.txt'
    manifest.write_text(''.join(f'{digest(p.read_bytes())}  {p.relative_to(out).as_posix()}\n' for p in files))
    target = out/'MFG_GATE_STRUCTURAL_EVIDENCE_ONLY.zip'
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as z:
        for p in files+[manifest]:
            z.write(p,p.relative_to(out).as_posix())
    return target

def collect(probe: Path, files: list[Path], required: set[Path], out: Path) -> int:
    out.mkdir(parents=True, exist_ok=False)
    identity=compiled_id(); records=[]; errors=[]; seen=set(); required_hashes=set()
    for path in required:
        if not path.is_file() or path.stat().st_size>MAX_FILE:
            raise ValueError(f'required provider missing/too large: {path}')
        required_hashes.add(digest(path.read_bytes()))
    with tempfile.TemporaryDirectory(prefix='.work-',dir=out) as temp:
        image_path=Path(temp)/'mapped.image'
        for index,path in enumerate(files,1):
            print(f'[{index}/{len(files)}] {path}',flush=True)
            try:
                if path.stat().st_size>MAX_FILE:raise ValueError('file budget exceeded')
                raw=path.read_bytes(); sha=digest(raw)
                if sha in seen:continue
                seen.add(sha)
                image,metadata=map_pe(raw);image_path.write_bytes(image); pending=[]
                for arch in ARCHES:
                    cp=subprocess.run([str(probe),str(image_path),arch],capture_output=True,timeout=60)
                    if len(cp.stdout)>8*1024*1024:raise ValueError('probe output budget exceeded')
                    if cp.returncode:raise RuntimeError(f'probe exit {cp.returncode}: {cp.stderr.decode(errors="replace")[:2000]}')
                    d=json.loads(cp.stdout)
                    if d.get('compiled_source_id')!=identity:raise ValueError('stale probe: compiled source identity mismatch')
                    again=subprocess.run([str(probe),str(image_path),arch],capture_output=True,timeout=60)
                    if again.returncode or json.loads(again.stdout)!=d:raise ValueError('nondeterministic probe output')
                    if d.get('registry_used') or not d.get('attempted') or not d.get('rollback_identical') or d.get('vendor_code_executed'):
                        raise ValueError('structural attempt or rollback invariant failed')
                    d.update(provider_sha256=sha, input_path=str(path), input_metadata=metadata,
                             input_changed=False, deterministic=True,
                             provider_preparation='NOT_RUN_HOST_GATE_ONLY')
                    pending.append((arch,d))
                    row={k:d[k] for k in ('provider_sha256','architecture','qualification','reason','ceiling','candidates','applied_copy','rollback_identical','deterministic')}
                    row['version']=metadata['versions'].get('file')
                    print(f"  {arch}: gate={d['qualification']} ceiling={d['ceiling']} apply={d['applied_copy']} rollback={d['rollback_identical']}",flush=True)
                if digest(path.read_bytes())!=sha:raise ValueError('input changed during collection')
                for arch,d in pending:
                    write_json(out/'providers'/sha/(arch+'.json'),d)
                    row={k:d[k] for k in ('provider_sha256','architecture','qualification','reason','ceiling','candidates','applied_copy','rollback_identical','deterministic')}
                    row['version']=metadata['versions'].get('file');records.append(row)
            except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
                errors.append({'input':str(path),'error':str(exc)})
                print('  ERROR:',exc,flush=True)
    for sha in required_hashes:
        rr=[r for r in records if r['provider_sha256']==sha]
        if len(rr)!=3 or any(r['qualification']!='STRUCTURAL_VERIFIED' or not r['applied_copy'] for r in rr):
            errors.append({'required_provider_sha256':sha,'error':'required regression did not qualify on all targets'})
    write_json(out/'compatibility_matrix.json',records)
    fields=['version','provider_sha256','architecture','qualification','reason','ceiling','candidates','applied_copy','rollback_identical','deterministic']
    with (out/'compatibility_matrix.csv').open('w',newline='',encoding='utf-8') as f:
        w=csv.DictWriter(f,fields);w.writeheader();w.writerows(records)
    md='| Provider | SHA prefix | Target | Host gate | Ceiling | Reason |\n|---|---|---|---|---:|---|\n'
    md+=''.join(f"| {r['version']} | {r['provider_sha256'][:12]} | {r['architecture']} | {r['qualification']} | {r['ceiling']} | {r['reason']} |\n" for r in records)
    (out/'compatibility_matrix.md').write_text(md,encoding='utf-8')
    write_json(out/'summary.json',{'schema':'mfg_gate_mg1','status':'ERROR' if errors else 'HOST_GATE_VALIDATION_COMPLETE',
        'providers':len(seen),'target_records':len(records),'structurally_qualified':sum(r['qualification']=='STRUCTURAL_VERIFIED' for r in records),
        'compiled_source_id':identity,'collector_sha256':digest(Path(__file__).read_bytes()),'errors':errors,'vendor_code_executed':False,'runtime_tested':False,
        'full_dlls_included':False,'required_provider_hashes':sorted(required_hashes)})
    archive=package(out)
    print('RETURN='+str(archive));print('SHA256='+digest(archive.read_bytes()))
    return 2 if errors else 0

def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--corpus-root',type=Path)
    parser.add_argument('--provider',type=Path,action='append',default=[])
    parser.add_argument('--require-provider',type=Path,action='append',default=[])
    args=parser.parse_args()
    paths=list(args.provider)+list(args.require_provider)
    if args.corpus_root:paths+=sorted(args.corpus_root.rglob('nvngx_dlssg.dll'))
    paths=sorted({p.resolve() for p in paths})
    if not paths or len(paths)>256:parser.error('need 1..256 provider inputs')
    if not args.probe.is_file():parser.error('missing production probe')
    return collect(args.probe.resolve(),paths,{p.resolve() for p in args.require_provider},args.out.resolve())
if __name__=='__main__':
    raise SystemExit(main())
