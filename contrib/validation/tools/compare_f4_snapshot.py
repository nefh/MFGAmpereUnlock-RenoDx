#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile and compare the unchanged production Read and isolated C1 in WSL/Linux.

All generated headers/executables/logs live in --out. No installed source, game,
provider or configuration is modified. Results are CPU microbenchmarks only.
"""
from pathlib import Path
import argparse, hashlib, json, platform, statistics, subprocess, sys


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--project',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--cxx',default='g++')
    a=ap.parse_args();p=a.project.resolve();out=a.out.resolve()
    if out.exists():raise ValueError('use a new output directory')
    if not out.is_relative_to((p/'dist').resolve()):raise ValueError('output must be under project/dist')
    test=p/'contrib/validation/tests';header=p/'src/addons/mfgunlock/presentation_observer.hpp'
    text=header.read_text(encoding='utf-8')
    reference=(test/'f4_passive_read_reference.hpp').read_text()
    reference=reference[reference.index('inline Snapshot ReferenceRead('):].replace('ReferenceRead(', 'Read(',1).rstrip()+'\n'
    replacement=(test/'f4_passive_read_candidate.hpp').read_text()
    replacement=replacement[replacement.index('inline Snapshot CandidateRead('):].replace('CandidateRead(', 'Read(',1).rstrip()+'\n'
    if text.count(reference)!=1:raise ValueError('production Read is not the expected F3 baseline; do not mix candidates')
    out.mkdir(parents=True)
    candidate=out/'candidate_presentation_observer.hpp';candidate.write_text(text.replace(reference,replacement))
    flags=['-std=c++20','-O2','-UNDEBUG','-Wall','-Wextra','-Wpedantic','-Werror','-pthread']
    compiler=subprocess.run([a.cxx,'--version'],capture_output=True,text=True,check=True).stdout
    commands=[]
    bench=p/'contrib/validation/tools/benchmark_f4_snapshot.cpp'
    for label,h in [('A',header),('B',candidate)]:
        cmd=[a.cxx,*flags,f'-DF4_READ_HEADER="{h}"',str(bench),'-o',str(out/label)]
        commands.append(cmd);subprocess.run(cmd,check=True,timeout=180)
    cmd=[a.cxx,*flags,f'-DF4_READ_HEADER="{candidate}"',str(test/'f4_passive_read_test.cpp'),'-o',str(out/'equivalence')]
    commands.append(cmd);subprocess.run(cmd,check=True,timeout=180)
    proof=subprocess.run([str(out/'equivalence')],capture_output=True,text=True,check=True,timeout=60)
    (out/'equivalence.log').write_text(proof.stdout+proof.stderr)
    runs=[]
    for i,label in enumerate('ABBA'):
        result=subprocess.run([str(out/label)],capture_output=True,text=True,check=True,timeout=120)
        row=json.loads(result.stdout);runs.append({'order':i,'phase':label,'result':row})
        (out/f'{i}_{label}.json').write_text(result.stdout)
    summary={}
    for mode in ('capture_off','capture_on_empty','capture_on_full'):
        values={phase:[v for r in runs if r['phase']==phase for v in r['result']['timings'][mode]['trials_ns']] for phase in ('A','B')}
        am,bm=statistics.median(values['A']),statistics.median(values['B'])
        summary[mode]={'A_median_ns':am,'B_median_ns':bm,'median_difference_ns':am-bm,'relative_difference_percent':100*(am-bm)/am}
    report={'schema':'f4_c1_microbench_v1','candidate':'C1_PASSIVE_READ','compiler':compiler,'platform':platform.platform(),
            'commands':commands,'baseline_header_sha256':hashlib.sha256(header.read_bytes()).hexdigest(),
            'candidate_header_sha256':hashlib.sha256(candidate.read_bytes()).hexdigest(),
            'equivalence_stdout':proof.stdout,'order':'ABBA','runs':runs,'summary':summary,
            'game_executed':False,'gpu_executed':False,'whole_generation_group_cost':'NOT_MEASURED',
            'candidate_default':'OFF','runtime_acceptance':'NOT_RUN'}
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(summary,indent=2));print('REPORT='+str(out/'result.json'))
    print('MICROBENCH_ONLY: C1 remains OFF until controlled game validation.')
    return 0

if __name__=='__main__':
    try:raise SystemExit(main())
    except (OSError,ValueError,subprocess.SubprocessError) as e:
        print('F4 C1 ERROR: '+str(e),file=sys.stderr);raise SystemExit(2)
