#!/usr/bin/env python3
"""Collect explicit runtime logs only; never load DLLs or launch a target.

An F3Vulkan record proves that the addon observed a Vulkan primary device and
native NGX call results. It does NOT prove GPU completion, displayed frames,
correct UI pixels, HDR luminance, or end-to-end latency.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import re
import sys
import zipfile
from pathlib import Path

MAX_LOG_BYTES = 32 * 1024 * 1024
MAX_TOTAL_BYTES = 64 * 1024 * 1024
MAX_EVENTS = 256

def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def parse_events(text: str) -> list[dict]:
    result = []
    for line in text.splitlines():
        if 'F3Vulkan ' not in line:
            continue
        values = dict(re.findall(r'(?:^|\s)([a-zA-Z0-9_]+)=([^\s]+)', line.split('F3Vulkan ', 1)[1]))
        if values.get('api') != 'Vulkan':
            continue
        result.append(values)
        if len(result) > MAX_EVENTS:
            result.pop(0)
    return result

def uint(row: dict, key: str) -> int:
    value = row.get(key, '')
    if not value.isdecimal() or len(value) > 20:
        return 0
    number = int(value)
    return number if number <= (1 << 64) - 1 else 0

def classify(events: list[dict]) -> dict:
    last = events[-1] if events else {}
    return {
        'primary_api_vulkan_observed': bool(events),
        'native_evaluate_success_observed': uint(last, 'evaluate_ok') > 0,
        'native_create_success_observed': uint(last, 'create_ok') > 0,
        'native_release_success_observed': uint(last, 'release_ok') > 0,
        'known_nonnull_images': sum(uint(last, f'r{i}_known') == 1 and uint(last, f'r{i}_image') > 0 for i in range(10)),
        'generation': uint(last, 'generation'),
        'last_cpu_snapshot_current': uint(last, 'snapshot_current') == 1,
        'gpu_completion': 'NOT_PROVEN_BY_NGX_RETURN',
        'scanout': 'UNKNOWN',
        'image_quality': 'NOT_ASSESSED',
        'hdr_luminance': 'NOT_ASSESSED',
        'runtime_qualified': False,
    }

def collect(logs: list[Path], output: Path, case: str, addons: list[Path] | None = None) -> Path:
    if not logs or len(logs) > 8:
        raise ValueError('supply 1..8 explicit --log paths')
    if output.exists():
        raise ValueError('output directory already exists; use a new case directory')
    if len(case) > 120:
        raise ValueError('case label too long')
    payloads: dict[str, bytes] = {}
    sources, events = [], []
    total = 0
    for index, raw in enumerate(logs):
        path = raw.resolve(strict=True)
        if not path.is_file() or path.suffix.lower() not in ('.log', '.txt'):
            raise ValueError('only explicit .log/.txt files are accepted')
        if path.stat().st_size > MAX_LOG_BYTES:
            raise ValueError('log exceeds 32 MiB limit: ' + path.name)
        with path.open('rb') as stream:
            data = stream.read(MAX_LOG_BYTES + 1)
        if len(data) > MAX_LOG_BYTES or data.startswith((b'MZ', b'\x7fELF', b'PK\x03\x04')):
            raise ValueError('not an accepted bounded text log: ' + path.name)
        total += len(data)
        if total > MAX_TOTAL_BYTES:
            raise ValueError('combined logs exceed 64 MiB')
        encoding = 'utf-16' if data.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig'
        text = data.decode(encoding, errors='replace')
        name = f'logs/log_{index:02d}.log'
        payloads[name] = data
        sources.append({'source': str(path), 'packed': name, 'bytes': len(data), 'sha256': digest(data)})
        events.extend(parse_events(text))
    events = events[-MAX_EVENTS:]
    addon_hashes = []
    for path in (addons or []):
        path = path.resolve(strict=True)
        if not path.is_file() or path.suffix.lower() != '.addon64' or path.stat().st_size > MAX_LOG_BYTES:
            raise ValueError('invalid --addon file (only its hash is recorded)')
        with path.open('rb') as stream:
            addon_hashes.append({'path': str(path), 'sha256': hashlib.file_digest(stream, 'sha256').hexdigest()})
    summary = {
        'schema': 'mfg_f3_runtime_evidence_v1', 'case': case,
        'status': 'LOGS_COLLECTED' if events else 'COLLECTED_NO_VULKAN_EVENT',
        'observation': classify(events), 'events': events, 'sources': sources,
        'addon_hashes': addon_hashes, 'vendor_dlls_included': False,
        'collector_executed_vendor_code': False, 'game_launched_by_collector': False,
        'scope': 'CPU return/descriptor observations only. Attach separate frame capture to prove GPU/output/UI.',
    }
    payloads['summary.json'] = (json.dumps(summary, indent=2, ensure_ascii=False) + '\n').encode('utf-8')
    payloads['SHA256SUMS.txt'] = ''.join(f'{digest(value)}  {name}\n' for name, value in sorted(payloads.items())).encode('ascii')
    output.mkdir(parents=True, exist_ok=False)
    archive = output / 'F3_RUNTIME_EVIDENCE_ONLY.zip'
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
        for name, value in sorted(payloads.items()):
            bundle.writestr(name, value)
    (output / 'summary.json').write_bytes(payloads['summary.json'])
    return archive

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', action='append', required=True, type=Path)
    parser.add_argument('--addon', action='append', type=Path, default=[])
    parser.add_argument('--case', required=True)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    try:
        archive = collect(args.log, args.out, args.case, args.addon)
        print('RETURN=' + str(archive))
        with archive.open('rb') as stream:
            print('SHA256=' + hashlib.file_digest(stream, 'sha256').hexdigest())
        print('COLLECTION_ONLY: no GPU/display/HDR qualification')
        return 0
    except (ValueError, OSError, zipfile.BadZipFile) as exc:
        print('F3 COLLECT FAIL: ' + str(exc), file=sys.stderr)
        return 2

if __name__ == '__main__':
    raise SystemExit(main())
