#!/usr/bin/env python3
"""Read-only probe for the complete Blackwell temporal framework on Ampere.

The failed standalone ISR experiment proved that Kernel_EstimateIntermMvecsScatter
cannot replace the existing midpoint backend by itself. This probe inspects the
exact DLSS-G 310.9.1 provider, finds the Blackwell framework kernels that upstream
uses as one temporal backend, retargets their sm_120 PTX to sm_86, and assembles
all variants without modifying the provider or writing NVIDIA payloads outside a
temporary directory.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import tempfile

import probe_intermediate_scatter as scatter
import probe_validated_warp as common

EXPECTED_SHA256 = "ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82"
EXPECTED_TIMESTAMP = 0x6A986031
EXPECTED_IMAGE_SIZE = 7_565_312
ADA_ARCH = 89
BLACKWELL_ARCH = 120
CUBIN_KIND = 2

ROLE_BY_SHARED = {
  7776: "motion_vector",
  3920: "inpaint",
  784: "inpaint_decision",
}


@dataclass(frozen=True)
class Candidate:
  role: str
  kernel: str
  start: int
  end: int
  entries: tuple[tuple[int, int, int, int, int, int], ...]
  blackwell_ptx: bytes
  ada_cubin: bytes


def GetEntries(entries, kind: int, architecture: int):
  return [entry for entry in entries if entry[0] == kind and entry[1] == architecture]


def ScanCandidates(provider: Path):
  data, sections = common.pe_sections(provider)
  candidates = []
  for start, end in common.iter_fatbins(data, sections):
    entries = tuple(common.iter_entries(data, start, end))
    blackwell_entries = GetEntries(entries, common.PTX_KIND, BLACKWELL_ARCH)
    cubin_entries = GetEntries(entries, CUBIN_KIND, ADA_ARCH)
    if len(blackwell_entries) != 1 or len(cubin_entries) != 1:
      continue

    blackwell_ptx = scatter.DecodePtx(data, blackwell_entries[0])
    kernel = scatter.EntryName(blackwell_ptx)
    if not kernel:
      continue

    cubin_entry = cubin_entries[0]
    ada_cubin = data[cubin_entry[2]:cubin_entry[2] + cubin_entry[3]]
    try:
      _, shared, _ = scatter.FingerprintElf(ada_cubin)
    except ValueError:
      continue
    role = ROLE_BY_SHARED.get(shared)
    if role is None:
      continue

    candidates.append(Candidate(role, kernel, start, end, entries,
                                blackwell_ptx, ada_cubin))
  return data, candidates


def Hashes(blob: bytes):
  return {
      "size": len(blob),
      "fnv1a64": f"0x{common.fnv1a64(blob):016x}",
      "sha256": hashlib.sha256(blob).hexdigest(),
  }


def CompileVariant(ptxas: Path, source: str, name: str, directory: Path):
  ok, blob, detail = scatter.Compile(ptxas, source, name, directory)
  result = {
      "status": "PASS" if ok else "FAIL",
      "size": len(blob),
      "sha256": hashlib.sha256(blob).hexdigest() if blob else "",
      "detail": detail,
  }
  if blob:
    try:
      result["elf"] = list(scatter.FingerprintElf(blob))
    except ValueError as error:
      result["elf_error"] = str(error)
  return result, blob


def Probe(provider: Path, ptxas_arg: str | None):
  data, candidates = ScanCandidates(provider)
  layout = scatter.ReadPeLayout(data)
  provider_sha256 = hashlib.sha256(data).hexdigest()
  if provider_sha256 != EXPECTED_SHA256:
    raise ValueError(
        f"provider SHA256 mismatch: expected {EXPECTED_SHA256}; got {provider_sha256}")
  if layout.timestamp != EXPECTED_TIMESTAMP or layout.image_size != EXPECTED_IMAGE_SIZE:
    raise ValueError(
        "provider metadata mismatch: expected timestamp "
        f"0x{EXPECTED_TIMESTAMP:08x}, image-size {EXPECTED_IMAGE_SIZE}; got "
        f"0x{layout.timestamp:08x}, {layout.image_size}")

  by_role = {role: [candidate for candidate in candidates if candidate.role == role]
             for role in ROLE_BY_SHARED.values()}
  if len(by_role["motion_vector"]) != 1:
    raise ValueError(
        f"found {len(by_role['motion_vector'])} motion-vector candidates, expected one")
  if len(by_role["inpaint"]) > 1 or len(by_role["inpaint_decision"]) > 1:
    raise ValueError(
        "ambiguous optional framework candidates: "
        f"inpaint={len(by_role['inpaint'])}, "
        f"decision={len(by_role['inpaint_decision'])}")

  ptxas = common.find_ptxas(ptxas_arg)
  report = {
      "provider": str(provider),
      "provider_sha256": provider_sha256,
      "provider_file_size": len(data),
      "pe_timestamp": f"0x{layout.timestamp:08x}",
      "pe_image_size": layout.image_size,
      "ptxas": str(ptxas),
      "candidate_counts": {role: len(items) for role, items in by_role.items()},
      "candidates": [],
  }

  failed = False
  with tempfile.TemporaryDirectory(prefix="mfg-blackwell-temporal-") as temp:
    directory = Path(temp)
    for index, candidate in enumerate(candidates):
      source_bytes = candidate.blackwell_ptx
      source = source_bytes.decode("ascii")
      target = scatter.Retarget(source, BLACKWELL_ARCH)
      baseline, baseline_blob = CompileVariant(
          ptxas, target, f"{index:02d}-{candidate.role}-baseline", directory)
      baseline["fits_ada_slot"] = len(baseline_blob) <= len(candidate.ada_cubin) if baseline_blob else False

      cubin_fingerprint = scatter.FingerprintElf(candidate.ada_cubin)
      entry = {
          "role": candidate.role,
          "kernel": candidate.kernel,
          "fatbin_offset": f"0x{candidate.start:x}",
          "fatbin_rva": f"0x{scatter.RawOffsetToRva(layout, candidate.start):x}",
          "fatbin_size": candidate.end - candidate.start,
          "layout": [[item[0], item[1]] for item in candidate.entries],
          "preferred_va_refs_offline": scatter.CountPreferredVaReferences(
              data, layout, candidate.start),
          "blackwell_ptx": Hashes(source_bytes),
          "ada_cubin_slot": {
              **Hashes(candidate.ada_cubin),
              "elf": list(cubin_fingerprint),
          },
          "sm86_baseline": baseline,
      }

      if candidate.role == "motion_vector":
        anchor_count = source.count(scatter.ISR_ANCHOR)
        entry["intermediate_scatter_anchor_count"] = anchor_count
        if anchor_count == 1:
          patched = scatter.PatchIntermediateScatter(source)
          patched = scatter.Retarget(patched, BLACKWELL_ARCH)
          isr, isr_blob = CompileVariant(
              ptxas, patched, f"{index:02d}-{candidate.role}-isr", directory)
          isr["fits_ada_slot"] = len(isr_blob) <= len(candidate.ada_cubin) if isr_blob else False
          entry["sm86_intermediate_scatter"] = isr
          failed |= isr["status"] != "PASS"
        else:
          entry["sm86_intermediate_scatter"] = {
              "status": "FAIL",
              "detail": f"ISR anchor count is {anchor_count}, expected one",
          }
          failed = True

      report["candidates"].append(entry)
      failed |= baseline["status"] != "PASS"
      failed |= entry["preferred_va_refs_offline"] == 0

  report["full_temporal_probe"] = "FAIL" if failed else "PASS"
  return report


def SelfTest() -> None:
  assert ROLE_BY_SHARED[7776] == "motion_vector"
  assert ROLE_BY_SHARED[3920] == "inpaint"
  assert ROLE_BY_SHARED[784] == "inpaint_decision"
  fixture = (
      ".version 8.7\n"
      ".target sm_120\n"
      ".visible .entry Kernel_EstimateIntermMvecsScatter() {\n"
      + scatter.ISR_ANCHOR +
      "ret;\n}\n"
  )
  patched = scatter.PatchIntermediateScatter(fixture)
  assert patched.count("0f3F000000") == 1
  assert scatter.Retarget(fixture, 120).count(".target sm_86") == 1
  print("self_test=PASS")


def main() -> int:
  parser = argparse.ArgumentParser()
  parser.add_argument("provider", nargs="?", type=Path)
  parser.add_argument("--ptxas")
  parser.add_argument("--json", type=Path)
  parser.add_argument("--self-test", action="store_true")
  args = parser.parse_args()

  if args.self_test:
    SelfTest()
    return 0
  if args.provider is None:
    parser.error("provider is required unless --self-test is used")

  report = Probe(args.provider, args.ptxas)
  print(f"provider={report['provider']}")
  print(f"provider_sha256={report['provider_sha256']}")
  print(f"pe_timestamp={report['pe_timestamp']} image_size={report['pe_image_size']}")
  counts = report["candidate_counts"]
  print("candidate_counts=" + ", ".join(f"{key}={value}" for key, value in counts.items()))
  for candidate in report["candidates"]:
    baseline = candidate["sm86_baseline"]
    line = (
        f"{candidate['role']}: kernel={candidate['kernel']} "
        f"fatbin={candidate['fatbin_offset']} refs={candidate['preferred_va_refs_offline']} "
        f"slot={candidate['ada_cubin_slot']['size']} "
        f"baseline={baseline['status']} size={baseline['size']} "
        f"fits_slot={'PASS' if baseline['fits_ada_slot'] else 'NO'}")
    print(line)
    if candidate["role"] == "motion_vector":
      isr = candidate["sm86_intermediate_scatter"]
      print(
          f"  intermediate_scatter={isr['status']} size={isr.get('size', 0)} "
          f"fits_slot={'PASS' if isr.get('fits_ada_slot', False) else 'NO'}")
  print(f"full_temporal_probe={report['full_temporal_probe']}")

  if args.json:
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"json={args.json}")

  return 0 if report["full_temporal_probe"] == "PASS" else 2


if __name__ == "__main__":
  raise SystemExit(main())
