#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Production SM75/SM86 preparation + optional real ptxas, without vendor execution.

A failed/unsupported provider is recorded separately from a tool failure. All
NVIDIA bytes and emitted PTX/cubins stay local. Return ZIP: reports and logs only.
One worker per input; native probe and ptxas run sequentially with timeouts.
No implicit builds, downloads, WSL, game-library changes or GPU calls.
"""
from __future__ import annotations
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import os
import struct
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
MAX_LOG = 1024 * 1024
REFERENCE = 'ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82'


def sha(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for part in iter(lambda: stream.read(1024*1024), b''):
            h.update(part)
    return h.hexdigest()


def source_id(root: Path = ROOT) -> str:
    paths = set((root/'src/addons/mfgunlock').rglob('*.hpp'))
    for suffix in ('*.hpp', '*.h'):
        paths.update((root/'contrib/validation/tests/provider_mocks').rglob(suffix))
    paths.update(root/'contrib/validation/tools'/n for n in
                 ('probe_provider_capability.cpp', 'mapped_provider_image.hpp'))
    text = ''.join(p.relative_to(root).as_posix()+'|'+sha(p)+'\n' for p in sorted(paths))
    return hashlib.sha256(text.encode()).hexdigest()


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=True)+'\n', encoding='utf-8')


def elf_target(path: Path, sm: int) -> bool:
    if not path.is_file(): return False
    with path.open('rb') as f: h=f.read(64)
    return (len(h)==64 and h[:6]==b'\x7fELF\x02\x01' and
            struct.unpack_from('<H',h,18)[0]==190 and (struct.unpack_from('<I',h,48)[0]&255)==sm)


def process(command: list[str], log: Path, *, stdin: bytes | None = None, timeout: int = 120) -> int:
    with log.open('wb') as output:
        try:
            code=subprocess.run(command,input=stdin,stdout=output,stderr=subprocess.STDOUT,
                                check=False,timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            output.write(b'\nTIMEOUT\n'); code=124
    if log.stat().st_size>MAX_LOG:
        with log.open('rb') as f:
            first=f.read(MAX_LOG//2);f.seek(-MAX_LOG//2,2);last=f.read()
        log.write_bytes(first+b'\n[LOG TRUNCATED]\n'+last)
    return code


def comparable(result: dict) -> dict:
    # Heap addresses vary between processes. Keep the actual changed-byte count
    # in evidence, but compare the number of pointer cells and all non-address
    # bytes instead. No other production result is normalized away.
    omitted = {'ptx_files'}
    if (result.get('address_cells', 0) > 0 and
            'changed_non_address_bytes' in result):
        omitted.add('changed_bytes')
    return {key: value for key, value in result.items() if key not in omitted}


def trial(path: Path, args, dest: Path) -> dict:
    import compatibility_corpus as c
    data=c.read_file(path); identity=hashlib.sha256(data).hexdigest()
    pe=c.PEView(data)
    if pe.size>128*1024*1024: raise ValueError('mapped image budget')
    image=bytearray(pe.size);image[:pe.headers]=data[:pe.headers]
    for section in pe.sections:
        n=section['raw_size'];rva=section['rva'];at=section['raw_offset']
        if rva+n>len(image) or at+n>len(data):raise ValueError('section bounds')
        image[rva:rva+n]=data[at:at+n]
    row={'provider_sha256':identity,'version':pe.versions(),'targets':{},'input_modified':False,
         'vendor_executed':False,'gpu_execution':'NOT_RUN','game_runtime':'NOT_RUN'}
    for target,sm in (('Turing',75),('Ampere',86)):
        out=dest/f'sm{sm}';out.mkdir()
        log=out/'probe.json';ptx_dir=out/'local_ptx'
        code=process([str(args.probe),'--provider',target,'--emit-ptx',str(ptx_dir)],log,stdin=image)
        if code:raise RuntimeError(f'{target} probe failed {code}: '+log.read_text(errors='replace')[-2048:])
        result=json.loads(log.read_text())
        if result.get('compiled_source_id')!=source_id():raise RuntimeError('stale probe: compiled/current source mismatch')
        repeat_log=out/'repeat.json'
        code=process([str(args.probe),'--provider',target],repeat_log,stdin=image)
        if code:raise RuntimeError(f'{target} repeat failed {code}')
        repeat=json.loads(repeat_log.read_text())
        if comparable(result)!=comparable(repeat):raise RuntimeError('non-deterministic production preparation')
        if not result['idempotent'] or not result['rollback_identical']:raise RuntimeError('transaction invariant failed')
        result['deterministic_two_processes']=True
        result['assembly']=[]
        for i,name in enumerate(result['ptx_files']):
            if Path(name).name!=name or not name.endswith('.ptx'):raise ValueError('unsafe exported PTX filename')
            text=ptx_dir/name
            item={'name':name,'ptx_sha256':sha(text),'target_sm':sm,'status':'NOT_RUN'}
            if args.ptxas:
                binary=out/f'local_{i:04d}.cubin';asm_log=out/f'ptxas_{i:04d}.log'
                rc=process([str(args.ptxas),'-arch=sm_'+str(sm),'-O3','-o',str(binary),str(text)],asm_log)
                item.update(exit_code=rc,elf_target=elf_target(binary,sm))
                item['status']='ASSEMBLED' if rc==0 and item['elf_target'] else 'ASSEMBLY_FAILED'
                if binary.exists():item['cubin_sha256']=sha(binary)
                print(f'    sm{sm} ptxas {i+1}/{len(result["ptx_files"])} {item["status"]}',flush=True)
            result['assembly'].append(item)
        result['assembly_status']=('NOT_PREPARED' if not result['prepared'] else
            'NOT_RUN' if not args.ptxas else 'NO_PTX_EXPORTED' if not result['assembly'] else
            'ASSEMBLED' if all(x['status']=='ASSEMBLED' for x in result['assembly']) else 'ASSEMBLY_FAILED')
        row['targets'][target]=result
    if sha(path)!=identity:raise ValueError('input changed during trial')
    return row


def inputs(args) -> list[Path]:
    if args.provider:return [p.resolve() for p in args.provider]
    root=args.corpus_root.resolve()
    manifest=json.loads((root/'dlss_g/compatibility_library_manifest.json').read_text(encoding='utf-8-sig'))
    if not isinstance(manifest,list) or not manifest:
        raise ValueError('empty/invalid provider manifest')
    result=[]
    for row in manifest:
        rel=row['relative_path'].replace('\\','/');path=(root/rel).resolve()
        if not rel.startswith('dlss_g/') or not path.is_relative_to(root) or path in result:
            raise ValueError('unsafe/duplicate corpus entry')
        if path.stat().st_size!=row['bytes'] or sha(path)!=row['sha256'].lower():
            raise ValueError('corpus_integrity_mismatch:'+rel)
        result.append(path)
    actual={p.resolve() for p in (root/'dlss_g').rglob('*') if p.is_file() and p.suffix.lower()=='.dll'}
    if set(result)!=actual:raise ValueError('undeclared/missing corpus DLL')
    return result


def main() -> int:
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--probe',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--corpus-root',type=Path);p.add_argument('--provider',type=Path,action='append')
    p.add_argument('--ptxas',type=Path);p.add_argument('--worker',type=Path,help=argparse.SUPPRESS)
    args=p.parse_args();args.probe=args.probe.resolve();args.out=args.out.resolve()
    if args.ptxas:args.ptxas=args.ptxas.resolve()
    if args.worker:
        try:
            result=trial(args.worker,args,args.out);write_json(args.out/'result.json',result);return 0
        except Exception as ex:
            write_json(args.out/'result.json',{'tool_error':str(ex)});return 2
    if not args.corpus_root and not args.provider:p.error('supply --corpus-root or --provider')
    if args.out.exists() and any(args.out.iterdir()):p.error('use a new output directory')
    args.out.mkdir(parents=True,exist_ok=True)
    rows=[];code=0
    try:
        if not args.probe.is_file():raise ValueError('probe missing')
        if args.ptxas and not args.ptxas.is_file():raise ValueError('ptxas missing; no compiler substituted')
        paths=inputs(args)
        if args.ptxas:
            process([str(args.ptxas),'--version'],args.out/'ptxas-version.log',timeout=15)
        for i,path in enumerate(paths):
            dest=args.out/f'provider_{i:03d}';dest.mkdir()
            print(f'[{i+1}/{len(paths)}] {path}',flush=True)
            cmd=[sys.executable,str(Path(__file__).resolve()),'--worker',str(path),'--probe',str(args.probe),'--out',str(dest)]
            if args.ptxas:cmd+=['--ptxas',str(args.ptxas)]
            # Inherit stdout: every assembler operation remains visible.
            try:rc=subprocess.run(cmd,timeout=1800,check=False).returncode
            except subprocess.TimeoutExpired:rc=124
            file=dest/'result.json'
            row=json.loads(file.read_text()) if file.exists() else {'tool_error':f'worker exit {rc}'}
            row['path']=str(path);rows.append(row)
            if rc or 'tool_error' in row:code=2
            else:
                print('  '+ ' '.join(f'{t}: prepared={v["prepared"]} endpoint={v["endpoint_selected"]} assembly={v["assembly_status"]}' for t,v in row['targets'].items()),flush=True)
                if any(v['assembly_status']=='ASSEMBLY_FAILED' for v in row['targets'].values()):code=2
            write_json(args.out/'results.json',rows)
    except Exception as ex:
        code=2;write_json(args.out/'error.json',{'error':str(ex)})
    counts={t:dict(Counter(('TOOL_ERROR' if 'tool_error' in r else
                 r['targets'][t]['assembly_status']) for r in rows)) for t in ('Turing','Ampere')}
    reference=[r for r in rows if r.get('provider_sha256')==REFERENCE]
    if reference:
        r=reference[0]['targets']['Turing']
        if not r['prepared'] or not r['endpoint_selected'] or r['endpoint_payloads']!=39:code=2
    summary={'schema':'sm75_structural_t2_validation','status':'COLLECTED' if not code else 'VALIDATION_ERROR',
             'providers':len(rows),'target_results':counts,'prepared':{t:sum(r.get('targets',{}).get(t,{}).get('prepared',False) for r in rows) for t in ('Turing','Ampere')},
             'source_id':source_id(),'probe_sha256':sha(args.probe) if args.probe.is_file() else None,'tool_sha256':sha(Path(__file__)),
             'kernel_launches':0,'runtime_tested':False,'reference_tested':bool(reference)}
    write_json(args.out/'summary.json',summary)
    evidence=[p for p in args.out.rglob('*') if p.is_file() and p.suffix in ('.json','.log')]
    manifest=args.out/'SHA256SUMS.txt'
    manifest.write_text(''.join(sha(p)+'  '+p.relative_to(args.out).as_posix()+'\n' for p in sorted(evidence)),encoding='ascii')
    archive=args.out/'SM75_T2_VALIDATION_NO_DLL.zip'
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
        for file in evidence+[manifest]:z.write(file,file.relative_to(args.out).as_posix())
    print(json.dumps(summary,indent=2));print('RETURN='+str(archive));return code


if __name__=='__main__':raise SystemExit(main())
