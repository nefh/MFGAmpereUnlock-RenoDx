#!/usr/bin/env python3
"""Offline analysis of MFGAmpereUnlock HUD_TRACE v1.

No external dependencies, no GPU interaction. The analyzer identifies per-frame
Streamline tag/option/timing anomalies; it cannot infer the visual contents of
HUD/UIAlpha pixels without a paired visual or GPU resource capture.
"""
import argparse
import collections
import csv
import json
import pathlib
import re
import sys

UNKNOWN = (1 << 64) - 1
ROLES = {1: 'HUDLessColor', 2: 'UIColorAndAlpha', 3: 'UIAlpha'}


def parse(path):
    events, meta, flushes = [], {}, []
    with open(path, 'r', encoding='utf-8-sig', newline='') as f:
        rows = []
        for line in f:
            if line.startswith('#'):
                if 'qpc_frequency=' in line:
                    m = re.search(r'qpc_frequency=(\d+)', line)
                    if m:
                        meta['qpc_frequency'] = int(m.group(1))
                if 'drop_lock=' in line:
                    m = re.search(r'drop_lock=(\d+),drop_full=(\d+)', line)
                    if m:
                        flushes.append((int(m.group(1)), int(m.group(2))))
                continue
            if line.strip():
                rows.append(line)
        if not rows:
            raise ValueError(f'No CSV payload found: {path}')
        for record in csv.DictReader(rows):
            try:
                events.append({
                    'seq': int(record['sequence']), 'qpc': int(record['qpc']),
                    'thread': int(record['thread']), 'kind': record['kind'],
                    'view': int(record['viewport']), 'frame': int(record['frame']),
                    'd': tuple(int(record[k]) for k in 'abcdefgh'),
                })
            except (ValueError, KeyError) as exc:
                raise ValueError(f'Malformed trace entry around {record!r}: {exc}') from exc
    events.sort(key=lambda e: e['seq'])
    return events, meta, flushes


def rate(num, den):
    return round(num / den, 4) if den else None


def analyze(path):
    events, meta, flushes = parse(path)
    kinds = collections.Counter(e['kind'] for e in events)
    roles = {'TAG_IN': collections.Counter(), 'TAG_OUT': collections.Counter()}
    resources = {'TAG_IN': collections.defaultdict(set),
                 'TAG_OUT': collections.defaultdict(set)}
    frames = collections.defaultdict(lambda: {'TAG_IN': collections.defaultdict(list),
                                               'TAG_OUT': collections.defaultdict(list)})
    present = []
    options = {'OPTIONS_GAME': [], 'OPTIONS_SENT': []}
    tag_errors = []
    unpaired = []
    invalid = []
    mask_transitions = []
    last_mask = None
    previous_present = None
    max_gap_ms = 0.0
    for e in events:
        kind, d = e['kind'], e['d']
        if kind in roles:
            role = d[0] >> 32
            label = ROLES.get(role, f'Unknown({role})')
            roles[kind][label] += 1
            resources[kind][label].add(d[1])
            if d[1] == 0 or (d[2] & 1) == 0 or (d[3] & 0xffffffff) == 0:
                invalid.append({'sequence': e['seq'], 'kind': kind, 'frame': e['frame'],
                                'viewport': e['view'], 'role': label,
                                'native_resource': d[1], 'extent_wh': d[3]})
            if e['frame'] != UNKNOWN:
                frames[(e['view'], e['frame'])][kind][label].append(d)
        elif kind == 'PRESENT':
            present.append(e)
            if last_mask is not None and d[1] != last_mask:
                mask_transitions.append({'sequence': e['seq'], 'qpc': e['qpc'],
                                         'old': last_mask, 'new': d[1]})
            last_mask = d[1]
            if previous_present is not None and meta.get('qpc_frequency'):
                gap_ms = (e['qpc'] - previous_present['qpc']) * 1000.0 / meta['qpc_frequency']
                max_gap_ms = max(max_gap_ms, gap_ms)
            previous_present = e
        elif kind in options:
            options[kind].append(e)
        elif kind == 'TAG_RESULT' and d[0] != 0:
            tag_errors.append({'sequence': e['seq'], 'result': d[0],
                               'viewport': e['view'], 'frame': e['frame']})
    missing_by_kind = {'TAG_IN': [], 'TAG_OUT': []}
    resource_mismatch = []
    spatial_mismatch = []
    for (view, frame), phases in frames.items():
        for kind, tag_map in phases.items():
            hud = tag_map.get('HUDLessColor', [])
            alpha = tag_map.get('UIAlpha', []) + tag_map.get('UIColorAndAlpha', [])
            if hud and not alpha:
                missing_by_kind[kind].append({'viewport': view, 'frame': frame})
            if hud and alpha and (hud[-1][3] != alpha[-1][3]):
                spatial_mismatch.append({'viewport': view, 'frame': frame, 'kind': kind,
                                         'hud_extent': hud[-1][3], 'ui_extent': alpha[-1][3]})
        source = phases['TAG_IN']
        forwarded = phases['TAG_OUT']
        for role in ROLES.values():
            sin = {v[1] for v in source.get(role, []) if v[1]}
            sout = {v[1] for v in forwarded.get(role, []) if v[1]}
            if sin and sout and sin != sout:
                resource_mismatch.append({'viewport': view, 'frame': frame, 'role': role,
                                          'input': sorted(sin), 'forwarded': sorted(sout)})
    unique_view_frames = len(frames)
    result = {
        'path': str(path),
        'qpc_frequency': meta.get('qpc_frequency'),
        'event_count': len(events),
        'counts': dict(kinds),
        'present_count': len(present),
        'largest_present_gap_ms': round(max_gap_ms, 3),
        'trace_loss': {'lock_contention': flushes[-1][0] if flushes else None,
                       'ring_overflow': flushes[-1][1] if flushes else None},
        'frames_with_explicit_frame_token': unique_view_frames,
        'tag_counts': {k: dict(v) for k, v in roles.items()},
        'unique_resource_counts': {k: {role: len(rs) for role,rs in value.items()}
                                   for k, value in resources.items()},
        'tag_failures': {'count': len(tag_errors), 'examples': tag_errors[:20]},
        'invalid_tag_resource_or_extent': {'count': len(invalid), 'examples': invalid[:20]},
        'hudless_without_matching_ui_in_same_frame_token': {
            k: {'count': len(items), 'examples': items[:25]}
            for k,items in missing_by_kind.items()},
        'tag_resource_identity_mismatch': {'count': len(resource_mismatch),
                                            'examples': resource_mismatch[:25]},
        'native_pair_extent_mismatch': {'count': len(spatial_mismatch),
                                        'examples': spatial_mismatch[:25]},
        'present_mask_transitions': {'count': len(mask_transitions),
                                     'examples': mask_transitions[:30]},
        'options': {k: [{'seq': x['seq'], 'viewport': x['view'],
                         'mode': x['d'][0], 'generated': x['d'][1],
                         'uir': x['d'][2], 'ui_format': x['d'][3],
                         'abi': x['d'][4], 'result': x['d'][5],
                         'flags': x['d'][6]}
                        for x in val[:100]] for k,val in options.items()},
    }
    return result


def print_report(r):
    print(f"HUD_TRACE: {r['path']}")
    print(f"events={r['event_count']}; presents={r['present_count']}; "
          f"explicit_token_frames={r['frames_with_explicit_frame_token']}; "
          f"largest_present_gap_ms={r['largest_present_gap_ms']}")
    for kind, counts in r['tag_counts'].items():
        print(kind, ' '.join(f'{name}={n}' for name, n in sorted(counts.items())))
    for name in ['trace_loss', 'tag_failures', 'invalid_tag_resource_or_extent',
                 'hudless_without_matching_ui_in_same_frame_token',
                 'tag_resource_identity_mismatch', 'native_pair_extent_mismatch',
                 'present_mask_transitions']:
        info = r[name]
        if name == 'hudless_without_matching_ui_in_same_frame_token':
            print(name, {k: v['count'] for k,v in info.items()})
        elif name == 'trace_loss':
            print(name, info)
        else:
            print(name, info['count'])
    print('Note: missing-per-token findings cover only frame-scoped tags; global tags may')
    print('      be reused validly across frames. Metadata does not reveal UIAlpha pixels.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('traces', type=pathlib.Path, nargs='+',
                        help='one or more MFGAmpereUnlock.hudtrace.<pid>.csv files')
    parser.add_argument('--out', type=pathlib.Path,
                        help='optional combined analysis JSON destination')
    ns = parser.parse_args()
    reports = [analyze(p) for p in ns.traces]
    for r in reports:
        print_report(r)
        print()
    if len(reports) == 2:
        a, b = reports
        print('COMPARISON (first trace A, second trace B):')
        for key in ['present_count', 'frames_with_explicit_frame_token',
                    'largest_present_gap_ms']:
            print(f'{key}: A={a[key]} B={b[key]}')
        for key in ['tag_failures', 'invalid_tag_resource_or_extent',
                    'native_pair_extent_mismatch', 'tag_resource_identity_mismatch']:
            print(f'{key}: A={a[key]["count"]} B={b[key]["count"]}')
    if ns.out:
        ns.out.parent.mkdir(parents=True, exist_ok=True)
        ns.out.write_text(json.dumps(reports, indent=2), encoding='utf-8')
        print('ANALYSIS_JSON=' + str(ns.out))

if __name__ == '__main__':
    main()
