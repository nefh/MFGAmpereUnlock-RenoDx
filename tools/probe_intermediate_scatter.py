#!/usr/bin/env python3
"""Read-only Intermediate Scatter Retention probe for the Ampere sm_86 path.

This reuses the existing validated-warp probe helpers. It accepts only the exact
DLSS-G 310.9.1 provider metadata used by the current Ampere quality path, finds
the intermediate motion-vector kernel, reproduces the upstream ISR PTX edit,
and assembles baseline/ISR sm_86 variants without modifying the provider.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

import probe_validated_warp as common

CUBIN_KIND = 2
AMPERE_ARCH = 86
ADA_ARCH = 89
EXPECTED_TIMESTAMP = 0x6A986031
EXPECTED_IMAGE_SIZE = 7_565_312
TARGET_ENTRY = "Kernel_EstimateIntermMvecsScatter"
WARP_ENTRY = "Kernel_BlendCandidatesFused"
ISR_ANCHOR = (
    "ld.param.f32 %f2, "
    "[Kernel_EstimateIntermMvecsScatter_param_0+120];\n"
)
ISR_INSERTION = (
    "mul.ftz.f32 %f2, %f2, 0f3F000000; "
    "// thin-geometry intermediate scatter\n"
)

IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_READ = 0x40000000


@dataclass(frozen=True)
class SectionLayout:
  raw_offset: int
  raw_size: int
  virtual_address: int
  characteristics: int


@dataclass(frozen=True)
class PeLayout:
  timestamp: int
  image_size: int
  image_base: int
  sections: tuple[SectionLayout, ...]


def ReadPeLayout(data: bytes) -> PeLayout:
  if len(data) < 0x40 or data[:2] != b"MZ":
    raise ValueError("provider is not a PE image")
  pe = struct.unpack_from("<I", data, 0x3C)[0]
  if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
    raise ValueError("provider has an invalid PE signature")
  count = struct.unpack_from("<H", data, pe + 6)[0]
  timestamp = struct.unpack_from("<I", data, pe + 8)[0]
  optional_size = struct.unpack_from("<H", data, pe + 20)[0]
  optional = pe + 24
  if optional + optional_size > len(data):
    raise ValueError("provider has a truncated optional header")
  if struct.unpack_from("<H", data, optional)[0] != 0x20B:
    raise ValueError("provider is not a PE32+ image")
  image_base = struct.unpack_from("<Q", data, optional + 24)[0]
  image_size = struct.unpack_from("<I", data, optional + 56)[0]
  first = optional + optional_size
  sections = []
  for index in range(count):
    offset = first + index * 40
    if offset + 40 > len(data):
      raise ValueError("provider has a truncated section table")
    virtual_address = struct.unpack_from("<I", data, offset + 12)[0]
    raw_size, raw_offset = struct.unpack_from("<II", data, offset + 16)
    characteristics = struct.unpack_from("<I", data, offset + 36)[0]
    if raw_offset > len(data) or raw_size > len(data) - raw_offset:
      raise ValueError("provider section exceeds the file")
    sections.append(SectionLayout(raw_offset, raw_size, virtual_address,
                                  characteristics))
  return PeLayout(timestamp, image_size, image_base, tuple(sections))


def DecodePtx(data: bytes, entry: tuple[int, int, int, int, int, int]) -> bytes:
  kind, _, offset, size, compressed, raw = entry
  if kind != common.PTX_KIND:
    raise ValueError("entry is not PTX")
  if compressed:
    if compressed > size:
      raise ValueError("compressed PTX size exceeds payload size")
    decoded = common.lz4_decompress(data[offset:offset + compressed], raw)
  else:
    decoded = data[offset:offset + size]
  return decoded.replace(b"\r", b"").rstrip(b"\0")


def EntryName(source: bytes) -> str | None:
  import re
  match = re.search(rb"\.entry\s+([A-Za-z0-9_]+)\s*\(", source)
  return match.group(1).decode("ascii") if match else None


def FindKernelFatbins(provider: Path, kernel: str):
  data, sections = common.pe_sections(provider)
  matches = []
  for start, end in common.iter_fatbins(data, sections):
    entries = list(common.iter_entries(data, start, end))
    names = set()
    for entry in entries:
      if entry[0] != common.PTX_KIND or entry[1] not in (ADA_ARCH, common.BLACKWELL_ARCH):
        continue
      name = EntryName(DecodePtx(data, entry))
      if name:
        names.add(name)
    if kernel in names:
      matches.append((start, end, entries))
  return data, matches


def GetEntry(entries, kind: int, architecture: int):
  matches = [entry for entry in entries
             if entry[0] == kind and entry[1] == architecture]
  if len(matches) != 1:
    raise ValueError(
        f"found {len(matches)} kind={kind} sm_{architecture} entries, expected one")
  return matches[0]


def FingerprintElf(blob: bytes) -> tuple[int, int, int]:
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


def RawOffsetToRva(layout: PeLayout, offset: int) -> int:
  for section in layout.sections:
    if section.raw_offset <= offset < section.raw_offset + section.raw_size:
      return section.virtual_address + offset - section.raw_offset
  raise ValueError("fatbin is not inside a PE section")


def CountPreferredVaReferences(data: bytes, layout: PeLayout, raw_offset: int) -> int:
  expected = struct.pack("<Q", layout.image_base + RawOffsetToRva(layout, raw_offset))
  count = 0
  for section in layout.sections:
    if not (section.characteristics & IMAGE_SCN_MEM_READ):
      continue
    if section.characteristics & IMAGE_SCN_MEM_EXECUTE:
      continue
    blob = data[section.raw_offset:section.raw_offset + section.raw_size]
    count += sum(blob[offset:offset + 8] == expected
                 for offset in range(0, max(0, len(blob) - 7), 8))
  return count


def PatchIntermediateScatter(source: str) -> str:
  return common.replace_once(
      source, ISR_ANCHOR, ISR_ANCHOR + ISR_INSERTION,
      "intermediate scatter divisor load")


def Retarget(source: str, source_arch: int) -> str:
  return common.replace_once(
      source, f".target sm_{source_arch}", ".target sm_86", "PTX target")


def Compile(ptxas: Path, source: str, name: str, directory: Path):
  ptx = directory / f"{name}.ptx"
  cubin = directory / f"{name}.cubin"
  ptx.write_text(source, encoding="ascii", newline="\n")
  result = subprocess.run(
      [str(ptxas), "-arch=sm_86", "-O3", str(ptx), "-o", str(cubin)],
      capture_output=True, text=True)
  detail = (result.stderr or result.stdout).strip()
  blob = cubin.read_bytes() if cubin.exists() else b""
  return result.returncode == 0, blob, detail


def Probe(provider: Path, ptxas_arg: str | None, inspect_only: bool):
  data, target_matches = FindKernelFatbins(provider, TARGET_ENTRY)
  _, warp_matches = FindKernelFatbins(provider, WARP_ENTRY)
  layout = ReadPeLayout(data)
  if layout.timestamp != EXPECTED_TIMESTAMP or layout.image_size != EXPECTED_IMAGE_SIZE:
    raise ValueError(
        "provider metadata mismatch: expected timestamp "
        f"0x{EXPECTED_TIMESTAMP:08x}, image-size {EXPECTED_IMAGE_SIZE}; got "
        f"0x{layout.timestamp:08x}, {layout.image_size}")
  if len(target_matches) != 1:
    raise ValueError(f"found {len(target_matches)} {TARGET_ENTRY} fatbins, expected one")

  start, end, entries = target_matches[0]
  expected_layout = [(common.PTX_KIND, common.BLACKWELL_ARCH),
                     (common.PTX_KIND, ADA_ARCH), (CUBIN_KIND, ADA_ARCH)]
  actual_layout = [(entry[0], entry[1]) for entry in entries]
  if actual_layout != expected_layout:
    raise ValueError(f"unexpected target fatbin layout: {actual_layout}")

  blackwell_ptx = DecodePtx(data, GetEntry(entries, common.PTX_KIND, common.BLACKWELL_ARCH))
  ada_ptx = DecodePtx(data, GetEntry(entries, common.PTX_KIND, ADA_ARCH))
  cubin_entry = GetEntry(entries, CUBIN_KIND, ADA_ARCH)
  ada_cubin = data[cubin_entry[2]:cubin_entry[2] + cubin_entry[3]]
  blackwell_text = blackwell_ptx.decode("ascii")
  ada_text = ada_ptx.decode("ascii")
  blackwell_anchor_count = blackwell_text.count(ISR_ANCHOR)
  ada_anchor_count = ada_text.count(ISR_ANCHOR)
  if blackwell_anchor_count != 1:
    raise ValueError(f"Blackwell ISR anchor count is {blackwell_anchor_count}, expected one")

  same_warp_fatbin = None
  warp_offset = None
  if len(warp_matches) == 1:
    warp_offset = warp_matches[0][0]
    same_warp_fatbin = warp_offset == start

  report = {
      "provider": str(provider),
      "provider_sha256": hashlib.sha256(data).hexdigest(),
      "provider_file_size": len(data),
      "pe_timestamp": f"0x{layout.timestamp:08x}",
      "pe_image_size": layout.image_size,
      "target_kernel": TARGET_ENTRY,
      "target_fatbin_offset": f"0x{start:x}",
      "target_fatbin_rva": f"0x{RawOffsetToRva(layout, start):x}",
      "target_fatbin_size": end - start,
      "target_layout": actual_layout,
      "target_preferred_va_refs_offline": CountPreferredVaReferences(data, layout, start),
      "warp_fatbin_matches": len(warp_matches),
      "warp_fatbin_offset": f"0x{warp_offset:x}" if warp_offset is not None else None,
      "same_physical_fatbin_as_validated_warp": same_warp_fatbin,
      "blackwell_ptx": {
          "size": len(blackwell_ptx),
          "fnv1a64": f"0x{common.fnv1a64(blackwell_ptx):016x}",
          "sha256": hashlib.sha256(blackwell_ptx).hexdigest(),
          "anchor_count": blackwell_anchor_count,
      },
      "ada_ptx": {
          "size": len(ada_ptx),
          "fnv1a64": f"0x{common.fnv1a64(ada_ptx):016x}",
          "sha256": hashlib.sha256(ada_ptx).hexdigest(),
          "anchor_count": ada_anchor_count,
      },
      "ada_cubin_slot": {
          "size": len(ada_cubin),
          "fnv1a64": f"0x{common.fnv1a64(ada_cubin):016x}",
          "sha256": hashlib.sha256(ada_cubin).hexdigest(),
          "elf": list(FingerprintElf(ada_cubin)),
      },
      "upstream_isr_edit": {
          "anchor": ISR_ANCHOR.rstrip(),
          "insertion": ISR_INSERTION.rstrip(),
      },
  }

  if inspect_only:
    report["sm86"] = "SKIPPED"
    return report, True

  ptxas = common.find_ptxas(ptxas_arg)
  report["ptxas"] = str(ptxas)
  variants = {
      "blackwell_baseline": Retarget(blackwell_text, common.BLACKWELL_ARCH),
      "intermediate_scatter": PatchIntermediateScatter(
          Retarget(blackwell_text, common.BLACKWELL_ARCH)),
  }
  if ada_anchor_count == 1:
    variants["ada_baseline_diagnostic"] = Retarget(ada_text, ADA_ARCH)
    variants["ada_isr_diagnostic"] = PatchIntermediateScatter(
        Retarget(ada_text, ADA_ARCH))

  passed = True
  results = {}
  with tempfile.TemporaryDirectory(prefix="mfg-isr-probe-") as temp:
    directory = Path(temp)
    for name, source in variants.items():
      ok, blob, detail = Compile(ptxas, source, name, directory)
      fits = bool(blob) and len(blob) <= len(ada_cubin)
      item = {"status": "PASS" if ok else "FAIL",
              "size": len(blob), "fits_ada_slot": fits}
      if blob:
        item["sha256"] = hashlib.sha256(blob).hexdigest()
        item["elf"] = list(FingerprintElf(blob))
      if detail:
        item["detail"] = detail
      results[name] = item
      if name in ("blackwell_baseline", "intermediate_scatter"):
        passed &= ok and fits
  report["sm86"] = results
  return report, passed


def PrintReport(report: dict) -> None:
  print(f"provider={report['provider']}")
  print(f"provider_sha256={report['provider_sha256']}")
  print(f"pe_timestamp={report['pe_timestamp']} image_size={report['pe_image_size']}")
  print("provider_metadata_exact_310_9_1=PASS")
  print(f"target_kernel={report['target_kernel']}")
  print(f"target_fatbin={report['target_fatbin_offset']} "
        f"rva={report['target_fatbin_rva']} size={report['target_fatbin_size']}")
  print(f"target_layout={report['target_layout']}")
  print(f"target_preferred_va_refs_offline={report['target_preferred_va_refs_offline']}")
  print(f"warp_fatbin_matches={report['warp_fatbin_matches']} "
        f"same_fatbin={report['same_physical_fatbin_as_validated_warp']}")
  for name in ("blackwell_ptx", "ada_ptx"):
    item = report[name]
    print(f"{name}: size={item['size']} fnv={item['fnv1a64']} "
          f"anchor_count={item['anchor_count']}")
  slot = report["ada_cubin_slot"]
  print(f"ada_cubin_slot: size={slot['size']} fnv={slot['fnv1a64']} "
        f"elf={tuple(slot['elf'])}")
  if report["sm86"] == "SKIPPED":
    print("sm86_compile=SKIPPED")
    return
  for name, item in report["sm86"].items():
    print(f"{name}: {item['status']} size={item['size']} "
          f"fits_slot={'PASS' if item['fits_ada_slot'] else 'FAIL'}")
    if item.get("detail") and item["status"] != "PASS":
      print(f"  {item['detail']}")


def SelfTest() -> None:
  source = ".target sm_120\n" + ISR_ANCHOR + "ret;\n"
  patched = PatchIntermediateScatter(source)
  if patched.count(ISR_INSERTION) != 1:
    raise AssertionError("ISR patch self-test failed")
  if ".target sm_86" not in Retarget(source, common.BLACKWELL_ARCH):
    raise AssertionError("retarget self-test failed")
  print("self_test=PASS")


def Main() -> int:
  parser = argparse.ArgumentParser()
  parser.add_argument("provider", nargs="?", type=Path)
  parser.add_argument("--ptxas")
  parser.add_argument("--inspect-only", action="store_true")
  parser.add_argument("--json", type=Path)
  parser.add_argument("--self-test", action="store_true")
  args = parser.parse_args()
  if args.self_test:
    SelfTest()
    return 0
  if args.provider is None:
    parser.error("provider is required unless --self-test is used")
  try:
    report, passed = Probe(args.provider, args.ptxas, args.inspect_only)
    PrintReport(report)
    if args.json:
      args.json.parent.mkdir(parents=True, exist_ok=True)
      args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if passed else 2
  except (OSError, ValueError, RuntimeError) as error:
    print(f"probe=FAIL: {error}")
    return 2


if __name__ == "__main__":
  raise SystemExit(Main())
