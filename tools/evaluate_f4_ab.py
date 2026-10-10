#!/usr/bin/env python3
"""Evaluate explicitly attributed whole-generation-group A/B measurements.

Input values must come from a profiler/capture, NOT Present interval, one kernel,
GetState counts, or requested MFG factor. No GPU data is manufactured here. Even
complete positive evidence is only ELIGIBLE_FOR_MANUAL_REVIEW, never a release.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import sys


def number(x, name, positive=False):
    if isinstance(x, bool) or not isinstance(x, (float, int)) or not math.isfinite(x) or x < 0 or (positive and x == 0):
        raise ValueError('invalid measurement: ' + name)
    return float(x)


def quantile(values, percent):
    a = sorted(values)
    return a[max(0, math.ceil(len(a) * percent / 100) - 1)]


def artifact(root: Path, item: dict) -> bool:
    if not isinstance(item, dict) or not item.get('path') or not item.get('sha256'):
        return False
    p = (root / item['path']).resolve()
    if not p.is_relative_to(root.resolve()) or not p.is_file() or p.stat().st_size > 2 * 1024**3:
        return False
    with p.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest() == item['sha256'].lower()


def evaluate(doc: dict, root: Path) -> dict:
    if doc.get('schema') != 'mfg_f4_ab_v1':
        raise ValueError('unknown AB schema')
    runs = doc.get('runs', [])
    pending = []
    rejected = []
    result = {'schema': 'mfg_f4_ab_result_v1', 'candidate': doc.get('candidate'),
              'runtime_default': 'OFF', 'release_qualified': False,
              'scope': 'Operator-attributed profiler/capture measurements; tool does not perform GPU execution.',
              'whole_group_gpu_cost': 'sum of all attributed group GPU operation durations, not elapsed frame latency'}
    if not runs:
        result.update(status='NOT_RUN', missing=['real paired A/B runs and frame captures'])
        return result
    if len(runs) > 64:
        raise ValueError('too many runs')
    samples = {'A': [], 'B': []}
    rates = {'A': [], 'B': []}
    own = {'A': [], 'B': []}
    delivered = {'A': [], 'B': []}
    costs = {'A': {}, 'B': {}}
    pair_map = {}
    signatures = {'A': set(), 'B': set()}
    contexts = set()
    after_recreate = {'A': False, 'B': False}
    order = []
    context_fields = ('game_exe_sha256','provider_sha256','streamline_identity','gpu','driver','api','output','settings_id','capture_method')
    for run in runs:
        phase = run.get('phase')
        if phase not in samples:
            raise ValueError('phase must be A or B')
        context = run.get('context', {})
        if any(not context.get(k) for k in context_fields):
            raise ValueError('incomplete fixed A/B context')
        contexts.add(json.dumps(context, sort_keys=True))
        identity = run.get('addon_sha256', '')
        if len(identity) != 64 or any(c not in '0123456789abcdef' for c in identity.lower()):
            raise ValueError('invalid addon hash')
        signatures[phase].add(identity.lower())
        if run.get('coverage') != 'ALL_DLSSG_GPU_OPERATIONS_PER_SOURCE_FRAME':
            pending.append('incomplete GPU group coverage')
        if not artifact(root, run.get('trace', {})):
            pending.append('missing or altered profiler trace')
        if run.get('warmup_excluded') is not True:
            pending.append('warmup was not explicitly excluded')
        pair = str(run.get('pair', ''))
        if not pair or phase in pair_map.setdefault(pair, set()):
            raise ValueError('duplicate/missing pair identity')
        pair_map[pair].add(phase)
        after_recreate[phase] |= run.get('after_recreate') is True
        order.append(phase)
        groups = run.get('groups', [])
        if not groups or len(groups) > 100000:
            raise ValueError('empty or oversized source-group set')
        seen = set()
        for group in groups:
            sid = group.get('source_frame_id')
            if not isinstance(sid, int) or isinstance(sid, bool) or sid < 0 or sid in seen:
                raise ValueError('missing or duplicate source-frame attribution')
            seen.add(sid)
            count = group.get('generated_count')
            if type(count) is not int or count < 1:
                raise ValueError('unknown generated group size')
            gpu = number(group.get('gpu_active_sum_ns'), 'whole group GPU', True)
            interval = number(group.get('source_interval_ns'), 'source interval', True)
            samples[phase].append(gpu)
            rates[phase].append(interval)
            costs[phase].setdefault(count, []).append(gpu)
            if group.get('own_cpu_ns') is not None:
                own[phase].append(number(group['own_cpu_ns'], 'own CPU'))
        if run.get('delivered_rate_source') != 'OBSERVED_OUTPUT_CAPTURE':
            pending.append('delivered cadence not independently observed')
        else:
            delivered[phase].append(number(run.get('delivered_hz'), 'observed delivered cadence', True))
    if len(contexts) != 1:
        rejected.append('A/B changed game/provider/API/settings/capture context')
    if any(len(v) != 1 for v in signatures.values()):
        rejected.append('each A/B arm must use one exact addon identity')
    if signatures['A'] == signatures['B']:
        rejected.append('baseline and source-patch candidate hashes are identical')
    if len(pair_map) < 2 or any(v != {'A', 'B'} for v in pair_map.values()):
        pending.append('need at least two complete interleaved pairs')
    if order not in (list('ABBA'), list('BAAB')) and sum(a != b for a, b in zip(order, order[1:])) < 2:
        pending.append('need interleaved runs, not one A block then one B block')
    if not all(after_recreate.values()):
        pending.append('missing post-recreate run for an arm')
    if any(len(v) < 32 for v in samples.values()):
        pending.append('need at least 32 measured groups per arm')
    if set(costs['A']) != set(costs['B']):
        pending.append('MFG group-count coverage differs between arms')
    stats = {}
    for phase in ('A', 'B'):
        if not samples[phase]:
            pending.append('missing arm ' + phase)
            continue
        stats[phase] = {'groups': len(samples[phase]),
                        'gpu_group_ns': {f'p{q}': quantile(samples[phase], q) for q in (50, 95, 99)},
                        'base_source_hz': 1e9 / statistics.mean(rates[phase]),
                        'delivered_hz_median': statistics.median(delivered[phase]) if delivered[phase] else None,
                        'gpu_by_generated_count': {str(k): {'groups':len(v), 'p50_ns':quantile(v,50), 'p95_ns':quantile(v,95)} for k,v in costs[phase].items()},
                        'own_cpu_p50_ns': quantile(own[phase],50) if own[phase] else None}
    review = doc.get('quality_review', {})
    if review.get('result') == 'FAIL':
        rejected.append('visual/temporal/UI regression reported')
    elif review.get('result') != 'PASS':
        pending.append('quality review NOT_RUN')
    if review.get('generated_and_rendered_frames_captured') is not True or not artifact(root, review.get('capture', {})):
        pending.append('missing verified generated/rendered capture artifact')
    if not review.get('method') or not review.get('scenes'):
        pending.append('quality review lacks method/scenes')
    # These are predeclared experiment criteria, not universal significance tests.
    thresholds = doc.get('thresholds', {})
    minimum = number(thresholds.get('min_median_improvement_percent', 3), 'minimum improvement')
    maximum = number(thresholds.get('max_p95_regression_percent', 2), 'maximum regression')
    metric = doc.get('benefit_metric', 'whole_group_gpu')
    improvement = None
    if all(p in stats for p in ('A','B')):
        # Both CPU and GPU comparisons need the same Dynamic group-count mix.
        if any(len(costs['A'].get(k,[])) != len(costs['B'].get(k,[])) for k in set(costs['A']) | set(costs['B'])):
            pending.append('group-count sample weights differ; do not pool a Dynamic mixture')
        if metric == 'whole_group_gpu':
            for count in sorted(set(costs['A']) & set(costs['B'])):
                if quantile(costs['B'][count],95) > quantile(costs['A'][count],95)*(1+maximum/100):
                    rejected.append(f'p95 whole GPU group cost regressed for generated_count={count}')
            a,b=stats['A']['gpu_group_ns']['p50'],stats['B']['gpu_group_ns']['p50']
        elif metric == 'own_cpu':
            a,b=stats['A']['own_cpu_p50_ns'],stats['B']['own_cpu_p50_ns']
            if len(own['A']) != len(samples['A']) or len(own['B']) != len(samples['B']):
                pending.append('own CPU measurement missing from some groups')
            for count in sorted(set(costs['A']) & set(costs['B'])):
                if quantile(costs['B'][count],95) > quantile(costs['A'][count],95)*(1+maximum/100):
                    rejected.append('whole GPU group regressed while optimizing CPU')
        else:
            raise ValueError('unsupported benefit metric')
        if a is not None and b is not None and a > 0:
            improvement=100*(a-b)/a
            if improvement < minimum:
                rejected.append('no measured median benefit above the predeclared threshold')
        else:
            pending.append('benefit cannot be established from an absent or zero baseline')
        if delivered['A'] and delivered['B'] and statistics.median(delivered['B']) < statistics.median(delivered['A'])*(1-maximum/100):
            rejected.append('observed delivered cadence regressed')
        if stats['B']['base_source_hz'] < stats['A']['base_source_hz']*(1-maximum/100):
            rejected.append('base-source cadence regressed')
    result.update(status='REJECTED' if rejected else 'INCOMPLETE' if pending else 'ELIGIBLE_FOR_MANUAL_REVIEW',
                  benefit_metric=metric, measured_median_improvement_percent=improvement,
                  statistics=stats, rejected=sorted(set(rejected)), missing=sorted(set(pending)),
                  provenance='Trace/capture files hash-verified; semantic attribution is operator-supplied, not inferred by this tool.')
    return result


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--input', type=Path, required=True)
    ap.add_argument('--artifacts', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a=ap.parse_args()
    try:
        if a.input.stat().st_size > 32*1024*1024:
            raise ValueError('AB JSON exceeds 32 MiB')
        result=evaluate(json.loads(a.input.read_text(encoding='utf-8-sig')),a.artifacts)
        a.out.parent.mkdir(parents=True,exist_ok=True)
        with a.out.open('x',encoding='utf-8') as f: json.dump(result,f,indent=2,allow_nan=False)
        print('F4 AB STATUS='+result['status'])
        return 0 if result['status']=='ELIGIBLE_FOR_MANUAL_REVIEW' else 2
    except (ValueError,OSError,KeyError,TypeError) as e:
        print('F4 AB ERROR: '+str(e),file=sys.stderr);return 2
if __name__=='__main__':raise SystemExit(main())
