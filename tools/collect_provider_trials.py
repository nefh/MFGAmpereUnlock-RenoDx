#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only local provider trials. Reports contain no complete DLL/mapped image.

Reuses compatibility_corpus.PEView and the production C++ preparation probe.
Each DLL is processed sequentially in a separate child; no vendor code executes.
Preset reports may contain bounded code excerpts for diagnosing an unresolved reader.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import zipfile
import compatibility_corpus as c



def preset_source_id(root: Path | None = None) -> str:
    root = root or Path(__file__).resolve().parents[1]
    paths = list((root / 'src/addons/mfgunlock').rglob('*.hpp'))
    for ext in ('*.h', '*.hpp'):
        paths.extend((root / 'contrib/validation/tests/provider_mocks').rglob(ext))
    paths.extend([root / 'contrib/validation/tools/probe_fg_preset.cpp',
                  root / 'contrib/validation/tests/fg_preset_mocks/ngx_hook.hpp'])
    lines = ''.join(f'{p.relative_to(root).as_posix()}|{hashlib.sha256(p.read_bytes()).hexdigest()}\n'
                    for p in sorted(set(paths), key=lambda p: p.as_posix()))
    return hashlib.sha256(lines.encode('utf-8')).hexdigest()


def preset_trial(path: Path, probe: Path) -> dict:
    """Inspect only the current preset reader; no provider preparation or DLL execution."""
    from collect_mfg_gate_evidence import map_pe
    raw = c.read_file(path)
    before = hashlib.sha256(raw).hexdigest()
    image, metadata = map_pe(raw)
    observations = []
    for _ in range(2):
        run = subprocess.run([str(probe)], input=image, capture_output=True, timeout=90, check=False)
        if run.returncode or len(run.stdout) > 4 * 1024 * 1024:
            raise RuntimeError(f'preset probe failed {run.returncode}: ' + run.stderr.decode(errors='replace')[-2048:])
        observation = json.loads(run.stdout)
        if observation.get('compiled_source_id') != preset_source_id():
            raise ValueError('stale preset probe: compiled source identity mismatch')
        if observation.get('vendor_code_executed') is not False or observation.get('input_unchanged') is not True:
            raise ValueError('preset inspection violated read-only invariant')
        observations.append(observation)
    if observations[0] != observations[1]:
        raise ValueError('non-deterministic preset inspection')
    if hashlib.sha256(c.read_file(path)).hexdigest() != before:
        raise ValueError('provider changed during read-only inspection')
    return {'path': str(path), 'sha256': before, 'file_bytes': len(raw), 'metadata': metadata,
            'preset': observations[0], 'deterministic_two_processes': True}

def trial(path: Path, probe: Path, architecture: str = 'Ampere') -> dict:
    data=c.read_file(path)
    result={'path':str(path), 'sha256':hashlib.sha256(data).hexdigest(), 'file_bytes':len(data)}
    pe=c.PEView(data)
    result.update(version=pe.versions(), pe_timestamp=pe.timestamp, size_of_image=pe.size,
                  identity_token=f'{pe.timestamp:08x}{pe.size:08x}', sections=pe.sections)
    if pe.size > 128*1024*1024:
        raise ValueError('mapped image budget exceeded')
    exports=pe.exports()
    own={e['name']:e['rva'] for e in exports if e['kind']=='code'}
    names=('NVSDK_NGX_GetGPUArchitecture','NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl')
    result['exports']=exports
    if not all(n in own for n in names):
        result['trial']={'trial_prepared':False,'failure_reason':'required own-code architecture/parameter exports absent'}
        return result
    image=bytearray(pe.size)
    image[:pe.headers]=data[:pe.headers]
    for section in pe.sections:
        n=section['raw_size']; start=section['rva']; offset=section['raw_offset']
        if start+n>len(image): raise ValueError('mapped section outside image')
        image[start:start+n]=data[offset:offset+n]
    # Relocations are not applied and no original image address is dereferenced.
    # This probe covers Ampere base PTX/architecture and MFG code contexts, not
    # optional pointer-descriptor/endpoint redirection or actual CUDA loading.
    result['export_code_windows']={name:bytes(image[rva:min(rva+64,len(image))]).hex()
                                   for name,rva in own.items() if name in names}
    command=[str(probe),architecture,*[str(own[n]) for n in names]]
    outputs=[]
    for _ in range(2):
        run=subprocess.run(command,input=image,capture_output=True,timeout=90,check=False)
        if run.returncode or len(run.stdout)>1024*1024:
            raise RuntimeError(f'probe failed {run.returncode}: '+run.stderr.decode(errors='replace')[-2048:])
        outputs.append(json.loads(run.stdout))
    if outputs[0]!=outputs[1]: raise ValueError('non-deterministic two-process result')
    result['trial']=outputs[0];result['deterministic_two_processes']=True
    if hashlib.sha256(c.read_file(path)).hexdigest()!=result['sha256']:
        raise ValueError('input file changed during read-only trial')
    return result


def main() -> int:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--probe',type=Path)
    ap.add_argument('--preset-probe',type=Path,help='read-only production preset resolver')
    ap.add_argument('--preset-only',action='store_true',help='inspect preset readers instead of base preparation')
    ap.add_argument('--corpus-root',type=Path)
    ap.add_argument('--provider',type=Path,action='append',default=[])
    ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args()
    if args.preset_only:
        if not args.preset_probe or not args.preset_probe.is_file(): ap.error('--preset-only requires --preset-probe')
    elif not args.probe or not args.probe.is_file(): ap.error('base preparation requires --probe')
    if args.preset_probe and not args.preset_probe.is_file(): ap.error('missing preset probe')
    if args.out.exists() and any(args.out.iterdir()):ap.error('output directory must be empty/new')
    args.out.mkdir(parents=True,exist_ok=True)
    paths=list(args.provider)
    if args.corpus_root:paths+=sorted((args.corpus_root/'dlss_g').rglob('nvngx_dlssg.dll'))
    paths=list(dict.fromkeys(p.resolve() for p in paths))
    if not paths: ap.error('no provider inputs found')
    rows=[]; errors=[]
    for i,path in enumerate(paths,1):
        try:
            if args.preset_only:
                row=preset_trial(path,args.preset_probe.resolve())
            else:
                row=trial(path,args.probe.resolve())
                if args.preset_probe:
                    row['preset']=preset_trial(path,args.preset_probe.resolve())['preset']
            if args.preset_only:
                print(f'[{i}/{len(paths)}] reader_resolved={row["preset"]["reader_resolved"]} (NOT GPU): {path}',flush=True)
            else:
                print(f'[{i}/{len(paths)}] prepared={row["trial"]["trial_prepared"]}: {path}',flush=True)
        except (OSError,ValueError,RuntimeError,subprocess.SubprocessError) as e:
            row={'path':str(path),'test_error':str(e)}; errors.append(row)
            print(f'[{i}/{len(paths)}] TEST_ERROR: {path}: {e}',flush=True)
        rows.append(row)
    report={'schema':'mfg_provider_trials_r4','scope':'ACTUAL_PRODUCTION_BASE_PREPARATION_WITH_OS_DOUBLES',
            'runtime_gpu_tested':False,'dll_files_included':False,'inputs':len(rows),
            'prepared_trials':sum(r.get('trial',{}).get('trial_prepared',False) for r in rows),
            'test_errors':errors,'libraries':rows}
    report['probe_sha256']=hashlib.sha256(args.probe.read_bytes()).hexdigest() if args.probe else None
    if args.preset_probe:
        report['preset_probe_sha256']=hashlib.sha256(args.preset_probe.read_bytes()).hexdigest()
        report['preset_compiled_source_id']=preset_source_id()
        report['preset_readers_resolved']=sum(r.get('preset',{}).get('reader_resolved',False) for r in rows)
    if args.preset_only:
        report['schema']='mfg_preset_trials_r6_3'
        report['scope']='OFFLINE_PRESET_READER_INSPECTION_ONLY_NOT_GPU'
        report['base_preparation_tested']=False
    report['tool_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    report['probe_source_sha256']=(hashlib.sha256((Path(__file__).resolve().parents[1]/
        'contrib/validation/tools/probe_provider_trial.cpp').read_bytes()).hexdigest()
        if not args.preset_only else None)
    report['source_sha256']={str(p.relative_to(Path(__file__).resolve().parents[1])):hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted((Path(__file__).resolve().parents[1]/'src/addons/mfgunlock').glob('*')) if p.is_file()}
    output=args.out/'provider_trials.json'
    output.write_text(json.dumps(report,indent=2),encoding='utf-8')
    sums=args.out/'SHA256SUMS.txt'
    sums.write_text(hashlib.sha256(output.read_bytes()).hexdigest()+'  provider_trials.json\n',encoding='ascii')
    archive=args.out/'provider_trials_EVIDENCE_ONLY.zip'
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
        for path in (output,sums):z.write(path,path.name)
    print('EVIDENCE_ONLY_ZIP='+str(archive.resolve()))
    return 1 if errors else 0

if __name__=='__main__': raise SystemExit(main())
