#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stage-1 read-only compatibility inventory. Never loads or patches a DLL.

Reuses the project's bounded PE/resource reader, LZ4 decoder and provider registry.
Observational fatbin envelopes follow the constraints in fatbin.hpp::Parse.
Export families describe observed surfaces, NOT proof of a C++ calling contract.
All runtime authorization remains UNKNOWN until a later integration/validation stage.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import sys
from typing import Any

from qualify_provider import Image, InvalidImage, registry
from probe_validated_warp import fnv1a64, lz4_decompress

SCHEMA = 'mfg_compatibility_foundation_v1'
MAX_IMAGE = 128 * 1024 * 1024
MAX_PTX = 32 * 1024 * 1024
MAX_DECODE = 128 * 1024 * 1024
MAX_FATBIN = 64 * 1024 * 1024
MAGIC = b'\x50\xed\x55\xba'
ROOT = Path(__file__).resolve().parents[1]


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical(value: Any) -> bytes:
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=True, allow_nan=False) + '\n').encode('ascii')


def identity_multiset(items: list[Any]) -> str | None:
    if not items:
        return None
    # Deliberately preserve duplicates: a set would authorize a missing copy.
    ordered = sorted(items, key=lambda item: json.dumps(item, sort_keys=True, separators=(',', ':')))
    return sha(canonical(ordered))


def gate(count: int, context: bool = False, invariants: bool = False, reason: str = '') -> dict:
    if count < 0:
        raise ValueError('negative_match_count')
    state = 'UNAVAILABLE' if count == 0 else 'AMBIGUOUS' if count > 1 else (
        'STRUCTURAL_MATCH' if context and invariants else 'OBSERVED')
    return {'state': state, 'matches': count, 'context_valid': bool(context),
            'invariants_valid': bool(invariants), 'runtime_authorized': False, 'reason': reason}


class PEView(Image):
    """Additional offline exports only; same base PE validation as qualify_provider."""
    def __init__(self, data: bytes):
        super().__init__(data, max_file_bytes=MAX_IMAGE)
        self.nt = self.u32(0x3c)
        self.opt = self.nt + 24
        self.opt_size = self.u16(self.nt + 20)
        self.directory_count = self.u32(self.opt + 108)
        if self.directory_count > (self.opt_size - 112) // 8 or self.directory_count > 16:
            raise InvalidImage('invalid_directory_count')

    def directory(self, index: int) -> tuple[int, int]:
        if index >= self.directory_count:
            return 0, 0
        return struct.unpack('<II', self.take(self.opt + 112 + 8 * index, 8))

    def cstring(self, rva: int, *, limit: int = 4096) -> str:
        # Never consume across the end of a file-backed header/section.
        if 0 <= rva < self.headers:
            data = self.at_rva(rva, min(limit, self.headers - rva))
        else:
            s = next((s for s in self.sections if 0 <= rva - s['rva'] < s['raw_size']), None)
            if s is None:
                raise InvalidImage('string_rva_has_no_backing')
            data = self.at_rva(rva, min(limit, s['rva'] + s['raw_size'] - rva))
        end = data.find(b'\0')
        if end < 0:
            raise InvalidImage('unterminated_export_string')
        try:
            return data[:end].decode('ascii')
        except UnicodeError as exc:
            raise InvalidImage('non_ascii_export_string') from exc

    def versions(self) -> dict:
        rva, size = self.resource
        if rva and size:
            root = self.at_rva(rva, size)
            if len(root) < 16:
                raise InvalidImage('truncated_resource_root')
            count = sum(struct.unpack_from('<HH', root, 12))
            if count > 128 or 16 + count * 8 > len(root):
                raise InvalidImage('invalid_resource_count')
            if not any(struct.unpack_from('<I', root, 16 + i * 8)[0] == 16 for i in range(count)):
                return {'file': None, 'product': None}
        return super().versions()

    def is_code(self, rva: int) -> bool:
        return any(s['flags'] & 0x20000000 and 0 <= rva - s['rva'] < s['raw_size'] for s in self.sections)

    def exports(self) -> list[dict]:
        rva, size = self.directory(0)
        if not rva and not size:
            return []
        if not rva or size < 40:
            raise InvalidImage('invalid_export_directory')
        self.at_rva(rva, size)
        fields = struct.unpack('<IIHHIIIIIII', self.at_rva(rva, 40))
        _, _, _, _, name, base, count, named, funcs, names, ordinals = fields
        if count > 65536 or named > 65536:
            raise InvalidImage('export_count_budget')
        if name:
            self.cstring(name)
        f = struct.unpack('<' + 'I' * count, self.at_rva(funcs, 4 * count)) if count else ()
        ns = struct.unpack('<' + 'I' * named, self.at_rva(names, 4 * named)) if named else ()
        os = struct.unpack('<' + 'H' * named, self.at_rva(ordinals, 2 * named)) if named else ()
        by_index: dict[int, list[str]] = defaultdict(list)
        seen = set()
        for at, ordinal in zip(ns, os):
            if ordinal >= count:
                raise InvalidImage('export_ordinal_out_of_range')
            symbol = self.cstring(at)
            if not symbol or symbol in seen:
                raise InvalidImage('empty_or_duplicate_export_name')
            seen.add(symbol)
            by_index[ordinal].append(symbol)
        result = []
        for index, target in enumerate(f):
            if not target:
                if index in by_index:
                    raise InvalidImage('named_null_export')
                continue
            forward = rva <= target < rva + size
            destination = self.cstring(target, limit=rva + size - target) if forward else None
            if not forward:
                self.at_rva(target, 1)
            for symbol in sorted(by_index.get(index, [None]), key=lambda n: n or ''):
                result.append({'name': symbol, 'ordinal': base + index, 'rva': target,
                               'kind': 'forwarder' if forward else 'code' if self.is_code(target) else 'data',
                               'forwarder': destination})
        return sorted(result, key=lambda e: (e['name'] or '', e['ordinal']))


def own_exports(exports: list[dict]) -> set[str]:
    return {e['name'] for e in exports if e['name'] and e['kind'] == 'code'}


def read_envelopes(image: PEView) -> tuple[list[dict], list[dict]]:
    """Inventory every bounded envelope; opaque extensions are hashed, not interpreted.

    Unlike the old offline qualifier's 256-byte cap, this follows the production
    parser's existing 4096-byte entry-header cap. It grants no new runtime support.
    Invalid candidates are recorded, never silently skipped as successful inventory.
    """
    containers, issues = [], []
    decoded_total = 0
    for section in image.sections:
        if not section['flags'] & 0x40000000 or section['flags'] & 0x20000000:
            continue
        blob = image.take(section['raw_offset'], section['raw_size'])
        start = 0
        while (start := blob.find(MAGIC, start)) >= 0:
            rva = section['rva'] + start
            try:
                if len(containers) + len(issues) >= 512:
                    raise InvalidImage('fatbin_count_budget')
                if start + 16 > len(blob):
                    raise InvalidImage('truncated_fatbin')
                _, version, header, payload = struct.unpack_from('<IHHQ', blob, start)
                end = start + header + payload
                if version != 1 or header != 16 or not 0 < payload <= MAX_FATBIN - 16 or end > len(blob):
                    raise InvalidImage('invalid_fatbin_header')
                cursor, entries = start + 16, []
                while cursor < end:
                    if end - cursor < 64 or len(entries) >= 32:
                        raise InvalidImage('entry_header_or_count_budget')
                    kind, ev, eh, size = struct.unpack_from('<HHIQ', blob, cursor)
                    compressed = struct.unpack_from('<I', blob, cursor + 16)[0]
                    arch = struct.unpack_from('<I', blob, cursor + 28)[0]
                    flags = struct.unpack_from('<Q', blob, cursor + 40)[0]
                    raw = struct.unpack_from('<Q', blob, cursor + 56)[0]
                    if (kind not in (1, 2) or ev != 0x101 or not 64 <= eh <= 4096 or eh % 8 or size % 8
                            or eh > end - cursor or size > end - cursor - eh or compressed > size or raw > MAX_PTX):
                        raise InvalidImage('invalid_entry_header')
                    body = blob[cursor + eh:cursor + eh + size]
                    entry = dict(kind=kind, architecture=arch, header_bytes=eh, payload_bytes=size,
                                 compressed_bytes=compressed, declared_raw_bytes=raw, flags=flags,
                                 header_sha256=sha(blob[cursor:cursor+eh]), payload_sha256=sha(body),
                                 elf_magic=body.startswith(b'\x7fELF'), decode='NOT_APPLICABLE')
                    if kind == 1:
                        decoded_total += raw if compressed else size
                        if decoded_total > MAX_DECODE:
                            raise InvalidImage('ptx_decode_budget')
                        if bool(compressed) != bool(flags & 0x2000):
                            raise InvalidImage('compression_flag_mismatch')
                        decoded = lz4_decompress(body[:compressed], raw) if compressed else body
                        normalized = decoded.replace(b'\r', b'').rstrip(b'\0')
                        readable_ptx = bool(re.search(rb'\.version\s+', normalized) and re.search(rb'\.target\s+', normalized))
                        entry.update(decode='PASS_PTX' if readable_ptx else 'PASS_OPAQUE',
                                     decoded_sha256=sha(decoded), decoded_bytes=len(decoded))
                        if readable_ptx:
                            target = re.findall(rb'(?m)^\s*\.target\s+sm_(\d+)', normalized)
                            entry.update(normalized_sha256=sha(normalized), normalized_bytes=len(normalized),
                                         ptx_target_arches=sorted({int(x) for x in target}),
                                         ptx_versions=sorted(set(x.decode() for x in re.findall(rb'\.version\s+(\d+\.\d+)', normalized))),
                                         entry_names=sorted(set(x.decode() for x in re.findall(rb'\.entry\s+([A-Za-z_$][\w$]*)', normalized))),
                                         _text=normalized)
                        else:
                            entry['note'] = 'Kind-1 body decompressed but not readable PTX; opaque hash only, no semantic claim.'
                    entries.append(entry)
                    cursor += eh + size
                if cursor != end or not entries:
                    raise InvalidImage('invalid_container_end')
                raw_bytes = blob[start:end]
                containers.append({'rva': rva, 'section': section['name'], 'bytes': end - start,
                                   'sha256': sha(raw_bytes), 'entries': entries, '_raw': raw_bytes})
                start = end
            except (InvalidImage, ValueError, struct.error) as exc:
                issues.append({'rva': rva, 'reason': str(exc)})
                if len(containers) + len(issues) >= 512:
                    return containers, issues
                start += 4
    return containers, issues


def export_surface(exports: list[dict]) -> str:
    names = own_exports(exports)
    common = {'slInit', 'slShutdown', 'slSetTag', 'slEvaluateFeature'}
    modern = {'slGetFeatureFunction', 'slGetFeatureRequirements', 'slGetFeatureVersion'}
    legacy = {'slGetFeatureSettings', 'slSetFeatureConstants'}
    if not common <= names:
        return 'unknown-safe'
    m, l = modern <= names, legacy <= names
    if m and not (legacy & names):
        return 'modern-export-surface'
    if l and not (modern & names):
        return 'legacy-export-surface'
    return 'ambiguous-export-surface' if m and l else 'unknown-safe'


def preset_regions() -> list[dict]:
    # Read the existing exact profile; do not maintain a second RVA table.
    text = (ROOT / 'src/addons/mfgunlock/fg_preset.hpp').read_text(encoding='utf-8')
    def value(symbol: str) -> int:
        match = re.search(r'\b' + re.escape(symbol) + r'\s*=\s*(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*\s*;', text)
        if not match:
            raise ValueError('preset_profile_symbol_missing:' + symbol)
        return int(match.group(1), 0)
    return [{'role': name, 'rva': value('k' + name + 'Rva'), 'bytes': value('k' + name + 'Bytes'),
             'fnv1a64': f"{value('k' + name + 'Hash'):016x}"} for name in ('Reader', 'Selection', 'ReportOverrides')]


def make_reference(data: bytes | None) -> dict | None:
    """Templates are extracted at analysis time from a hash-qualified user DLL.
    No proprietary instruction/payload bytes are written to report or source.
    """
    if data is None:
        return None
    profiles = registry()['profiles']
    matches = [p for p in profiles if p['sha256'] == sha(data)]
    if len(matches) != 1:
        raise ValueError('missing_or_ambiguous_reference_profile')
    profile = matches[0]
    image = PEView(data)
    containers, errors = read_envelopes(image)
    if errors:
        raise ValueError('reference_fatbin_errors')
    templates = {}
    for group in ('retarget_payloads', 'endpoint_code', 'endpoint_payloads', 'mfg_gates'):
        rows = []
        for r in profile[group]:
            body = image.at_rva(r['rva'], r['bytes'])
            if f'{fnv1a64(body):016x}' != r['fnv1a64']:
                raise ValueError('reference_region_mismatch:' + group)
            rows.append({'bytes': r['bytes'], 'sha256': sha(body), '_body': body})
        templates[group] = rows
    templates['preset'] = []
    for r in preset_regions():
        body = image.at_rva(r['rva'], r['bytes'])
        if f'{fnv1a64(body):016x}' != r['fnv1a64']:
            raise ValueError('reference_preset_mismatch')
        templates['preset'].append({'role': r['role'], 'bytes': r['bytes'], 'sha256': sha(body), '_body': body})
    return {'profile': profile, 'templates': templates}


def match_code(image: PEView, templates: list[dict]) -> dict:
    anchors = []
    for index, template in enumerate(templates):
        positions = []
        for s in image.sections:
            if not s['flags'] & 0x20000000:
                continue
            blob = image.take(s['raw_offset'], s['raw_size'])
            at = 0
            while (at := blob.find(template['_body'], at)) >= 0:
                positions.append(s['rva'] + at)
                at += 1
                if len(positions) > 64:
                    break
        anchors.append({'anchor': index, 'bytes': template['bytes'], 'sha256': template['sha256'],
                        'match_rvas': positions, 'match_count': len(positions)})
    ambiguous = any(a['match_count'] > 1 for a in anchors)
    all_unique = bool(anchors) and all(a['match_count'] == 1 for a in anchors)
    result = gate(2 if ambiguous else 1 if all_unique else 0, all_unique, all_unique,
                  'Exact whole-code anchors; relocation/caller/descriptor ABI still require separate validation.')
    result['anchors'] = anchors
    return result


def kernel_matches(image: PEView, containers: list[dict], profile: dict) -> dict:
    rows = {}
    for k in profile['kernels']:
        candidates = []
        for b in containers:
            for entry in b['entries']:
                text = entry.get('_text')
                if (text is not None and entry['architecture'] == k['arch'] and len(text) == k['normalized_size']
                        and k['entry'].encode() in text and k['signature'].encode() in text
                        and f'{fnv1a64(text):016x}' == k['fnv1a64']
                        and (not k['declared_raw_size'] or entry['declared_raw_bytes'] == k['declared_raw_size'])):
                    candidates.append((b, entry))
        evidence = []
        for b, entry in candidates:
            needle = struct.pack('<Q', image.base + b['rva'])
            refs = []
            for s in image.sections:
                if not s['flags'] & 0x40000000 or s['flags'] & 0x20000000:
                    continue
                blob = image.take(s['raw_offset'], s['raw_size']); at = 0
                while (at := blob.find(needle, at)) >= 0:
                    if (s['rva'] + at) % 8 == 0:
                        refs.append(s['rva'] + at)
                    at += 1
            siblings = [e for e in b['entries'] if e['kind'] == 2 and e['architecture'] == 89
                        and e['payload_bytes'] == k['ada_cubin_bytes']]
            cubin_ok = not k['ada_cubin_bytes']
            if not cubin_ok:
                cursor = 16
                for e in b['entries']:
                    if e in siblings:
                        body = b['_raw'][cursor+e['header_bytes']:cursor+e['header_bytes']+e['payload_bytes']]
                        cubin_ok |= f'{fnv1a64(body):016x}' == k['ada_cubin_fnv1a64']
                    cursor += e['header_bytes'] + e['payload_bytes']
            evidence.append({'container_rva': b['rva'], 'normalized_sha256': entry['normalized_sha256'],
                             'descriptor_reference_rvas': refs, 'descriptor_count_expected': k['descriptor_references'],
                             'descriptor_count_match': len(refs) == k['descriptor_references'], 'sibling_cubin_match': cubin_ok})
        unique_ok = len(evidence) == 1 and evidence[0]['descriptor_count_match'] and evidence[0]['sibling_cubin_match']
        rows[k['role']] = {**gate(len(candidates), bool(candidates), unique_ok,
                                   'Existing exact PTX identity plus descriptor-count/sibling evidence; GPU preparation NOT_RUN.'),
                           'evidence': evidence}
    return rows


def transform_profile_observations(containers: list[dict]) -> dict:
    """Presence of historical source fingerprints, never execution permission.

    Runtime SM75 admission is structural since T1; the old whole-program
    fingerprints intentionally live only in the validation fixture. Keep the
    offline corpus report able to compare that historical evidence without
    reintroducing it as a runtime gate.
    """
    sources = [
        (ROOT / 'src/addons/mfgunlock/ptx_retarget.hpp', 'runtime'),
        (ROOT / 'contrib/validation/tests/sm75_reference_profiles.hpp', 'historical-validation-only'),
    ]
    texts = [(path.read_text(encoding='utf-8'), origin) for path, origin in sources if path.is_file()]
    result = {}
    for table in ('kPackedHalfProfiles', 'kTuringMmaProfiles'):
        block = None
        origin = None
        for text, candidate_origin in texts:
            block = re.search(r'\b' + table + r'\[\]\s*=\s*\{(.*?)\n\};', text, re.S)
            if block:
                origin = candidate_origin
                break
        if not block:
            raise ValueError('transform_profile_missing:' + table)
        profiles = [(int(n), int(h, 16), int(count)) for n, h, count in re.findall(
            r'\{(\d+),\s*0x([0-9a-f]+)ull,\s*(\d+),?', block.group(1))]
        observed = []
        for size, digest, count in profiles:
            matches = []
            for b in containers:
                for e in b['entries']:
                    value = e.get('_text')
                    if (value is not None and e['architecture'] == 89 and len(value) == size
                            and fnv1a64(value) == digest):
                        matches.append({'container_rva': b['rva'], 'normalized_sha256': e['normalized_sha256']})
            observed.append({'source_bytes': size, 'source_fnv1a64': f'{digest:016x}',
                             'expected_transform_count': count, 'matches': matches,
                             'status': gate(len(matches), bool(matches), len(matches) == 1,
                                            'Known source fingerprint only; transformation postconditions not executed here.')})
        result[table] = observed
    return result


def temporal_observations(containers: list[dict]) -> dict:
    """Reuse profile definitions from midpoint.hpp; descriptor binding is NOT claimed."""
    source = (ROOT / 'src/addons/mfgunlock/midpoint.hpp').read_text(encoding='utf-8')
    block = re.search(r'kTemporalProfiles\[\]\s*=\s*\{(.*?)\n\};', source, re.S)
    if not block:
        raise ValueError('temporal_profiles_missing')
    profiles = [(int(n), name, desc) for n, name, desc in re.findall(
        r'\{(\d+),\s*"([^"]+)",\s*"([^"]+)"', block.group(1))]
    expected = int(re.search(r'kExpectedMidpoints\s*=\s*(\d+)', source).group(1))
    observations = []
    for size, name, descriptor in profiles:
        matches = []
        for b in containers:
            for e in b['entries']:
                text = e.get('_text')
                if text is None or e['declared_raw_bytes'] != size:
                    continue
                entry = ('.entry ' + name + '(').encode()
                param = ('.param .align 8 .b8 ' + name + '_param_0[144]').encode()
                join = b'$L__BB0_3:'
                multiplications = list(re.finditer(rb'(?m)^mul\.ftz\.f32 [^\n]*0f3F000000;', text))
                insertion = text.find(b'\n', text.find(join))
                context = entry in text and param in text and b'.reg .f32 %f<1362>;' in text
                invariants = (text.count(join) == 1 and insertion >= 0 and len(multiplications) == expected
                              and multiplications[0].start() > insertion)
                matches.append({'container_rva': b['rva'], 'normalized_sha256': e['normalized_sha256'],
                                'signature_context': context, 'join_count': text.count(join),
                                'midpoint_count': len(multiplications), 'invariants': invariants})
        validated = [m for m in matches if m['signature_context'] and m['invariants']]
        observations.append({'entry_name': name, 'descriptor_name': descriptor, 'declared_raw_bytes': size,
                             'candidates': matches, 'status': gate(len(matches), bool(validated), len(validated) == 1,
                             'PTX signature/count evidence; descriptor ABI and composed retarget+temporal runtime remain unqualified.')})
    return {'profiles': observations, 'descriptor_binding': 'NOT_RUN', 'runtime_authorized': False}


def strip_private(obj: Any) -> Any:
    if isinstance(obj, dict):
        return {k: strip_private(v) for k, v in obj.items() if not k.startswith('_')}
    if isinstance(obj, list):
        return [strip_private(v) for v in obj]
    return obj


def analyze_bytes(data: bytes, *, reference: dict | None = None, provider_requested: bool = False) -> dict:
    result = {'schema': SCHEMA, 'sha256': sha(data), 'file_bytes': len(data), 'pe_status': 'INVALID',
              'family': 'unknown-safe', 'capabilities': {}, 'structural_validation': {},
              'provider_payload_identity': None, 'recognized_signatures': {}, 'notes': [],
              'runtime_authorized': False, 'gpu_execution': 'NOT_RUN'}
    try:
        im = PEView(data)
        exp = im.exports()
        names = own_exports(exp)
        vers = im.versions()
        result.update(pe_status='PASS', machine='AMD64', file_version=vers['file'], product_version=vers['product'],
                      pe_timestamp=im.timestamp, preferred_image_base=im.base, size_of_image=im.size,
                      sections=[{**s, 'raw_sha256': sha(im.take(s['raw_offset'], s['raw_size']))} for s in im.sections],
                      exports=exp, export_surface_identity=sha(canonical(sorted(names))),
                      source_has_certificate=bool(im.directory(4)[0]),
                      version_origin='VS_FIXEDFILEINFO' if vers['file'] else 'ABSENT', family=export_surface(exp))
        result['capabilities']['pe_metadata'] = gate(1, True, True, 'PE and export/resource bounds validated offline.')
        for key, export in [('feature_function_lookup', 'slGetFeatureFunction'),
                            ('tag_for_frame_export', 'slSetTagForFrame'), ('plugin_gateway_export', 'slGetPluginFunction'),
                            ('ngx_d3d12_entry', 'NVSDK_NGX_D3D12_CreateFeature'),
                            ('ngx_vulkan_entry', 'NVSDK_NGX_VULKAN_CreateFeature')]:
            result['capabilities'][key] = gate(int(export in names), False, False, 'Export presence only; no call was made.')
        marker_names = ['dlfg_kernel', 'Kernel_EstimateIntermMvecsScatter', 'Kernel_OutputPull']
        markers = {name: name.encode() in data for name in marker_names}
        identity_ok = ('NVSDK_NGX_GetGPUArchitecture' in names and
                       bool({'NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl', 'NVSDK_NGX_VULKAN_PopulateDeviceParameters_Impl'} & names)
                       and (markers['dlfg_kernel'] or (markers['Kernel_EstimateIntermMvecsScatter'] and markers['Kernel_OutputPull'])))
        result['provider_identity_observed'] = identity_ok
        if provider_requested or identity_ok:
            bins, errors = read_envelopes(im)
            inventory = [(e['kind'], e['architecture']) for b in bins for e in b['entries']]
            counts = Counter(inventory)
            signature = [{'kind': k, 'architecture': a, 'count': n} for (k, a), n in sorted(counts.items())]
            payload_ids = [{'bytes': b['bytes'], 'sha256': b['sha256']} for b in bins]
            decoded_ids = [{'kind': e['kind'], 'architecture': e['architecture'],
                            'sha256': e.get('normalized_sha256', e['payload_sha256'])} for b in bins for e in b['entries']]
            result.update(provider_markers=markers, fatbins=strip_private(bins),
                          readable_ptx_entries=sum(e.get('decode') == 'PASS_PTX' for b in bins for e in b['entries']),
                          opaque_kind1_entries=sum(e.get('decode') == 'PASS_OPAQUE' for b in bins for e in b['entries']),
                          fatbin_errors=errors, fatbin_count=len(bins), entry_counts=signature,
                          provider_payload_identity=identity_multiset(payload_ids) if not errors else None,
                          normalized_entry_multiset_identity=identity_multiset(decoded_ids) if not errors else None)
            arches = sorted({a for _, a in inventory})
            naming = 'dlfg' if markers['dlfg_kernel'] else 'named-framework' if identity_ok else 'unproven-identity'
            result['family'] = 'provider/' + naming + '/sm_' + '_'.join(str(a) for a in arches)
            result['structural_validation']['envelopes'] = {'status': 'PASS' if not errors else 'PARTIAL', 'issues': errors}
            result['capabilities']['provider_identity'] = gate(int(identity_ok), identity_ok, identity_ok, 'Combined own-code exports and multiple payload markers, mirroring existing runtime identification.')
            result['capabilities']['fatbin_inventory'] = gate(int(bool(bins)), bool(bins), not errors, 'Bounded envelope/decompression; opaque kind-1 bodies stay opaque, not a backport capability.')
            if reference is not None:
                expected = Counter((r['bytes'], r['sha256']) for r in reference['templates']['retarget_payloads'])
                actual = Counter((r['bytes'], r['sha256']) for r in payload_ids)
                same = bool(bins) and not errors and actual == expected
                result['recognized_signatures']['retarget_payload_multiset'] = gate(int(same), same, same,
                    'Complete SHA256 multiset equality with the existing reference; offsets and version are not identity inputs.')
                result['recognized_signatures']['payload_match_counts'] = {'expected': sum(expected.values()),
                    'matched': sum((expected & actual).values()), 'unexpected': sum((actual - expected).values()),
                    'missing': sum((expected - actual).values())}
                for group in ('endpoint_code', 'mfg_gates', 'preset'):
                    result['recognized_signatures'][group] = match_code(im, reference['templates'][group])
                result['recognized_signatures']['quality_kernels'] = kernel_matches(im, bins, reference['profile'])
                result['recognized_signatures']['source_transforms'] = transform_profile_observations(bins)
                result['recognized_signatures']['temporal_midpoint'] = temporal_observations(bins)
                result['existing_profile_metadata_match'] = (im.timestamp == reference['profile']['timestamp'] and im.size == reference['profile']['image_size'])
                result['existing_profile_exact_file_match'] = result['sha256'] == reference['profile']['sha256']
            else:
                result['notes'].append('No hash-qualified reference supplied; profile-specific comparisons unavailable.')
            for key in ('retarget_runtime', 'temporal_runtime', 'validated_warp_runtime', 'preset_override_runtime', 'architecture_gate_patch'):
                result['capabilities'][key] = {'state': 'UNKNOWN', 'runtime_authorized': False,
                                              'reason': 'Stage 1 is read-only; target preparation and full runtime bindings not validated.'}
        result['capabilities']['streamline_call_abi'] = {'state': 'UNKNOWN', 'runtime_authorized': False,
                'reason': 'Export surface does not encode full struct layout, calling convention, or negotiated runtime support.'}
    except (InvalidImage, ValueError, struct.error, UnicodeError) as exc:
        result.update(pe_status='INVALID' if 'exports' not in result else 'PARTIAL', family='unknown-safe')
        result['structural_validation']['error'] = str(exc)
        result['notes'].append('Rejected for analysis; no patch authorization.')
    return result


def classify_bundle(rows: list[dict]) -> dict:
    by_name = defaultdict(list)
    for row in rows:
        by_name[row['file_name'].lower()].append(row)
    def one(name: str) -> dict | None:
        candidates = by_name.get(name, [])
        return candidates[0] if len(candidates) == 1 and candidates[0]['pe_status'] == 'PASS' else None
    interposer, common, plugin = (one(n) for n in ('sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll'))
    surface = interposer['family'] if interposer else 'unknown-safe'
    gateway_common = bool(common and 'slGetPluginFunction' in own_exports(common['exports']))
    gateway_dlssg = bool(plugin and 'slGetPluginFunction' in own_exports(plugin['exports']))
    complete = surface in ('modern-export-surface', 'legacy-export-surface') and gateway_common
    return {'surface_family': surface, 'assembly_family': surface if complete else 'unknown-safe',
            'interposer_present': interposer is not None, 'common_present': common is not None,
            'dlssg_plugin_present': plugin is not None, 'common_gateway_observed': gateway_common,
            'dlssg_gateway_observed': gateway_dlssg,
            'sl_api_exports': sorted(n for n in own_exports(interposer['exports']) if n.startswith('sl')) if interposer else [],
            'export_signature_identity': sha(canonical(sorted(n for n in own_exports(interposer['exports']) if n.startswith('sl')))) if interposer else None,
            'abi_validation': 'NOT_RUN', 'dynamic_mfg_runtime': 'UNKNOWN', 'ui_runtime': 'UNKNOWN',
            'runtime_authorized': False,
            'notes': ['Missing DLSS-G plugin is not missing interposer support.',
                      'Do not hook slInit based on this export family alone; confirm the adapter ABI separately.']}


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding='utf-8-sig'))


def read_file(path: Path) -> bytes:
    with path.open('rb') as f:
        data = f.read(MAX_IMAGE + 1)
    if len(data) > MAX_IMAGE:
        raise ValueError('file_budget_exceeded:' + str(path))
    return data


def collect(dlssg_root: Path, streamline_root: Path, meta: Path, *, analyzer=None) -> dict:
    """Validate manifests and collect evidence. An optional bounded worker may
    replace only per-file analysis; manifest/identity/assembly logic is shared.
    """
    providers = load_json(dlssg_root / 'compatibility_library_manifest.json')
    streamline = load_json(meta / 'streamline_manifest.json' if meta.is_dir() else meta)
    if not isinstance(providers, list) or not providers or not isinstance(streamline, list) or not streamline:
        raise ValueError('empty_or_invalid_corpus_manifest')
    input_rows = []
    for origin, root, entries, prefix in [('dlss_g', dlssg_root, providers, 'dlss_g/'),
                                         ('streamline', streamline_root, streamline, 'streamline/')]:
        declared = set()
        for entry in entries:
            rel = entry['relative_path'].replace('\\', '/')
            parts = PurePosixPath(rel)
            if parts.is_absolute() or '..' in parts.parts or not rel.startswith(prefix):
                raise ValueError('unsafe_manifest_path:' + rel)
            local = root / rel[len(prefix):]
            if local.resolve().is_relative_to(root.resolve()) is False or not local.is_file():
                raise ValueError('missing_or_escaping_file:' + rel)
            if local.resolve() in declared:
                raise ValueError('duplicate_manifest_path:' + rel)
            declared.add(local.resolve())
            data = read_file(local)
            if sha(data) != entry['sha256'].lower() or len(data) != entry['bytes']:
                raise ValueError('corpus_integrity_mismatch:' + rel)
            input_rows.append((origin, entry, local))
        actual = {p.resolve() for p in root.rglob('*') if p.is_file() and p.suffix.lower() == '.dll'}
        if actual != declared:
            raise ValueError('undeclared_or_missing_dlls:' + origin)
    ref_sha = registry()['profiles'][0]['sha256']
    refs = [p for _, e, p in input_rows if e['sha256'].lower() == ref_sha]
    reference = make_reference(read_file(refs[0])) if refs and analyzer is None else None
    cache, rows = {}, []
    for index, (origin, entry, local) in enumerate(input_rows, 1):
        key = (entry['sha256'].lower(), local.name.lower() == 'nvngx_dlssg.dll')
        if key not in cache:
            cache[key] = (analyzer(local, refs[0] if refs else None, key[1])
                          if analyzer is not None else
                          analyze_bytes(read_file(local), reference=reference, provider_requested=key[1]))
            if cache[key]['sha256'] != key[0]:
                raise ValueError('file_changed_during_analysis:' + str(local))
        row = {**cache[key], 'kind': origin, 'path': entry['relative_path'].replace('\\', '/'),
               'file_name': local.name, 'sdk_version': entry.get('sdk_version'), 'source_version': entry.get('source_version'),
               'source_url': entry.get('package_url', entry.get('source_url')), 'manifest_hash_match': True,
               'collector_authenticode_status': entry.get('authenticode_status'),
               'authenticode_reverified': False}
        rows.append(row)
        print(f'[{index}/{len(input_rows)}] {row["path"]}: {row["pe_status"]}', file=sys.stderr, flush=True)
    rows.sort(key=lambda r: r['path'])
    bundles = []
    for version in sorted({r['sdk_version'] for r in rows if r['sdk_version']}, key=lambda v: tuple(map(int, v.split('.')))):
        subset = [r for r in rows if r['sdk_version'] == version]
        bundles.append({'sdk_version': version, 'dll_count': len(subset), **classify_bundle(subset)})
    groups = defaultdict(list)
    for row in rows:
        if row['provider_payload_identity']:
            groups[row['provider_payload_identity']].append({'path': row['path'], 'file_version': row['file_version'], 'sha256': row['sha256']})
    return {'schema': SCHEMA, 'scope': 'offline-foundation-only',
            'reference_sha256': ref_sha if refs else None,
            'normalization': 'PTX remove CR and trailing NUL, matching existing qualifier; no semantic normalization.',
            'summary': {'dlssg_corpus_records': len(providers), 'streamline_records': len(streamline),
                        'streamline_sdk_versions': len(bundles), 'dll_records': len(rows),
                        'unique_file_hashes': len({r['sha256'] for r in rows}),
                        'pe_pass': sum(r['pe_status'] == 'PASS' for r in rows),
                        'provider_payload_groups': len(groups)},
            'provider_payload_groups': [{'identity': identity, 'members': values} for identity, values in sorted(groups.items())],
            'streamline_bundles': bundles, 'libraries': rows}


def write_outputs(report: dict, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out / 'compatibility_matrix.json').write_bytes(canonical(report))
    columns = ['kind', 'path', 'sha256', 'file_version', 'product_version', 'pe_timestamp', 'family', 'pe_status',
               'sdk_version', 'provider_payload_identity', 'fatbin_count', 'exports', 'sections',
               'recognized_signatures', 'capabilities', 'structural_validation', 'notes', 'source_url', 'runtime_authorized']
    with (out / 'compatibility_matrix.csv').open('w', encoding='utf-8', newline='') as f:
        writer = csv.DictWriter(f, columns, lineterminator='\n'); writer.writeheader()
        for row in report['libraries']:
            record = {}
            for key in columns:
                value = row.get(key)
                record[key] = json.dumps(value, sort_keys=True, ensure_ascii=True, separators=(',', ':')) if isinstance(value, (dict, list)) else value
            writer.writerow(record)
    lines = ['# Compatibility matrix - Stage 1', '',
             '**Offline evidence only. No DLL was loaded or patched. No runtime capability is authorized.**', '',
             'Metadata and export presence do not prove a calling ABI. Payload equality does not prove whole-provider compatibility.', '',
             '## Summary', '', '```json', json.dumps(report['summary'], indent=2), '```', '',
             '## Provider corpus', '',
             '| Version | File hash (prefix) | Family | Fatbins | Full reference payload set | Quality signatures (structural) |',
             '|---|---|---|---:|---|---|']
    for r in [r for r in report['libraries'] if r['kind'] == 'dlss_g']:
        sig = r['recognized_signatures']; q = sig.get('quality_kernels', {})
        lines.append(f'| {r.get("file_version")} | `{r["sha256"][:16]}` | {r["family"]} | {r.get("fatbin_count",0)} | {sig.get("retarget_payload_multiset",{}).get("state","UNKNOWN")} | {", ".join(k for k,v in q.items() if v["state"]=="STRUCTURAL_MATCH") or "none"} |')
    lines += ['', '## Streamline bundles', '', '| SDK label | DLLs | Export family | Common gateway | DLSS-G gateway | TagForFrame | ABI verified |', '|---|---:|---|---|---|---|---|']
    for b in report['streamline_bundles']:
        lines.append(f'| {b["sdk_version"]} | {b["dll_count"]} | {b["assembly_family"]} | {b["common_gateway_observed"]} | {b["dlssg_gateway_observed"]} | {"slSetTagForFrame" in b["sl_api_exports"]} | NOT_RUN |')
    lines += ['', '## Shared raw-container payload identities', '', 'Locations/versions are excluded; duplicate containers remain part of the multiset.', '']
    for group in report['provider_payload_groups']:
        if len(group['members']) > 1:
            lines += [f'- `{group["identity"]}`: ' + ', '.join(f'`{m["path"]}`' for m in group['members'])]
    lines += ['', '## Limits', '',
              'The complete JSON includes every file, export, section, container and matched-anchor RVA. These RVAs are observations, not patch tables.',
              'PTX signatures reuse the supplied exact profile. Shader/CUDA target preparation, private ABI, relocation binding and GPU execution are NOT_RUN.',
              'Authenticode is recorded from the collector manifest, not reverified on this host. SHA256 was recomputed for every DLL.',
              'Missing plugin files are recorded as missing; no feature support is inferred from the SDK version.', '']
    (out / 'compatibility_matrix.md').write_text('\n'.join(lines), encoding='utf-8', newline='\n')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--dlssg-root', type=Path, required=True)
    ap.add_argument('--streamline-root', type=Path, required=True)
    ap.add_argument('--streamline-meta', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    report = collect(args.dlssg_root, args.streamline_root, args.streamline_meta)
    write_outputs(report, args.out)
    print(json.dumps(report['summary'], indent=2))
    return 0 if report['summary']['pe_pass'] == report['summary']['dll_records'] else 2


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError) as exc:
        print('Corpus analysis failed: ' + str(exc), file=sys.stderr)
        raise SystemExit(1)
