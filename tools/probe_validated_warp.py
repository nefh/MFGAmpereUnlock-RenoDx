#!/usr/bin/env python3
"""Probe whether the validated-warp DLSS-G PTX can assemble for sm_86/sm_75.

The NVIDIA payload is read from a provider supplied by the developer and is not
stored in this repository. The probe only accepts the exact validated
Kernel_BlendCandidatesFused PTX identity used by DLSS-G 310.9.0/310.9.1.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

EXPECTED_PROVIDER_SHA256 = "ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82"
FATBIN_MAGIC = 0xBA55ED50
PTX_KIND = 1
BLACKWELL_ARCH = 120
RAW_SIZE = 39639
NORMALIZED_SIZE = 39638
FNV1A64 = 0x7A6F5F41105C6D85
ENTRY = ".entry Kernel_BlendCandidatesFused("
PARAMS = ".param .align 8 .b8 Kernel_BlendCandidatesFused_param_0[240]"
BACKPORT_UNSUPPORTED = (
    "wgmma.", "tcgen05.", "tensormap.", "cp.async.bulk",
    ".e4m3", ".e5m2", ".e2m1",
)
SM75_UNSUPPORTED = (
    "cp.async", "mbarrier.", "redux.sync", "mma.sp.", ".bf16", ".tf32",
    ".f16x2.f32", "max.f16", "min.f16",
    "mma.sync.aligned.m16n8k16", "mma.sync.aligned.m16n8k32",
    "mma.sync.aligned.m16n8k64", "mma.sync.aligned.m8n8k128",
    "mma.sync.aligned.m16n8k128", "mma.sync.aligned.m16n8k256",
    "mma.sync.aligned.m8n8k4.row.col.f64",
)


def pe_sections(path: Path) -> tuple[bytes, list[tuple[int, int, str]]]:
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"{path}: not a PE image")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path}: invalid PE signature")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    first = pe + 24 + optional_size
    sections = []
    for index in range(count):
        offset = first + index * 40
        name = data[offset:offset + 8].rstrip(b"\0").decode("ascii", "replace")
        virtual_size, _, raw_size, raw_offset = struct.unpack_from("<IIII", data, offset + 8)
        sections.append((raw_offset, max(virtual_size, raw_size), name))
    return data, sections


def iter_fatbins(data: bytes, sections: list[tuple[int, int, str]], data_only: bool = True):
    magic = struct.pack("<I", FATBIN_MAGIC)
    for raw_offset, size, name in sections:
        if data_only and name != ".data":
            continue
        blob = data[raw_offset:raw_offset + size]
        cursor = 0
        while True:
            relative = blob.find(magic, cursor)
            if relative < 0:
                break
            start = raw_offset + relative
            if start + 16 <= len(data):
                header = struct.unpack_from("<H", data, start + 6)[0]
                payload = struct.unpack_from("<Q", data, start + 8)[0]
                end = start + 16 + payload
                if header == 16 and 0 < payload < 4 << 20 and end <= len(data):
                    yield start, end
            cursor = relative + 4


def iter_entries(data: bytes, start: int, end: int):
    cursor = start + 16
    while cursor + 64 <= end:
        kind = struct.unpack_from("<H", data, cursor)[0]
        header = struct.unpack_from("<I", data, cursor + 4)[0]
        payload = struct.unpack_from("<Q", data, cursor + 8)[0]
        compressed = struct.unpack_from("<I", data, cursor + 16)[0]
        arch = struct.unpack_from("<I", data, cursor + 28)[0]
        raw = struct.unpack_from("<Q", data, cursor + 56)[0]
        if not 64 <= header <= 256 or payload > end - cursor - header:
            return
        yield kind, arch, cursor + header, int(payload), compressed, int(raw)
        cursor += header + payload


def lz4_decompress(data: bytes, expected: int) -> bytes:
    output = bytearray()
    cursor = 0

    def extended(value: int) -> int:
        nonlocal cursor
        if value == 15:
            while True:
                if cursor >= len(data):
                    raise ValueError("truncated LZ4 length")
                extra = data[cursor]
                cursor += 1
                value += extra
                if extra != 255:
                    break
        return value

    while cursor < len(data):
        token = data[cursor]
        cursor += 1
        literals = extended(token >> 4)
        if cursor + literals > len(data):
            raise ValueError("truncated LZ4 literal run")
        output.extend(data[cursor:cursor + literals])
        cursor += literals
        if cursor == len(data):
            break
        if cursor + 2 > len(data):
            raise ValueError("truncated LZ4 back-reference")
        distance = struct.unpack_from("<H", data, cursor)[0]
        cursor += 2
        count = extended(token & 15) + 4
        if distance == 0 or distance > len(output):
            raise ValueError("invalid LZ4 back-reference")
        for _ in range(count):
            output.append(output[-distance])
        if len(output) > expected:
            raise ValueError("LZ4 output exceeds declared size")
    if len(output) != expected:
        raise ValueError(f"LZ4 output is {len(output)} bytes, expected {expected}")
    return bytes(output)


def fnv1a64(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def replace_once(source: str, old: str, new: str, label: str) -> str:
    count = source.count(old)
    if count != 1:
        raise ValueError(f"{label}: found {count} instances, expected one")
    return source.replace(old, new, 1)


def retarget_ptx(source: str, source_arch: int, target_sm: int) -> str:
    if target_sm not in (75, 86):
        raise ValueError(f"unsupported probe target sm_{target_sm}")
    return replace_once(
        source, f".target sm_{source_arch}", f".target sm_{target_sm}", "PTX target")


def unsupported_features(source: str, target_sm: int) -> list[str]:
    tokens = list(BACKPORT_UNSUPPORTED)
    if target_sm == 75:
        tokens.extend(SM75_UNSUPPORTED)
    return [token for token in tokens if token in source]


def turing_unsupported_features(source: str) -> list[str]:
    return unsupported_features(source, 75)



PACKED_HALF_MARKER = "MFGUNLOCK_SM75_PACKED_HALF"
PACKED_HALF_FORM = re.compile(r"\{ cvt\.rn\.f16x2\.f32 (%r([0-9]+)), (%f([0-9]+)), (%f([0-9]+)); \}(\n?)")


def lower_packed_half(source: str, target_sm: int, expected_count: int) -> str:
    if target_sm == 86:
        return source
    if target_sm != 75:
        raise ValueError("unsupported PTX compatibility target")
    if PACKED_HALF_MARKER in source or "%mfg_h" in source:
        raise ValueError("packed-half lowering already present or register collision")
    if expected_count == 0:
        if ".f16x2.f32" in source:
            raise ValueError("unexpected packed-half conversion")
        return source
    declarations = []
    for prefix in (".reg .b32 %r<", ".reg .f32 %f<"):
        matches = list(re.finditer(re.escape(prefix) + r"([0-9]+)>;\n", source))
        if (source.count(prefix) != 1 or len(matches) != 1 or
                not 0 < int(matches[0][1]) <= 0xFFFFFFFF):
            raise ValueError("packed-half register declarations changed")
        declarations.append(matches[0])
    count = 0
    offset = 0
    output = []
    for line in source.splitlines(keepends=True):
        if ".f16x2.f32" not in line:
            output.append(line)
        else:
            match = PACKED_HALF_FORM.fullmatch(line)
            if (match is None or offset < max(item.end() for item in declarations) or
                    int(match[2]) >= int(declarations[0][1]) or
                    max(int(match[4]), int(match[6])) >= int(declarations[1][1])):
                raise ValueError("unrecognized packed-half conversion form")
            # cvt(a,b) = {high=a, low=b}; mov.b32 packs low first.
            output.append("{\ncvt.rn.f16.f32 %mfg_h0, " + match[3] +
                          ";\ncvt.rn.f16.f32 %mfg_h1, " + match[5] +
                          ";\nmov.b32 " + match[1] +
                          ", {%mfg_h1, %mfg_h0};\n}" + match[7])
            count += 1
        offset += len(line)
    if count != expected_count:
        raise ValueError("packed-half conversion count changed")
    result = "".join(output)
    end = declarations[0].end()
    return (result[:end] + ".reg .b16 %mfg_h<2>; // " +
            PACKED_HALF_MARKER + "\n" + result[end:])



MMA16816 = "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16"
MMA1688 = "mma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16"
MMA_FORM = re.compile(
    r"mma\.sync\.aligned\.m16n8k16\.row\.col\.f16\.f16\.f16\.f16 "
    r"\{(%r([0-9]+)),(%r([0-9]+))\}, \{(%r([0-9]+)),(%r([0-9]+)),(%r([0-9]+)),(%r([0-9]+))\}, "
    r"\{(%r([0-9]+)),(%r([0-9]+))\}, \{(%r([0-9]+)),(%r([0-9]+))\};(\n?)")


def lower_turing_mma16816(source: str, target_sm: int, expected_count: int) -> str:
    if target_sm == 86:
        return source
    if target_sm != 75:
        raise ValueError("unsupported PTX compatibility target")
    if expected_count == 0:
        if MMA16816 in source:
            raise ValueError("unexpected m16n8k16 instruction")
        return source
    declarations = list(re.finditer(r"\.reg \.b32 %r<([0-9]+)>;\n", source))
    if source.count(".reg .b32 %r<") != 1 or len(declarations) != 1:
        raise ValueError("MMA register declaration changed")
    limit = int(declarations[0][1])
    count = 0
    offset = 0
    output = []
    for line in source.splitlines(keepends=True):
        if MMA16816 not in line:
            output.append(line)
        else:
            match = MMA_FORM.fullmatch(line)
            numbers = [int(match[i]) for i in (2, 4, 6, 8, 10, 12, 14, 16, 18, 20)] if match else []
            if (match is None or offset < declarations[0].end() or
                    any(number >= limit for number in numbers)):
                raise ValueError("unrecognized m16n8k16 instruction form")
            d0, d1, a0, a1, a2, a3, b0, b1, c0, c1 = [match[i] for i in (1,3,5,7,9,11,13,15,17,19)]
            if d0 in (a2, a3, b1) or d1 in (a2, a3, b1):
                raise ValueError("m16n8k16 split has an output/input register hazard")
            output.append(
                f"{MMA1688} {{{d0},{d1}}},{{{a0},{a1}}},{{{b0}}},{{{c0},{c1}}};\n"
                f"{MMA1688} {{{d0},{d1}}},{{{a2},{a3}}},{{{b1}}},{{{d0},{d1}}};" + match[21])
            count += 1
        offset += len(line)
    if count != expected_count:
        raise ValueError("m16n8k16 instruction count changed")
    return "".join(output)


HALF_MINMAX_FORMS = (
    ("{max.f16 ", "scalar_max", "max", "%rs"),
    ("{min.f16 ", "scalar_min", "min", "%rs"),
    ("{max.f16x2 ", "packed_max", "max", "%r"),
    ("{min.f16x2 ", "packed_min", "min", "%r"),
)


def has_turing_half_minmax(source: str) -> bool:
    return any(("max." in line or "min." in line) and ".f16" in line
               for line in source.splitlines())


def lower_turing_half_minmax(source: str, target_sm: int,
                             expected: tuple[int, int, int, int]) -> str:
    if target_sm == 86:
        return source
    if target_sm != 75:
        raise ValueError("unsupported PTX compatibility target")
    if "%h" in source:
        raise ValueError("half min/max temporary register collision")
    if len(expected) != 4 or any(value < 0 for value in expected):
        raise ValueError("invalid half min/max expected counts")
    if sum(expected) == 0:
        if has_turing_half_minmax(source):
            raise ValueError("unexpected f16 min/max instruction")
        return source

    def declaration(prefix: str):
        matches = list(re.finditer(re.escape(prefix) + r"([0-9]+)>;\n", source))
        if source.count(prefix) != 1 or len(matches) != 1:
            raise ValueError("half min/max register declarations changed")
        limit = int(matches[0][1])
        if not 0 < limit <= 0xFFFFFFFF:
            raise ValueError("half min/max register declarations changed")
        return matches[0], limit

    integer, integer_limit = declaration(".reg .b32 %r<")
    floating, float_limit = declaration(".reg .f32 %f<")
    if float_limit > 0xFFFFFFFF - 2:
        raise ValueError("half min/max register declarations changed")
    scalar_count = expected[0] + expected[1]
    scalar = scalar_limit = None
    if scalar_count:
        scalar, scalar_limit = declaration(".reg .b16 %rs<")

    f0 = f"%f{float_limit}"
    f1 = f"%f{float_limit + 1}"
    actual = [0, 0, 0, 0]
    output = []
    offset = 0

    scalar_re = re.compile(r"\{(max|min)\.f16 (%rs([0-9]+)),(%rs([0-9]+)),(%rs([0-9]+));(\n?)")
    packed_re = re.compile(r"\{(max|min)\.f16x2 (%r([0-9]+)),(%r([0-9]+)),(%r([0-9]+));(\n?)")

    for line in source.splitlines(keepends=True):
        candidate = ("max.f16" in line or "min.f16" in line)
        if not candidate:
            output.append(line)
            offset += len(line)
            continue

        scalar_match = scalar_re.fullmatch(line)
        packed_match = packed_re.fullmatch(line)
        if scalar_match is not None:
            if scalar is None or offset < scalar.end():
                raise ValueError("unrecognized f16 min/max instruction form")
            registers = [int(scalar_match[index]) for index in (3, 5, 7)]
            if any(register >= scalar_limit for register in registers):
                raise ValueError("unrecognized f16 min/max instruction form")
            op = scalar_match[1]
            index = 0 if op == "max" else 1
            destination, a, b = scalar_match[2], scalar_match[4], scalar_match[6]
            output.append(
                "{\n"
                f"cvt.f32.f16 {f0},{a};\n"
                f"cvt.f32.f16 {f1},{b};\n"
                f"{op}.f32 {f0},{f0},{f1};\n"
                f"cvt.rn.f16.f32 {destination},{f0};\n")
            actual[index] += 1
        elif packed_match is not None:
            if offset < integer.end():
                raise ValueError("unrecognized f16 min/max instruction form")
            registers = [int(packed_match[index]) for index in (3, 5, 7)]
            if any(register >= integer_limit for register in registers):
                raise ValueError("unrecognized f16 min/max instruction form")
            op = packed_match[1]
            index = 2 if op == "max" else 3
            destination, a, b = packed_match[2], packed_match[4], packed_match[6]
            output.append(
                "{\n"
                f"mov.b32 {{%h0,%h1}},{a};\n"
                f"mov.b32 {{%h2,%h3}},{b};\n"
                f"cvt.f32.f16 {f0},%h0;\n"
                f"cvt.f32.f16 {f1},%h2;\n"
                f"{op}.f32 {f0},{f0},{f1};\n"
                f"cvt.rn.f16.f32 %h0,{f0};\n"
                f"cvt.f32.f16 {f0},%h1;\n"
                f"cvt.f32.f16 {f1},%h3;\n"
                f"{op}.f32 {f0},{f0},{f1};\n"
                f"cvt.rn.f16.f32 %h1,{f0};\n"
                f"mov.b32 {destination},{{%h0,%h1}};\n")
            actual[index] += 1
        else:
            raise ValueError("unrecognized f16 min/max instruction form")
        offset += len(line)

    if tuple(actual) != tuple(expected):
        raise ValueError("f16 min/max instruction count changed")
    result = "".join(output)
    old_float = f".reg .f32 %f<{float_limit}>;\n"
    new_float = f".reg .f32 %f<{float_limit + 2}>;\n"
    if result.count(old_float) != 1:
        raise ValueError("half min/max f32 declaration changed during rewrite")
    result = result.replace(old_float, new_float, 1)
    if expected[2] + expected[3]:
        integer_decl = f".reg .b32 %r<{integer_limit}>;\n"
        if result.count(integer_decl) != 1:
            raise ValueError("half min/max b32 declaration changed during rewrite")
        result = result.replace(integer_decl, integer_decl + ".reg .b16 %h<4>;\n", 1)
    return result


def compact_provider_ptx(source: str) -> str:
    if "/*" in source or "*/" in source:
        raise ValueError("block-comment PTX is not eligible for compaction")
    output = []
    for line in source.splitlines():
        stripped = line.strip(" \t\r")
        if not stripped or stripped == "//":
            continue
        if '"' not in line and "//" not in line:
            line = line.replace(", ", ",")
        output.append(line)
    if not output:
        raise ValueError("PTX compaction produced an empty program")
    return "\n".join(output)


def lz4_compress_provider(source: bytes, max_bytes: int) -> bytes:
    head = [-1] * 65536
    chain = [-1] * len(source)
    output = bytearray()

    def hash4(position: int) -> int:
        value = int.from_bytes(source[position:position + 4], "little")
        return ((value * 2654435761) & 0xFFFFFFFF) >> 16

    def insert(position: int) -> None:
        if position + 4 > len(source):
            return
        key = hash4(position)
        chain[position] = head[key]
        head[key] = position

    def best(position: int):
        if position + 4 > len(source) or position + 12 > len(source):
            return (0, -1)
        maximum = len(source) - 5 - position
        key = hash4(position)
        candidate = head[key]
        best_length, best_source = 0, -1
        searched = 0
        while candidate >= 0 and searched < 65535:
            searched += 1
            if candidate >= position or position - candidate > 65535:
                break
            if source[candidate:candidate + 4] == source[position:position + 4]:
                length = 4
                while length < maximum and source[candidate + length] == source[position + length]:
                    length += 1
                if length > best_length:
                    best_length, best_source = length, candidate
                    if length == maximum:
                        break
            candidate = chain[candidate]
        return best_length, best_source

    def extra(value: int) -> None:
        while value >= 255:
            output.append(255)
            value -= 255
        output.append(value)

    anchor = position = 0
    while position + 4 <= len(source):
        length, previous = best(position)
        if length < 4:
            insert(position)
            position += 1
            continue
        while position + 5 <= len(source):
            insert(position)
            next_length, next_previous = best(position + 1)
            if next_length < length + 1:
                break
            position += 1
            length, previous = next_length, next_previous
        literals = position - anchor
        encoded = length - 4
        output.append((min(literals, 15) << 4) | min(encoded, 15))
        if literals >= 15:
            extra(literals - 15)
        output.extend(source[anchor:position])
        distance = position - previous
        output.extend(struct.pack("<H", distance))
        if encoded >= 15:
            extra(encoded - 15)
        if len(output) > max_bytes:
            raise ValueError("compressed PTX exceeds provider payload")
        end = position + length
        for cursor in range(position, end):
            if cursor + 4 > len(source):
                break
            if head[hash4(cursor)] != cursor:
                insert(cursor)
        position = anchor = end
    literals = len(source) - anchor
    output.append(min(literals, 15) << 4)
    if literals >= 15:
        extra(literals - 15)
    output.extend(source[anchor:])
    if len(output) > max_bytes:
        raise ValueError("compressed PTX exceeds provider payload")
    if lz4_decompress(bytes(output), len(source)) != source:
        raise ValueError("compressed PTX failed round-trip validation")
    return bytes(output)


def lz4_compress_provider_high(source: bytes, max_bytes: int) -> bytes:
    if not source or not max_bytes:
        raise ValueError("compressed PTX exceeds provider payload")
    depth = 128
    lookahead = 128
    last_literals = 5
    last_match_start = 12
    heads = [-1] * 65536
    chain = [-1] * len(source)
    matches = [(0, -1)] * len(source)

    def hash4(position: int) -> int:
        value = int.from_bytes(source[position:position + 4], "little")
        return ((value * 2654435761) & 0xFFFFFFFF) >> 16

    for position in range(max(0, len(source) - 3)):
        key = hash4(position)
        candidate = heads[key]
        best_length, best_source = 0, -1
        if position + last_match_start <= len(source):
            maximum = len(source) - last_literals - position
            searched = 0
            while candidate >= 0 and searched < depth:
                searched += 1
                if candidate >= position or position - candidate > 65535:
                    break
                if source[candidate:candidate + 4] == source[position:position + 4]:
                    if (best_length and best_length < maximum and
                            source[candidate + best_length] != source[position + best_length]):
                        candidate = chain[candidate]
                        continue
                    length = 4
                    while (length < maximum and
                           source[candidate + length] == source[position + length]):
                        length += 1
                    if length > best_length:
                        best_length, best_source = length, candidate
                        if length == maximum:
                            break
                candidate = chain[candidate]
        matches[position] = (best_length, best_source)
        chain[position] = heads[key]
        heads[key] = position

    def extension_bytes(value: int) -> int:
        return 0 if value < 15 else (value - 15) // 255 + 1

    def sequence_bytes(literals: int, match_length: int) -> int:
        return 1 + extension_bytes(literals) + literals + 2 + extension_bytes(match_length - 4)

    def final_bytes(literals: int) -> int:
        return 1 + extension_bytes(literals) + literals

    n = len(source)
    costs = [0] * (n + 1)
    choices = [None] * (n + 1)
    for anchor in range(n, -1, -1):
        costs[anchor] = final_bytes(n - anchor)
        choices[anchor] = None
        if anchor == n:
            continue
        last = min(n - 1, anchor + lookahead)
        for position in range(anchor, last + 1):
            length, previous = matches[position]
            if length < 4:
                continue
            next_position = position + length
            candidate = sequence_bytes(position - anchor, length) + costs[next_position]
            if candidate < costs[anchor]:
                costs[anchor] = candidate
                choices[anchor] = (position, length, previous)

    if costs[0] > max_bytes:
        raise ValueError("compressed PTX exceeds provider payload")
    output = bytearray()

    def extra(value: int) -> None:
        while value >= 255:
            output.append(255)
            value -= 255
        output.append(value)

    anchor = 0
    while anchor < n:
        choice = choices[anchor]
        if choice is None:
            literals = n - anchor
            output.append(min(literals, 15) << 4)
            if literals >= 15:
                extra(literals - 15)
            output.extend(source[anchor:])
            break
        position, length, previous = choice
        literals = position - anchor
        encoded = length - 4
        output.append((min(literals, 15) << 4) | min(encoded, 15))
        if literals >= 15:
            extra(literals - 15)
        output.extend(source[anchor:position])
        distance = position - previous
        if not 0 < distance <= 65535:
            raise ValueError("invalid high-compression LZ4 distance")
        output.extend(struct.pack("<H", distance))
        if encoded >= 15:
            extra(encoded - 15)
        anchor = position + length

    if not output or len(output) > max_bytes:
        raise ValueError("compressed PTX exceeds provider payload")
    if lz4_decompress(bytes(output), len(source)) != source:
        raise ValueError("compressed PTX failed round-trip validation")
    return bytes(output)


def lz4_compress_provider_fit(source: bytes, max_bytes: int) -> tuple[bytes, str]:
    try:
        return lz4_compress_provider(source, max_bytes), "fast"
    except ValueError:
        return lz4_compress_provider_high(source, max_bytes), "high"


def prepare_compatible_ptx(source: str, target_sm: int, expected_count: int):
    before = unsupported_features(source, target_sm)
    lowered = lower_packed_half(source, target_sm, expected_count)
    count = expected_count if target_sm == 75 else 0
    metadata = {
        "lowering_applied": count != 0,
        "lowered_instruction_count": count,
        "unsupported_features_before_lowering": before,
        "unsupported_features_after_lowering": unsupported_features(lowered, target_sm),
    }
    return retarget_ptx(lowered, BLACKWELL_ARCH, target_sm), metadata


def add_lowering_result(item: dict, metadata: dict) -> None:
    item.update(metadata)
    if item["status"] == "PASS" and metadata["lowering_applied"]:
        item["qualification"] = "PASS after strict SM75 packed-half lowering"


def fingerprint_elf(blob: bytes) -> tuple[int, int, int]:
    if len(blob) < 0x40 or blob[:4] != b"\x7fELF":
        raise ValueError("not an ELF cubin")
    section_offset = struct.unpack_from("<Q", blob, 0x28)[0]
    section_size, section_count, string_index = struct.unpack_from("<HHH", blob, 0x3A)
    if (section_size < 0x40 or section_count == 0 or
            string_index >= section_count or section_offset > len(blob) or
            section_size * section_count > len(blob) - section_offset):
        raise ValueError("invalid ELF section table")
    string_header = section_offset + string_index * section_size
    strings = struct.unpack_from("<Q", blob, string_header + 0x18)[0]
    string_size = struct.unpack_from("<Q", blob, string_header + 0x20)[0]
    if strings >= len(blob) or string_size > len(blob) - strings:
        raise ValueError("invalid ELF string table")

    text = shared = registers = 0
    for index in range(section_count):
        section = section_offset + index * section_size
        name_offset = struct.unpack_from("<I", blob, section)[0]
        if name_offset >= string_size:
            continue
        begin = strings + name_offset
        end = blob.find(b"\0", begin, strings + string_size)
        if end < 0:
            continue
        name = blob[begin:end]
        size = struct.unpack_from("<Q", blob, section + 0x20)[0]
        info = struct.unpack_from("<I", blob, section + 0x2C)[0]
        if name.startswith(b".text."):
            text = int(size)
            registers = (info >> 24) & 0xFF
        elif name.startswith(b".nv.shared"):
            shared = int(size)
    if not text:
        raise ValueError("cubin has no text section")
    return text, shared, registers


def patch_validated_warp_blend(source: str) -> str:
    declarations = ".reg .pred %p<260>;\n"
    source = replace_once(
        source,
        declarations,
        declarations + ".reg .pred %qv<7>;\n.reg .f32 %qf<12>;\n",
        "blend register declaration",
    )
    anchor = "ld.param.u8 %rs8, [%rd6+220];\n"
    program = """// MFGUNLOCK_VALIDATED_WARP_BLEND_V1
cvt.rn.f32.u32 %qf0, %r10;
cvt.rn.f32.u32 %qf1, %r11;
div.approx.ftz.f32 %qf0, 0f3F000000, %qf0;
div.approx.ftz.f32 %qf1, 0f3F000000, %qf1;
sub.ftz.f32 %qf2, 0f3F800000, %qf0;
sub.ftz.f32 %qf3, 0f3F800000, %qf1;
setp.ge.f32 %qv0, %f123, %qf0;
setp.le.f32 %qv2, %f123, %qf2;
and.pred %qv0, %qv0, %qv2;
setp.ge.f32 %qv2, %f124, %qf1;
and.pred %qv0, %qv0, %qv2;
setp.le.f32 %qv2, %f124, %qf3;
and.pred %qv0, %qv0, %qv2;
not.pred %qv2, %p17;
and.pred %qv0, %qv0, %qv2;
setp.ge.f32 %qv1, %f129, %qf0;
setp.le.f32 %qv2, %f129, %qf2;
and.pred %qv1, %qv1, %qv2;
setp.ge.f32 %qv2, %f130, %qf1;
and.pred %qv1, %qv1, %qv2;
setp.le.f32 %qv2, %f130, %qf3;
and.pred %qv1, %qv1, %qv2;
not.pred %qv2, %p16;
and.pred %qv1, %qv1, %qv2;
abs.f32 %qf4, %f125;
abs.f32 %qf5, %f126;
abs.f32 %qf6, %f127;
add.f32 %qf4, %qf4, %qf5;
add.f32 %qf4, %qf4, %qf6;
setp.lt.f32 %qv2, %qf4, 0f7F800000;
and.pred %qv0, %qv0, %qv2;
abs.f32 %qf5, %f131;
abs.f32 %qf6, %f132;
abs.f32 %qf7, %f133;
add.f32 %qf5, %qf5, %qf6;
add.f32 %qf5, %qf5, %qf7;
setp.lt.f32 %qv2, %qf5, 0f7F800000;
and.pred %qv1, %qv1, %qv2;
and.pred %qv3, %qv0, %qv1;
sub.f32 %qf6, %f115, %f119;
sub.f32 %qf7, %f116, %f120;
sub.f32 %qf8, %f117, %f121;
abs.f32 %qf6, %qf6;
abs.f32 %qf7, %qf7;
abs.f32 %qf8, %qf8;
add.f32 %qf6, %qf6, %qf7;
add.f32 %qf6, %qf6, %qf8;
sub.f32 %qf9, %f125, %f131;
sub.f32 %qf10, %f126, %f132;
sub.f32 %qf11, %f127, %f133;
abs.f32 %qf9, %qf9;
abs.f32 %qf10, %qf10;
abs.f32 %qf11, %qf11;
add.f32 %qf9, %qf9, %qf10;
add.f32 %qf9, %qf9, %qf11;
add.f32 %qf10, %qf9, 0f3DA3D70A;
setp.lt.f32 %qv4, %qf10, %qf6;
setp.lt.f32 %qv2, %qf9, 0f3E19999A;
and.pred %qv4, %qv4, %qv2;
and.pred %qv4, %qv4, %qv3;
setp.gt.f32 %qv2, %qf6, 0f3E800000;
setp.ge.f32 %qv5, %f148, 0f3E4CCCCD;
and.pred %qv5, %qv5, %qv2;
or.pred %qv5, %qv5, %qv4;
and.pred %qv0, %qv0, %qv5;
setp.ge.f32 %qv6, %f149, 0f3E4CCCCD;
and.pred %qv6, %qv6, %qv2;
or.pred %qv6, %qv6, %qv4;
and.pred %qv1, %qv1, %qv6;
max.f32 %qf0, %f148, 0f3F59999A;
min.f32 %qf0, %qf0, 0f3F800000;
max.f32 %qf1, %f149, 0f3F59999A;
min.f32 %qf1, %qf1, 0f3F800000;
sub.f32 %qf2, %f125, %f115;
sub.f32 %qf3, %f126, %f116;
sub.f32 %qf4, %f127, %f117;
@%qv0 fma.rn.f32 %f39, %qf0, %qf2, %f115;
@%qv0 fma.rn.f32 %f38, %qf0, %qf3, %f116;
@%qv0 fma.rn.f32 %f37, %qf0, %qf4, %f117;
sub.f32 %qf2, %f131, %f119;
sub.f32 %qf3, %f132, %f120;
sub.f32 %qf4, %f133, %f121;
@%qv1 fma.rn.f32 %f43, %qf1, %qf2, %f119;
@%qv1 fma.rn.f32 %f42, %qf1, %qf3, %f120;
@%qv1 fma.rn.f32 %f41, %qf1, %qf4, %f121;
"""
    return replace_once(source, anchor, program + anchor, "blend insertion point")


def extract_source(provider: Path) -> str:
    data, sections = pe_sections(provider)
    matches = []
    for start, end in iter_fatbins(data, sections):
        for kind, arch, offset, payload, compressed, raw in iter_entries(data, start, end):
            if kind != PTX_KIND or arch != BLACKWELL_ARCH or raw != RAW_SIZE or compressed == 0:
                continue
            blob = data[offset:offset + compressed]
            decoded = lz4_decompress(blob, raw)
            normalized = decoded.replace(b"\r", b"").rstrip(b"\0")
            if (len(normalized) == NORMALIZED_SIZE and fnv1a64(normalized) == FNV1A64 and
                    ENTRY.encode() in normalized and PARAMS.encode() in normalized):
                matches.append(normalized.decode("ascii"))
    if len(matches) != 1:
        raise ValueError(f"{provider}: found {len(matches)} exact validated-warp PTX entries, expected one")
    return matches[0]


def find_ptxas(explicit: str | None) -> Path:
    candidates = []
    if explicit:
        if not Path(explicit).is_file():
            raise FileNotFoundError(f"explicit ptxas does not exist: {explicit}")
        return Path(explicit)
    found = shutil.which("ptxas") or shutil.which("ptxas.exe")
    if found:
        candidates.append(Path(found))
    for name, value in os.environ.items():
        if name == "CUDA_PATH" or name.startswith("CUDA_PATH_V"):
            candidates.append(Path(value) / "bin" / "ptxas.exe")
            candidates.append(Path(value) / "bin" / "ptxas")
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise FileNotFoundError("ptxas not found; install the CUDA Toolkit or pass --ptxas")


def compile_variant(ptxas: Path | None, source: str, target_sm: int, name: str, directory: Path) -> dict:
    target = f"sm_{target_sm}"
    ptx = directory / f"{name}-{target}.ptx"
    cubin = directory / f"{name}-{target}.cubin"
    ptx.write_text(source, encoding="ascii", newline="\n")
    if ptxas is None:
        return {
            "status": "NOT_RUN", "target_sm": target_sm, "size": 0, "sha256": "",
            "detail": "ptxas was not run (--no-assemble)",
            "source_sha256": hashlib.sha256(source.encode("ascii")).hexdigest(),
            "unsupported_features": unsupported_features(source, target_sm),
            "qualification": "NOT_RUN ptxas",
        }
    result = subprocess.run(
        [str(ptxas), f"-arch={target}", "-O3", str(ptx), "-o", str(cubin)],
        capture_output=True, text=True, timeout=120,
    )
    detail = (result.stderr or result.stdout).strip()
    blob = cubin.read_bytes() if cubin.exists() else b""
    unsupported = unsupported_features(source, target_sm)
    item = {
        "status": "PASS" if result.returncode == 0 and blob and not unsupported else "FAIL",
        "target_sm": target_sm,
        "source_sha256": hashlib.sha256(source.encode("ascii")).hexdigest(),
        "size": len(blob),
        "sha256": hashlib.sha256(blob).hexdigest() if blob else "",
        "detail": detail,
        "ptxas_stdout": result.stdout,
        "ptxas_stderr": result.stderr,
        "unsupported_features": unsupported,
    }
    if blob:
        try:
            text, shared, registers = fingerprint_elf(blob)
            item["elf"] = [text, shared, registers]
            item["text_bytes"] = text
            item["shared_memory_bytes"] = shared
            item["registers"] = registers
        except ValueError as error:
            item["elf_error"] = str(error)
    if item["status"] == "PASS":
        item["qualification"] = "PASS after target-only retarget"
    elif unsupported:
        item["qualification"] = "requires clean-room adaptation"
    else:
        item["qualification"] = "FAIL ptxas"
    return item


def packed_half_self_test() -> None:
    original = (".version 8.7\n.target sm_120\n.address_size 64\n"
                ".visible .entry X() {\n.reg .b32 %r<4>;\n.reg .f32 %f<4>;\n"
                "{ cvt.rn.f16x2.f32 %r1, %f2, %f3; }\nret;\n}\n")
    lowered = lower_packed_half(original, 75, 1)
    assert "cvt.rn.f16.f32 %mfg_h0, %f2;" in lowered
    assert "cvt.rn.f16.f32 %mfg_h1, %f3;" in lowered
    assert "mov.b32 %r1, {%mfg_h1, %mfg_h0};" in lowered
    assert not turing_unsupported_features(lowered)
    assert ".f16x2.f32" in turing_unsupported_features(original)
    assert lower_packed_half(original, 86, 1) == original
    for text, count in ((original, 0), (original, 2), (lowered, 1),
                        (original.replace("cvt.rn.", "cvt.rz."), 1),
                        (original.replace("%f3;", "%f4;"), 1),
                        (original + "// %mfg_h0\n", 1)):
        try:
            lower_packed_half(text, 75, count)
        except ValueError:
            continue
        raise AssertionError("invalid packed-half lowering was accepted")




def turing_half_minmax_self_test() -> None:
    original = (".version 8.7\n.target sm_89\n.address_size 64\n"
                ".visible .entry X() {\n.reg .b32 %r<8>;\n.reg .b16 %rs<8>;\n.reg .f32 %f<4>;\n"
                "{max.f16 %rs0,%rs1,%rs2;\n}\n"
                "{min.f16 %rs3,%rs4,%rs5;\n}\n"
                "{max.f16x2 %r0,%r1,%r2;\n}\n"
                "{min.f16x2 %r3,%r4,%r5;\n}\nret;\n}\n")
    counts = (1, 1, 1, 1)
    lowered = lower_turing_half_minmax(original, 75, counts)
    assert not has_turing_half_minmax(lowered)
    assert ".reg .f32 %f<6>;" in lowered
    assert ".reg .b16 %h<4>;" in lowered
    assert "max.f32 %f4,%f4,%f5;" in lowered
    assert "min.f32 %f4,%f4,%f5;" in lowered
    assert "mov.b32 {%h0,%h1},%r1;" in lowered
    assert "mov.b32 %r0,{%h0,%h1};" in lowered
    assert lower_turing_half_minmax(original, 86, counts) == original
    assert "max.f16" in turing_unsupported_features(original)
    assert not turing_unsupported_features(lowered)
    for text, expected in (
            (original, (2, 1, 1, 1)),
            (original, (0, 0, 0, 0)),
            (original.replace("{max.f16 ", "{max.ftz.f16 ", 1), counts),
            (original.replace("%rs0,%rs1,%rs2", "%rs8,%rs1,%rs2", 1), counts),
            (original.replace("%r0,%r1,%r2", "%r8,%r1,%r2", 1), counts),
            (original + "%h0;\n", counts)):
        try:
            lower_turing_half_minmax(text, 75, expected)
        except ValueError:
            continue
        raise AssertionError("invalid half min/max lowering was accepted")


def turing_mma_self_test() -> None:
    original = (".version 8.7\n.target sm_89\n.address_size 64\n"
                ".visible .entry X() {\n.reg .b32 %r<16>;\n"
                "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 "
                "{%r0,%r1}, {%r2,%r3,%r4,%r5}, {%r6,%r7}, {%r8,%r9};\nret;\n}\n")
    lowered = lower_turing_mma16816(original, 75, 1)
    assert MMA16816 not in lowered
    assert lowered.count(MMA1688) == 2
    assert "{%r0,%r1},{%r4,%r5},{%r7},{%r0,%r1};" in lowered
    compact = compact_provider_ptx("\n//\nadd.s32 %r1, %r2, 1;\n.file 1 \"a, b\"\n")
    assert compact == "add.s32 %r1,%r2,1;\n.file 1 \"a, b\""
    raw = (("abc123" * 1000) + "\0").encode("ascii")
    compressed = lz4_compress_provider(raw, len(raw))
    high = lz4_compress_provider_high(raw, len(raw))
    assert len(compressed) < len(raw)
    assert len(high) <= len(compressed)
    assert lz4_decompress(compressed, len(raw)) == raw
    assert lz4_decompress(high, len(raw)) == raw
    for text, count in ((original, 0), (original, 2),
                        (original.replace("{%r0,%r1}, {%r2,%r3,%r4,%r5}",
                                          "{%r4,%r1}, {%r2,%r3,%r4,%r5}"), 1),
                        (original.replace(".f16.f16.f16.f16", ".f32.f16.f16.f32"), 1)):
        try:
            lower_turing_mma16816(text, 75, count)
        except ValueError:
            continue
        raise AssertionError("invalid Turing MMA lowering was accepted")


def self_test() -> None:
    packed_half_self_test()
    turing_half_minmax_self_test()
    turing_mma_self_test()
    fixture = ".version 8.7\n.target sm_120\n.visible .entry X() { ret; }\n"
    if ".target sm_75" not in retarget_ptx(fixture, 120, 75):
        raise AssertionError("sm_75 retarget self-test failed")
    if ".target sm_86" not in retarget_ptx(fixture, 120, 86):
        raise AssertionError("sm_86 retarget self-test failed")
    if turing_unsupported_features("cp.async.ca.shared.global [x], [y], 16;") != ["cp.async"]:
        raise AssertionError("Turing PTX feature self-test failed")
    if unsupported_features("wgmma.mma_async.sync.aligned;", 86) != ["wgmma."]:
        raise AssertionError("generic backport PTX feature self-test failed")
    patched = patch_validated_warp_blend(
        ".reg .pred %p<260>;\n.visible .entry Kernel_BlendCandidatesFused() {\n"
        "ld.param.u8 %rs8, [%rd6+220];\nret;\n}\n")
    if patched.count("MFGUNLOCK_VALIDATED_WARP_BLEND_V1") != 1:
        raise AssertionError("Validated Warp rewrite self-test failed")
    print("self_test=PASS")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("provider", nargs="?", type=Path)
    parser.add_argument("--ptxas")
    parser.add_argument("--no-assemble", action="store_true", help="static audit only; never reports compile PASS")
    parser.add_argument("--targets", nargs="+", default=["sm_86", "sm_75"])
    parser.add_argument("--target-sm", type=int, choices=(75, 86))
    parser.add_argument("--json", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0
    if args.provider is None:
        parser.error("provider is required unless --self-test is used")

    ptxas = None if args.no_assemble else find_ptxas(args.ptxas)
    provider_data = args.provider.read_bytes()
    if hashlib.sha256(provider_data).hexdigest() != EXPECTED_PROVIDER_SHA256:
        raise ValueError("provider SHA256 is not the qualified 310.9.1 image")
    source = extract_source(args.provider)
    patched = patch_validated_warp_blend(source)
    try:
        targets = [args.target_sm] if args.target_sm else [
            int(value.removeprefix("sm_")) for value in args.targets
        ]
    except ValueError:
        parser.error("targets must be sm_75, sm_86, 75, or 86")
    if any(target_sm not in (75, 86) for target_sm in targets):
        parser.error("only sm_75 and sm_86 are supported qualification targets")
    report = {
        "provider": str(args.provider),
        "provider_sha256": hashlib.sha256(provider_data).hexdigest(),
        "ptxas": str(ptxas) if ptxas is not None else None,
        "kernel": "Kernel_BlendCandidatesFused",
        "source_identity": {
            "size": len(source.encode("ascii")),
            "fnv1a64": f"0x{fnv1a64(source.encode('ascii')):016x}",
        },
        "targets": {},
    }
    print(f"provider={args.provider}")
    print(f"provider_sha256={report['provider_sha256']}")
    print(f"ptxas={ptxas}")
    print("kernel=Kernel_BlendCandidatesFused exact_identity=PASS")

    failed = False
    with tempfile.TemporaryDirectory(prefix="mfg-validwarp-probe-") as temp:
        directory = Path(temp)
        for target_sm in targets:
            target = f"sm_{target_sm}"
            target_source, baseline_meta = prepare_compatible_ptx(source, target_sm, 20)
            target_patched, validated_meta = prepare_compatible_ptx(patched, target_sm, 20)
            baseline = compile_variant(ptxas, target_source, target_sm, "baseline", directory)
            validated = compile_variant(ptxas, target_patched, target_sm, "validated-warp", directory)
            add_lowering_result(baseline, baseline_meta)
            add_lowering_result(validated, validated_meta)
            baseline["entrypoint"] = validated["entrypoint"] = "Kernel_BlendCandidatesFused"
            report["targets"][target] = {"baseline": baseline, "validated_warp": validated}
            print(f"{target}: baseline={baseline['status']} size={baseline['size']}; "
                  f"validated_warp={validated['status']} size={validated['size']}")
            for label, item in (("baseline", baseline), ("validated_warp", validated)):
                if item["unsupported_features"]:
                    print(f"  {label} unsupported_features={','.join(item['unsupported_features'])}")
                if item["detail"] and item["status"] != "PASS":
                    print(f"  {label}: {item['detail']}")
            failed |= baseline["status"] != "PASS" or validated["status"] != "PASS"
    report["validated_warp_probe"] = "NOT_RUN" if args.no_assemble else ("FAIL" if failed else "PASS")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"json={args.json}")
    return 2 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
