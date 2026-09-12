#!/usr/bin/env python3
"""Probe whether the validated-warp DLSS-G PTX can assemble for sm_86/sm_75.

The NVIDIA payload is read from a provider supplied by the developer and is not
stored in this repository. The probe only accepts the exact validated
Kernel_BlendCandidatesFused PTX identity used by DLSS-G 310.9.0/310.9.1.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

FATBIN_MAGIC = 0xBA55ED50
PTX_KIND = 1
BLACKWELL_ARCH = 120
RAW_SIZE = 39639
NORMALIZED_SIZE = 39638
FNV1A64 = 0x7A6F5F41105C6D85
ENTRY = ".entry Kernel_BlendCandidatesFused("
PARAMS = ".param .align 8 .b8 Kernel_BlendCandidatesFused_param_0[240]"


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


def iter_fatbins(data: bytes, sections: list[tuple[int, int, str]]):
    magic = struct.pack("<I", FATBIN_MAGIC)
    for raw_offset, size, name in sections:
        if name != ".data":
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
        candidates.append(Path(explicit))
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


def compile_variant(ptxas: Path, source: str, target: str, name: str, directory: Path) -> tuple[bool, int, str]:
    ptx = directory / f"{name}-{target}.ptx"
    cubin = directory / f"{name}-{target}.cubin"
    ptx.write_text(source, encoding="ascii", newline="\n")
    result = subprocess.run(
        [str(ptxas), f"-arch={target}", "-O3", str(ptx), "-o", str(cubin)],
        capture_output=True, text=True,
    )
    detail = (result.stderr or result.stdout).strip()
    return result.returncode == 0, cubin.stat().st_size if cubin.exists() else 0, detail


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("provider", type=Path)
    parser.add_argument("--ptxas")
    parser.add_argument("--targets", nargs="+", default=["sm_86", "sm_75"])
    args = parser.parse_args()

    ptxas = find_ptxas(args.ptxas)
    source = extract_source(args.provider)
    patched = patch_validated_warp_blend(source)
    print(f"provider={args.provider}")
    print(f"ptxas={ptxas}")
    print("kernel=Kernel_BlendCandidatesFused exact_identity=PASS")

    failed = False
    with tempfile.TemporaryDirectory(prefix="mfg-validwarp-probe-") as temp:
        directory = Path(temp)
        for target in args.targets:
            target_source = replace_once(source, ".target sm_120", f".target {target}", "PTX target")
            target_patched = replace_once(patched, ".target sm_120", f".target {target}", "PTX target")
            baseline_ok, baseline_size, baseline_detail = compile_variant(
                ptxas, target_source, target, "baseline", directory)
            patched_ok, patched_size, patched_detail = compile_variant(
                ptxas, target_patched, target, "validated-warp", directory)
            print(f"{target}: baseline={'PASS' if baseline_ok else 'FAIL'} size={baseline_size}; "
                  f"validated_warp={'PASS' if patched_ok else 'FAIL'} size={patched_size}")
            if baseline_detail and not baseline_ok:
                print(f"  baseline: {baseline_detail}")
            if patched_detail and not patched_ok:
                print(f"  validated_warp: {patched_detail}")
            failed |= not baseline_ok or not patched_ok
    return 2 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
