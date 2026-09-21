#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only PE/DLSS-G qualifier. It never loads, executes, or rewrites a DLL.
The packaged JSON is generated from provider_profile.hpp; CTest verifies parity.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

from probe_validated_warp import fnv1a64, lz4_decompress

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / 'tools' / 'data' / 'provider_profiles.json'
MAX_FILE = 64 * 1024 * 1024
MAX_PAYLOAD = 4 * 1024 * 1024
MAGIC = b'\x50\xed\x55\xba'


class InvalidImage(ValueError):
    pass


class Image:
    def __init__(self, data: bytes):
        self.data = data
        if not 256 <= len(data) <= MAX_FILE:
            raise InvalidImage('file_size_out_of_range')
        if self.take(0, 2) != b'MZ':
            raise InvalidImage('missing_dos_signature')
        nt = self.u32(0x3c)
        if self.take(nt, 4) != b'PE\0\0' or self.u16(nt + 4) != 0x8664:
            raise InvalidImage('not_pe64_x86_64')
        count, opt_size = self.u16(nt + 6), self.u16(nt + 20)
        opt = nt + 24
        if not 1 <= count <= 96 or opt_size < 112 or self.u16(opt) != 0x20b:
            raise InvalidImage('invalid_optional_header')
        self.take(opt, opt_size)
        self.timestamp = self.u32(nt + 8)
        self.base = self.u64(opt + 24)
        self.size = self.u32(opt + 56)
        self.headers = self.u32(opt + 60)
        if not self.headers <= self.size <= MAX_FILE or self.headers > len(data):
            raise InvalidImage('invalid_image_dimensions')
        table = opt + opt_size
        if table + count * 40 > self.headers:
            raise InvalidImage('section_table_outside_headers')
        self.sections = []
        for i in range(count):
            at = table + i * 40
            name = self.take(at, 8).split(b'\0')[0].decode('ascii', 'replace')
            virtual, rva, raw_size, raw = struct.unpack('<IIII', self.take(at + 8, 16))
            flags = self.u32(at + 36)
            if rva < self.headers or rva + max(virtual, raw_size) > self.size:
                raise InvalidImage('section_outside_image')
            self.take(raw, raw_size)
            self.sections.append(dict(name=name, rva=rva, virtual_size=virtual,
                                      raw_offset=raw, raw_size=raw_size, flags=flags))
        for a, b in zip(sorted(self.sections, key=lambda x: x['rva']),
                        sorted(self.sections, key=lambda x: x['rva'])[1:]):
            if a['rva'] + max(a['virtual_size'], a['raw_size']) > b['rva']:
                raise InvalidImage('overlapping_virtual_sections')
        raw_sections = sorted((s for s in self.sections if s['raw_size']), key=lambda x: x['raw_offset'])
        for i, s in enumerate(raw_sections):
            if s['raw_offset'] < self.headers or (i and raw_sections[i-1]['raw_offset'] + raw_sections[i-1]['raw_size'] > s['raw_offset']):
                raise InvalidImage('overlapping_raw_sections')
        dirs = self.u32(opt + 108)
        self.resource = (0, 0)
        if dirs > 2 and opt_size >= 112 + 3 * 8:
            self.resource = struct.unpack('<II', self.take(opt + 112 + 16, 8))

    def take(self, at: int, size: int) -> bytes:
        if at < 0 or size < 0 or at > len(self.data) or size > len(self.data) - at:
            raise InvalidImage('file_bounds')
        return self.data[at:at+size]

    def u16(self, at: int) -> int:
        return struct.unpack('<H', self.take(at, 2))[0]

    def u32(self, at: int) -> int:
        return struct.unpack('<I', self.take(at, 4))[0]

    def u64(self, at: int) -> int:
        return struct.unpack('<Q', self.take(at, 8))[0]

    def at_rva(self, rva: int, size: int) -> bytes:
        if 0 <= rva <= self.headers and size <= self.headers - rva:
            return self.take(rva, size)
        for s in self.sections:
            delta = rva - s['rva']
            if 0 <= delta <= s['raw_size'] and size <= s['raw_size'] - delta:
                return self.take(s['raw_offset'] + delta, size)
        raise InvalidImage('rva_has_no_file_backing')

    def versions(self) -> dict:
        rva, size = self.resource
        if not rva or not size:
            return {'file': None, 'product': None}
        tree = self.at_rva(rva, size)
        values: set[tuple[str, str]] = set()
        visited = set()
        def walk(at: int, depth: int, version: bool):
            if depth > 3 or at in visited or at + 16 > len(tree):
                raise InvalidImage('invalid_resource_directory')
            visited.add(at)
            count = sum(struct.unpack_from('<HH', tree, at + 12))
            if count > 128 or at + 16 + count * 8 > len(tree):
                raise InvalidImage('invalid_resource_count')
            for i in range(count):
                name, entry = struct.unpack_from('<II', tree, at + 16 + i * 8)
                is_version = version or (depth == 0 and name == 16)
                if not is_version:
                    continue
                if entry & 0x80000000:
                    walk(entry & 0x7fffffff, depth + 1, is_version)
                else:
                    if entry + 16 > len(tree):
                        raise InvalidImage('invalid_resource_data_entry')
                    data_rva, data_bytes = struct.unpack_from('<II', tree, entry)
                    block = self.at_rva(data_rva, data_bytes)
                    if len(block) < 6:
                        raise InvalidImage('invalid_version_resource')
                    length, value_length, value_type = struct.unpack_from('<HHH', block)
                    key = 'VS_VERSION_INFO\0'.encode('utf-16le')
                    off = (6 + len(key) + 3) & ~3
                    if (length > len(block) or block[6:6+len(key)] != key or
                        value_type != 0 or value_length < 52 or off + 52 > length):
                        raise InvalidImage('invalid_fixed_file_info')
                    fields = struct.unpack_from('<13I', block, off)
                    if fields[0] != 0xfeef04bd:
                        raise InvalidImage('invalid_fixed_file_info_signature')
                    def version_string(a, b):
                        return '.'.join(str(x) for x in (a >> 16, a & 65535, b >> 16, b & 65535))
                    values.add((version_string(fields[2], fields[3]), version_string(fields[4], fields[5])))
        walk(0, 0, False)
        if len(values) != 1:
            raise InvalidImage('missing_or_ambiguous_version')
        file, product = next(iter(values))
        return {'file': file, 'product': product}

    def fatbins(self):
        result = []
        decoded_total = 0
        for s in self.sections:
            if not s['flags'] & 0x40000000 or s['flags'] & 0x20000000:
                continue
            blob = self.take(s['raw_offset'], s['raw_size'])
            pos = 0
            while (pos := blob.find(MAGIC, pos)) >= 0:
                if pos + 16 > len(blob):
                    raise InvalidImage('truncated_fatbin')
                _, version, header, payload = struct.unpack_from('<IHHQ', blob, pos)
                end = pos + 16 + payload
                if version != 1 or header != 16 or not 0 < payload <= MAX_PAYLOAD or end > len(blob):
                    raise InvalidImage('invalid_fatbin_header')
                entries, cursor = [], pos + 16
                while cursor < end:
                    if cursor + 64 > end:
                        raise InvalidImage('truncated_entry')
                    kind, ev = struct.unpack_from('<HH', blob, cursor)
                    eh, eb, compressed = struct.unpack_from('<IQI', blob, cursor + 4)
                    arch = struct.unpack_from('<I', blob, cursor + 28)[0]
                    raw = struct.unpack_from('<Q', blob, cursor + 56)[0]
                    if (ev != 0x101 or not 64 <= eh <= 256 or eb > end - cursor - eh or
                        compressed > eb or raw > MAX_PAYLOAD):
                        raise InvalidImage('invalid_entry_header')
                    body = blob[cursor + eh:cursor + eh + eb]
                    text = None
                    if kind == 1:
                        decoded_total += raw if compressed else len(body)
                        if decoded_total > 32 * 1024 * 1024:
                            raise InvalidImage("ptx_decode_budget")
                        decoded = lz4_decompress(body[:compressed], raw) if compressed else body
                        text = decoded.replace(b'\r', b'').rstrip(b'\0')
                    entries.append({'kind': kind, 'arch': arch, 'bytes': eb, 'declared_raw_size': raw,
                                    'fnv1a64': f'{fnv1a64(body):016x}', '_text': text})
                    cursor += eh + eb
                result.append({'rva': s['rva'] + pos, 'bytes': end - pos, 'entries': entries})
                if len(result) > 512:
                    raise InvalidImage('fatbin_count_limit')
                pos = end
        return result


def registry() -> dict:
    parsed = json.loads(MANIFEST.read_text(encoding='utf-8'))
    if parsed.get('schema') != 1 or not parsed.get('profiles'):
        raise ValueError('invalid_profile_manifest')
    return parsed


def qualify(data: bytes, manifest: dict) -> dict:
    report = {'schema': 'mfg_provider_qualification_v1', 'sha256': hashlib.sha256(data).hexdigest(),
              'file_bytes': len(data), 'qualification': 'UNKNOWN', 'profile': None,
              'allowed_backend_targets': [], 'allowed_quality_paths': [], 'rejection_reasons': [],
              'target_preparation': 'NOT_RUN', 'gpu_execution': 'NOT_RUN', 'dll_modified': False}
    try:
        image = Image(data)
        report['image'] = {'timestamp': image.timestamp, 'size': image.size,
                           'preferred_base': image.base, 'sections': image.sections}
        report['versions'] = image.versions()
        inventory = image.fatbins()
        counts = Counter((e['kind'], e['arch']) for b in inventory for e in b['entries'])
        report['inventory'] = {'fatbins': len(inventory),
            'ptx_entries': sum(v for (k, _), v in counts.items() if k == 1),
            'cubin_entries': sum(v for (k, _), v in counts.items() if k == 2),
            'targets': [{'kind': k, 'sm': sm, 'count': count} for (k, sm), count in sorted(counts.items())],
            'containers': [{**b, 'entries': [{k: v for k, v in e.items() if k != '_text'} for e in b['entries']]} for b in inventory]}
        matches = [p for p in manifest['profiles'] if p['timestamp'] == image.timestamp and p['image_size'] == image.size]
        if len(matches) != 1:
            report['rejection_reasons'].append('unknown_or_ambiguous_profile')
            return report
        p = matches[0]
        report['profile'] = p['id']
        report['qualification'] = 'OBSERVE_ONLY'
        if report['sha256'] != p['sha256']:
            report['rejection_reasons'].append('full_file_sha256_mismatch')
        def match_regions(name):
            rows = []
            for r in p[name]:
                try:
                    actual = f"{fnv1a64(image.at_rva(r['rva'], r['bytes'])):016x}"
                except InvalidImage:
                    actual = None
                rows.append({**r, 'actual_fnv1a64': actual, 'match': actual == r['fnv1a64']})
            report[name] = rows
            if not rows or not all(r['match'] for r in rows):
                report['rejection_reasons'].append(name + '_mismatch')
        for name in ('retarget_payloads', 'endpoint_code', 'endpoint_payloads', 'mfg_gates'):
            match_regions(name)
        expected_regions = {(r['rva'], r['bytes']) for r in p['retarget_payloads']}
        if {(b['rva'], b['bytes']) for b in inventory} != expected_regions:
            report['rejection_reasons'].append('fatbin_inventory_mismatch')
        report['selectors'] = []
        for s in p['selectors']:
            actual = list(image.at_rva(s['rva'], 2))
            report['selectors'].append({**s, 'actual': actual, 'match': actual == s['before']})
        if not all(s['match'] for s in report['selectors']):
            report['rejection_reasons'].append('selector_bytes_mismatch')
        # These two vtable entries are also checked by the existing runtime V4 gate.
        report['endpoint_vtables_match'] = all(
            struct.unpack('<Q', image.at_rva(at, 8))[0] == image.base + target
            for at, target in ((b['rva'], b['target_rva']) for b in p['endpoint_vtables']))
        if not report['endpoint_vtables_match']:
            report['rejection_reasons'].append('endpoint_vtable_mismatch')
        report['kernels'] = []
        for k in p['kernels']:
            candidates = [(b, e) for b in inventory for e in b['entries']
                          if e['_text'] is not None and e['arch'] == k['arch']
                          and len(e['_text']) == k['normalized_size']
                          and f"{fnv1a64(e['_text']):016x}" == k['fnv1a64']
                          and k['entry'].encode() in e['_text'] and k['signature'].encode() in e['_text']
                          and (not k['declared_raw_size'] or e['declared_raw_size'] == k['declared_raw_size'])]
            row = {'role': k['role'], 'ptx_matches': len(candidates), 'descriptor_references': 0,
                   'ada_cubin_match': False, 'match': False}
            if len(candidates) == 1:
                b, _ = candidates[0]
                needle = struct.pack('<Q', image.base + b['rva'])
                refs = 0
                for s in image.sections:
                    if s['flags'] & 0x40000000 and not s['flags'] & 0x20000000:
                        blob = image.take(s['raw_offset'], s['raw_size'])
                        at = 0
                        while (at := blob.find(needle, at)) >= 0:
                            if (s['rva'] + at) % 8 == 0:
                                refs += 1
                            at += 1
                cubin_ok = not k['ada_cubin_bytes'] or any(
                    e['kind'] == 2 and e['arch'] == 89 and e['bytes'] == k['ada_cubin_bytes']
                    and e['fnv1a64'] == k['ada_cubin_fnv1a64'] for e in b['entries'])
                row.update(fatbin_rva=b['rva'], descriptor_references=refs, ada_cubin_match=cubin_ok,
                           match=refs == k['descriptor_references'] and cubin_ok)
            report['kernels'].append(row)
        if not all(k['match'] for k in report['kernels']):
            report['rejection_reasons'].append('quality_kernel_or_descriptor_mismatch')
        if not report['rejection_reasons']:
            report['qualification'] = 'QUALIFIED'
            report['allowed_backend_targets'] = p['backend_sms']
            report['allowed_quality_paths'] = [k['role'] for k in p['kernels']]
        report['streamline_dynamic_abi'] = {'version': p['dynamic_streamline_version'],
                                          'options': p['options_version'], 'state': p['state_version']}
        report['qualification_scope'] = 'Pristine file identity/layout only; runtime rechecks target preparation before PATCHABLE.'
    except (InvalidImage, ValueError, struct.error, IndexError) as error:
        report['rejection_reasons'].append(str(error))
        report['allowed_backend_targets'] = []
        report['allowed_quality_paths'] = []
    return report


def self_test(check_registry: str | None, provider: Path | None = None):
    manifest = registry()
    assert len(manifest['profiles']) == 1
    p = manifest['profiles'][0]
    assert len(p['retarget_payloads']) == 70 and len(p['endpoint_payloads']) == 39
    assert len(p['selectors']) == 2 and len(p['kernels']) == 4
    assert all(r in p['retarget_payloads'] for r in p['endpoint_payloads'])
    for size in (0, 1, 63, 255, 256, 1024):
        r = qualify(bytes(size), manifest)
        assert r['qualification'] == 'UNKNOWN' and not r['allowed_backend_targets']
    if check_registry:
        result = subprocess.run([check_registry], check=True, capture_output=True, text=True, timeout=15)
        assert json.loads(result.stdout) == manifest, 'generated manifest differs from runtime registry'
    for packed, size in ((b'\xf0\xff\x00', 1), (b'\x1fA\x01\x00\xff\x00', 4)):
        try:
            lz4_decompress(packed, size)
        except ValueError:
            pass
        else:
            raise AssertionError('malformed LZ4 accepted')
    if provider is not None:
        before = provider.read_bytes()
        good = qualify(before, manifest)
        assert good['qualification'] == 'QUALIFIED', good['rejection_reasons']
        assert good == qualify(bytes(before), manifest), 'nondeterministic qualification'
        image = Image(before)
        for region in (p['endpoint_code'][0], p['endpoint_payloads'][0], p['retarget_payloads'][-1]):
            section = next(s for s in image.sections if s['rva'] <= region['rva'] < s['rva'] + s['raw_size'])
            at = section['raw_offset'] + region['rva'] - section['rva'] + region['bytes'] - 1
            altered = bytearray(before)
            altered[at] ^= 1
            rejected = qualify(bytes(altered), manifest)
            assert rejected['qualification'] != 'QUALIFIED' and not rejected['allowed_backend_targets']
        assert before == provider.read_bytes(), 'provider file changed'
        print('Exact provider: deterministic/read-only/code/payload mutation checks PASS')
    print('qualify_provider self_test=PASS')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('provider', nargs='?', type=Path)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--check-registry')
    args = parser.parse_args()
    if args.self_test:
        self_test(args.check_registry, args.provider)
        return 0
    if args.provider is None:
        parser.error('provide a DLL path or --self-test')
    # Only rb; no LoadLibrary, subprocess execution, output-file or DLL write path.
    with args.provider.open('rb') as src:
        data = src.read(MAX_FILE + 1)
    result = qualify(data, registry())
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result['qualification'] == 'QUALIFIED' else 2


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, AssertionError, subprocess.SubprocessError) as exc:
        print(f'qualification error: {exc}', file=sys.stderr)
        sys.exit(1)
