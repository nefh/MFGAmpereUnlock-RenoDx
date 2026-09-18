#!/usr/bin/env python3
"""Read-only probe for the complete Blackwell temporal framework.

The failed standalone ISR experiment proved that Kernel_EstimateIntermMvecsScatter
cannot replace the existing midpoint backend by itself. This probe inspects the
exact DLSS-G 310.9.1 provider, finds the Blackwell framework kernels that upstream
uses as one temporal backend, retargets their sm_120 PTX to an explicit sm_86 or
sm_75 qualification target, and assembles all variants without modifying the
provider or writing NVIDIA payloads outside a temporary directory.
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

EXPECTED_KERNEL_BY_ROLE = {
  "motion_vector": "Kernel_EstimateIntermMvecsScatter",
  "inpaint": "Kernel_Prev2CurrUnpackPull",
  "inpaint_decision": "Kernel_OutputPull",
}

EXPECTED_PTX_BY_ROLE = {
  "motion_vector": (90731, 0xB1A2811B29625D41),
  "inpaint": (26439, 0x546151924160B69B),
  "inpaint_decision": (23116, 0x9B47635B91B2436B),
}

BOUNDARY_REGISTER_ANCHOR = ".reg .pred %p<656>;\n"
BOUNDARY_BALANCED_MARKER = "MFGUNLOCK_BOUNDARY_ARTIFACT_BALANCED_V1"
BOUNDARY_AGGRESSIVE_MARKER = "MFGUNLOCK_BOUNDARY_ARTIFACT_AGGRESSIVE_V1"

BOUNDARY_DIRECTIONS = {
  "CURR_TO_PREV": {
    "anchor": "fma.rn.ftz.f32 %f14, %f7, %f7, %f157;\n",
    "center": ("%f7", "%f8", "%f9"),
    "neighbors": [
      ("%f55", "%f56", "%f57"),
      ("%f78", "%f79", "%f80"),
      ("%f97", "%f98", "%f99"),
      ("%f120", "%f121", "%f122"),
    ],
    "length": "%f14",
    "reload": False,
  },
  "PREV_TO_CURR": {
    "anchor": "fma.rn.ftz.f32 %f22, %f15, %f15, %f942;\n",
    "center": ("%f15", "%f16", "%f17"),
    "neighbors": [
      ("%f840", "%f841", "%f842"),
      ("%f863", "%f864", "%f865"),
      ("%f882", "%f883", "%f884"),
      ("%f905", "%f906", "%f907"),
    ],
    "length": "%f22",
    "reload": True,
  },
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


def CompileVariant(ptxas: Path | None, source: str, target_sm: int, name: str, directory: Path):
  result = common.compile_variant(ptxas, source, target_sm, name, directory)
  result["entrypoint"] = scatter.EntryName(source.encode("ascii")) or ""
  path = directory / f"{name}-sm_{target_sm}.cubin"
  return result, path.read_bytes() if path.exists() else b""


def BoundaryProgram(direction: str, aggressive: bool) -> str:
  spec = BOUNDARY_DIRECTIONS[direction]
  center_x, center_y, center_depth = spec["center"]
  marker = BOUNDARY_AGGRESSIVE_MARKER if aggressive else BOUNDARY_BALANCED_MARKER
  depth_limit = "0f40000000" if aggressive else "0f40400000"
  lines = [f"// {marker}_{direction}"]
  if spec["reload"]:
    lines.append(scatter.ISR_ANCHOR.rstrip("\n"))
  lines += [
    "mov.f32 %qgf0, 0f00000000;",
    f"div.approx.ftz.f32 %qgf2, {spec['length']}, %f2;",
    "max.ftz.f32 %qgf2, %qgf2, 0f3F800000;",
  ]
  for neighbor_x, neighbor_y, neighbor_depth in spec["neighbors"]:
    lines += [
      f"sub.ftz.f32 %qgf3, {neighbor_x}, {center_x};",
      f"sub.ftz.f32 %qgf4, {neighbor_y}, {center_y};",
      "mul.ftz.f32 %qgf5, %qgf4, %qgf4;",
      "fma.rn.ftz.f32 %qgf5, %qgf3, %qgf3, %qgf5;",
      "div.approx.ftz.f32 %qgf6, %qgf5, %qgf2;",
      "sub.ftz.f32 %qgf6, 0f3F800000, %qgf6;",
      "setp.gt.f32 %qgp0, %qgf6, 0f00000000;",
      f"sub.ftz.f32 %qgf7, {neighbor_depth}, {center_depth};",
      "abs.ftz.f32 %qgf7, %qgf7;",
      f"setp.lt.and.f32 %qgp0, %qgf7, {depth_limit}, %qgp0;",
      "@!%qgp0 mov.f32 %qgf6, 0f00000000;",
    ]
    if aggressive:
      lines.append("add.f32 %qgf0, %qgf0, %qgf6;")
    else:
      lines += [
        "min.ftz.f32 %qgf6, %qgf6, 0f3F800000;",
        "max.f32 %qgf0, %qgf0, %qgf6;",
      ]
  if aggressive:
    lines += [
      "sub.f32 %qgf0, %qgf0, 0f3F800000;",
      "max.f32 %qgf0, %qgf0, 0f00000000;",
      "min.f32 %qgf0, %qgf0, 0f3F800000;",
      "mul.f32 %qgf0, %qgf0, %qgf0;",
      "fma.rn.f32 %qgf11, %qgf0, 0fBE800000, 0f3F800000;",
    ]
  else:
    lines.append("fma.rn.f32 %qgf11, %qgf0, 0fBF000000, 0f3F800000;")
  lines.append("mul.ftz.f32 %f2, %f2, %qgf11;")
  return "\n".join(lines) + "\n"


def PatchBoundaryArtifactMitigation(source: str, aggressive: bool) -> str:
  source = common.replace_once(
      source,
      BOUNDARY_REGISTER_ANCHOR,
      BOUNDARY_REGISTER_ANCHOR + ".reg .pred %qgp<2>;\n.reg .f32 %qgf<12>;\n",
      "boundary register declaration")
  for direction, spec in BOUNDARY_DIRECTIONS.items():
    source = common.replace_once(
        source,
        spec["anchor"],
        spec["anchor"] + BoundaryProgram(direction, aggressive),
        f"{direction} boundary insertion")
  return source



GENERIC_PACKED_HALF_PROFILES = {
  "Kernel_BlendCandidatesFused": (42029, 0x9B5219CDD66E0969, 20),
  "Kernel_DL1Net_Input": (10036, 0x87E83571B0C08CBE, 1),
  "Kernel_OutputPull": (30175, 0x6578876BE2B5FBE2, 8),
  "Kernel_OutputPullMiddle": (11165, 0x1D6BA2D7FD03152E, 8),
  "Kernel_OutputPush": (52834, 0x62A7B5EE15C6F3E1, 2),
}


TURING_MMA_PROFILES = {
  (20437, 0xCE2C5C3F5D55CD81): (18, (4, 4, 0, 0)),
  (11638, 0x75FE3A8642B3E477): (9, (0, 0, 4, 2)),
  (16101, 0xD932CB22DD83419E): (18, (0, 0, 4, 2)),
  (15662, 0x6702B594AF03CAEC): (18, (0, 0, 4, 2)),
  (16341, 0x8EB26063A973182C): (12, (0, 0, 4, 2)),
  (13231, 0xBC841338134753C6): (12, (0, 0, 4, 2)),
  (14603, 0xDDE1C125651EB889): (9, (0, 0, 4, 2)),
  (11750, 0xFDFE9B7351889BFD): (9, (0, 0, 4, 2)),
  (14684, 0xC83BDCE19A1A249F): (18, (0, 0, 4, 2)),
  (14693, 0x400B19B43610CCE1): (18, (0, 0, 4, 2)),
  (17609, 0xE0BA6D48FB005CC1): (9, (4, 4, 0, 0)),
  (33114, 0xA582DB83FC911FE0): (32, (8, 0, 0, 0)),
  (16453, 0xFB821A89F2997ECE): (16, (16, 0, 0, 0)),
  (32740, 0x802242E77FAE47CB): (36, (16, 0, 0, 0)),
  (12511, 0xE52F68BF382C1392): (8, (16, 0, 0, 0)),
  (9881, 0xDBA2CDA2F6AF70A5): (4, (0, 0, 0, 0)),
  (56261, 0x80FCCB8DA5F37636): (108, (48, 0, 0, 0)),
  (12754, 0xE1FD0775658C8DD3): (8, (16, 0, 0, 0)),
  (59794, 0x58F45C0876B5B7C5): (64, (8, 0, 0, 0)),
  (33295, 0x3E454577F62ECF8F): (36, (16, 0, 0, 0)),
  (43831, 0x6E407251363CF2A5): (48, (8, 0, 0, 0)),
  (33114, 0x625EC319E7D0A5C9): (32, (8, 0, 0, 0)),
  (33114, 0xE5FA510399D45224): (32, (8, 0, 0, 0)),
  (20153, 0xDEF932BB960C28D0): (16, (8, 0, 0, 0)),
  (24116, 0x5702CF69C73D6AF4): (32, (16, 0, 0, 0)),
  (24206, 0xDC473E258540EF53): (32, (16, 0, 0, 0)),
  (16214, 0x92F1A598E2B2254C): (16, (16, 0, 0, 0)),
}



def ProbeProviderLowering(provider: Path, ptxas: Path, target_sm: int, directory: Path):
  data = provider.read_bytes()
  layout = scatter.ReadPeLayout(data)
  sections = [(item.raw_offset, item.raw_size, "provider") for item in layout.sections
              if item.characteristics & scatter.IMAGE_SCN_MEM_READ and
              not item.characteristics & scatter.IMAGE_SCN_MEM_EXECUTE]
  results = []
  packed_seen = set()
  mma_seen = set()
  eligible = 0
  rejected = []
  for start, end in common.iter_fatbins(data, sections, data_only=False):
    entries = list(common.iter_entries(data, start, end))
    image_set = [(e[0], e[1]) for e in entries]
    if image_set not in ([(1, 120), (1, 89), (2, 89)], [(1, 89)]):
      rejected.append({"offset": hex(start), "reason": "unreviewed image set"})
      continue
    eligible += 1
    entry = entries[1 if len(entries) == 3 else 0]
    source_bytes = scatter.DecodePtx(data, entry)
    source = source_bytes.decode("ascii")
    kernel = scatter.EntryName(source_bytes) or ""

    if ".f16x2.f32" in source:
      profile = GENERIC_PACKED_HALF_PROFILES.get(kernel)
      if (profile is None or kernel in packed_seen or len(source_bytes) != profile[0] or
          common.fnv1a64(source_bytes) != profile[1]):
        raise ValueError(f"unqualified SM89 packed-half program: {kernel}")
      packed_seen.add(kernel)
      before = common.unsupported_features(source, target_sm)
      lowered = common.lower_packed_half(source, target_sm, profile[2])
      target = common.retarget_ptx(lowered, ADA_ARCH, target_sm)
      item, _ = CompileVariant(ptxas, target, target_sm, "provider-packed-" + kernel, directory)
      count = profile[2] if target_sm == 75 else 0
      common.add_lowering_result(item, {
          "lowering_kind": "packed_half",
          "lowering_applied": count != 0,
          "lowered_instruction_count": count,
          "unsupported_features_before_lowering": before,
          "unsupported_features_after_lowering": common.unsupported_features(target, target_sm),
      })
      header = entry[2] - (entries[0][2] + entries[0][3])
      rebuilt_size = 16 + header + ((len(target.encode("ascii")) + 1 + 7) & ~7)
      item.update({"source": Hashes(source_bytes), "fatbin_offset": hex(start),
                   "physical_bytes": end - start, "rebuilt_visible_bytes": rebuilt_size,
                   "fits_physical_fatbin": rebuilt_size <= end - start})
      results.append(item)
      continue

    if target_sm == 75 and common.MMA16816 in source:
      identity = (len(source_bytes), common.fnv1a64(source_bytes))
      profile = TURING_MMA_PROFILES.get(identity)
      if image_set != [(1, 89)] or profile is None or identity in mma_seen:
        raise ValueError(f"unqualified SM89 m16n8k16 program: {kernel} at {start:#x}")
      count, half_counts = profile
      mma_seen.add(identity)
      before = common.unsupported_features(source, target_sm)
      lowered = common.lower_turing_mma16816(source, target_sm, count)
      lowered = common.lower_turing_half_minmax(lowered, target_sm, half_counts)
      lowered = common.compact_provider_ptx(lowered)
      target = common.retarget_ptx(lowered, ADA_ARCH, target_sm)
      item, _ = CompileVariant(ptxas, target, target_sm, f"provider-mma-{start:08x}", directory)
      raw = (target + "\0").encode("ascii")
      try:
        compressed, compression_mode = common.lz4_compress_provider_fit(raw, entry[3])
        compression_detail = ""
      except ValueError as error:
        compressed = b""
        compression_mode = "failed"
        compression_detail = str(error)
      half_count = sum(half_counts) if target_sm == 75 else 0
      item.update({
          "lowering_kind": "mma_m16n8k16_plus_half_minmax",
          "lowering_applied": True,
          "lowered_instruction_count": count + half_count,
          "lowered_mma_instruction_count": count,
          "lowered_half_minmax_instruction_count": half_count,
          "half_minmax_counts": {
              "scalar_max": half_counts[0], "scalar_min": half_counts[1],
              "packed_max": half_counts[2], "packed_min": half_counts[3],
          },
          "unsupported_features_before_lowering": before,
          "unsupported_features_after_lowering": common.unsupported_features(target, target_sm),
          "source": Hashes(source_bytes),
          "fatbin_offset": hex(start),
          "physical_bytes": end - start,
          "compressed_capacity": entry[3],
          "compressed_bytes": len(compressed),
          "compression_slack": entry[3] - len(compressed) if compressed else -1,
          "compression_mode": compression_mode,
          "compression_detail": compression_detail,
          "fits_physical_fatbin": bool(compressed),
      })
      if item["status"] == "PASS":
        item["qualification"] = "PASS after strict SM75 MMA and half min/max lowering"
      results.append(item)
      continue

    features = common.unsupported_features(source, target_sm)
    if features:
      rejected.append({"kernel": kernel, "unsupported_features": features})

  packed_results = [r for r in results if r.get("lowering_kind") == "packed_half"]
  packed_structure = (packed_seen == set(GENERIC_PACKED_HALF_PROFILES) and
                      len(packed_results) == len(GENERIC_PACKED_HALF_PROFILES) and
                      all(r["fits_physical_fatbin"] and
                          not r["unsupported_features_after_lowering"] for r in packed_results))
  packed_compile = ptxas is not None and all(r["status"] == "PASS" for r in packed_results)
  packed_pass = packed_structure and packed_compile
  mma_results = [r for r in results if r.get("lowering_kind") == "mma_m16n8k16_plus_half_minmax"]
  mma_structure = (target_sm != 75 or
                   (mma_seen == set(TURING_MMA_PROFILES) and
                    len(mma_results) == len(TURING_MMA_PROFILES) and
                    sum(r.get("lowered_mma_instruction_count", 0) for r in mma_results) == 670 and
                    sum(r.get("lowered_half_minmax_instruction_count", 0) for r in mma_results) == 294 and
                    all(r["fits_physical_fatbin"] and not r["unsupported_features_after_lowering"]
                        for r in mma_results)))
  mma_compile = target_sm != 75 or (ptxas is not None and
                                     all(r["status"] == "PASS" for r in mma_results))
  mma_pass = mma_structure and mma_compile
  static_pass = packed_structure and mma_structure and eligible == 70 and not rejected
  compile_pass = packed_pass and mma_pass and static_pass
  status = "PASS" if compile_pass else ("NOT_RUN" if ptxas is None and static_pass else "FAIL")
  return {"status": status, "target_sm": target_sm,
          "packed_half_programs": "NOT_RUN" if ptxas is None else ("PASS" if packed_pass else "FAIL"),
          "mma_programs": "NOT_RUN" if ptxas is None else ("PASS" if mma_pass else "FAIL"),
          "half_minmax_programs": "NOT_RUN" if ptxas is None else ("PASS" if mma_pass else "FAIL"),
          "static_qualification": "PASS" if static_pass else "FAIL",
          "lowered_mma_instruction_count": sum(r.get("lowered_mma_instruction_count", 0) for r in mma_results),
          "lowered_half_minmax_instruction_count": sum(r.get("lowered_half_minmax_instruction_count", 0) for r in mma_results),
          "eligible_fatbins": eligible, "unchanged_rejections": rejected,
          "changed_programs": results}


def Probe(provider: Path, ptxas_arg: str | None, target_sm: int = 86,
          audit_provider: bool = False, assemble: bool = True):
  if target_sm not in (75, 86):
    raise ValueError(f"unsupported probe target sm_{target_sm}")
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
  if any(len(items) != 1 for items in by_role.values()):
    raise ValueError(
        f"found {len(by_role['motion_vector'])} motion-vector candidates, expected one")
  if len(by_role["inpaint"]) > 1 or len(by_role["inpaint_decision"]) > 1:
    raise ValueError(
        "ambiguous optional framework candidates: "
        f"inpaint={len(by_role['inpaint'])}, "
        f"decision={len(by_role['inpaint_decision'])}")

  for role, items in by_role.items():
    if len(items) != 1:
      continue
    candidate = items[0]
    expected_kernel = EXPECTED_KERNEL_BY_ROLE[role]
    if candidate.kernel != expected_kernel:
      raise ValueError(
          f"{role} entrypoint mismatch: expected {expected_kernel}; got {candidate.kernel}")
    expected_size, expected_fnv = EXPECTED_PTX_BY_ROLE[role]
    actual_fnv = common.fnv1a64(candidate.blackwell_ptx)
    if len(candidate.blackwell_ptx) != expected_size or actual_fnv != expected_fnv:
      raise ValueError(
          f"{role} PTX identity mismatch: expected size={expected_size} "
          f"fnv=0x{expected_fnv:016x}; got size={len(candidate.blackwell_ptx)} "
          f"fnv=0x{actual_fnv:016x}")

  ptxas = common.find_ptxas(ptxas_arg) if assemble else None
  report = {
      "provider": str(provider),
      "provider_sha256": provider_sha256,
      "provider_file_size": len(data),
      "pe_timestamp": f"0x{layout.timestamp:08x}",
      "pe_image_size": layout.image_size,
      "ptxas": str(ptxas) if ptxas is not None else None,
      "target_sm": target_sm,
      "candidate_counts": {role: len(items) for role, items in by_role.items()},
      "candidates": [],
  }
  prefix = f"sm{target_sm}"

  failed = False
  with tempfile.TemporaryDirectory(prefix="mfg-blackwell-temporal-") as temp:
    directory = Path(temp)
    for index, candidate in enumerate(candidates):
      source_bytes = candidate.blackwell_ptx
      source = source_bytes.decode("ascii")
      target, lowering = common.prepare_compatible_ptx(
          source, target_sm, 8 if candidate.role == "inpaint_decision" else 0)
      baseline, baseline_blob = CompileVariant(
          ptxas, target, target_sm, f"{index:02d}-{candidate.role}-baseline", directory)
      common.add_lowering_result(baseline, lowering)
      baseline["fits_ada_slot"] = len(baseline_blob) <= len(candidate.ada_cubin) if baseline_blob else False

      cubin_fingerprint = common.fingerprint_elf(candidate.ada_cubin)
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
          f"{prefix}_baseline": baseline,
      }

      if candidate.role == "motion_vector":
        anchor_counts = {
            "divisor": source.count(scatter.ISR_ANCHOR),
            "registers": source.count(BOUNDARY_REGISTER_ANCHOR),
            **{direction.lower(): source.count(spec["anchor"])
               for direction, spec in BOUNDARY_DIRECTIONS.items()},
        }
        entry["boundary_anchor_counts"] = anchor_counts
        if any(count != 1 for count in anchor_counts.values()):
          failure = {
              "status": "FAIL",
              "detail": f"boundary anchor counts are {anchor_counts}, expected all one",
          }
          entry[f"{prefix}_intermediate_scatter"] = failure
          entry[f"{prefix}_boundary_balanced"] = failure
          entry[f"{prefix}_boundary_aggressive"] = failure
          failed = True
        else:
          off_source = common.retarget_ptx(
              scatter.PatchIntermediateScatter(source), BLACKWELL_ARCH, target_sm)
          off, off_blob = CompileVariant(
              ptxas, off_source, target_sm,
              f"{index:02d}-{candidate.role}-boundary-off", directory)
          off["fits_ada_slot"] = len(off_blob) <= len(candidate.ada_cubin) if off_blob else False
          common.add_lowering_result(off, lowering)
          entry[f"{prefix}_intermediate_scatter"] = off

          balanced_source = common.retarget_ptx(
              PatchBoundaryArtifactMitigation(source, aggressive=False), BLACKWELL_ARCH, target_sm)
          balanced, balanced_blob = CompileVariant(
              ptxas, balanced_source, target_sm,
              f"{index:02d}-{candidate.role}-boundary-balanced", directory)
          balanced["fits_ada_slot"] = (
              len(balanced_blob) <= len(candidate.ada_cubin) if balanced_blob else False)
          common.add_lowering_result(balanced, lowering)
          entry[f"{prefix}_boundary_balanced"] = balanced

          aggressive_source = common.retarget_ptx(
              PatchBoundaryArtifactMitigation(source, aggressive=True), BLACKWELL_ARCH, target_sm)
          aggressive, aggressive_blob = CompileVariant(
              ptxas, aggressive_source, target_sm,
              f"{index:02d}-{candidate.role}-boundary-aggressive", directory)
          aggressive["fits_ada_slot"] = (
              len(aggressive_blob) <= len(candidate.ada_cubin) if aggressive_blob else False)
          common.add_lowering_result(aggressive, lowering)
          entry[f"{prefix}_boundary_aggressive"] = aggressive

          failed |= any(item["status"] != "PASS" for item in (off, balanced, aggressive))

      report["candidates"].append(entry)
      failed |= baseline["status"] != "PASS"
      failed |= entry["preferred_va_refs_offline"] == 0

    if audit_provider:
      report["provider_lowering_probe"] = ProbeProviderLowering(provider, ptxas, target_sm, directory)
  report["full_temporal_probe"] = "NOT_RUN" if not assemble else ("FAIL" if failed else "PASS")
  return report


def SelfTest() -> None:
  common.packed_half_self_test()
  common.turing_mma_self_test()
  assert len(TURING_MMA_PROFILES) == 27
  assert sum(profile[0] for profile in TURING_MMA_PROFILES.values()) == 670
  assert sum(sum(profile[1]) for profile in TURING_MMA_PROFILES.values()) == 294
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
  assert common.retarget_ptx(fixture, 120, 86).count(".target sm_86") == 1
  assert common.retarget_ptx(fixture, 120, 75).count(".target sm_75") == 1
  assert common.turing_unsupported_features("cp.async.ca.shared.global [x], [y], 16;") == ["cp.async"]
  assert common.turing_unsupported_features("max.f16 %rs0,%rs1,%rs2;") == ["max.f16"]
  assert common.turing_unsupported_features("min.f16x2 %r0,%r1,%r2;") == ["min.f16"]
  assert common.unsupported_features("wgmma.mma_async.sync.aligned;", 86) == ["wgmma."]

  boundary_fixture = (
      ".version 8.7\n"
      ".target sm_120\n"
      + BOUNDARY_REGISTER_ANCHOR +
      ".visible .entry Kernel_EstimateIntermMvecsScatter() {\n"
      + scatter.ISR_ANCHOR +
      BOUNDARY_DIRECTIONS["CURR_TO_PREV"]["anchor"] +
      BOUNDARY_DIRECTIONS["PREV_TO_CURR"]["anchor"] +
      "ret;\n}\n"
  )
  balanced = PatchBoundaryArtifactMitigation(boundary_fixture, aggressive=False)
  assert balanced.count(BOUNDARY_BALANCED_MARKER) == 2
  assert "0fBF000000" in balanced
  assert "0f40400000" in balanced
  assert "MFGUNLOCK_INTERMEDIATE_SCATTER_V1" not in balanced
  aggressive = PatchBoundaryArtifactMitigation(boundary_fixture, aggressive=True)
  assert aggressive.count(BOUNDARY_AGGRESSIVE_MARKER) == 2
  assert "0fBE800000" in aggressive
  assert "0f40000000" in aggressive
  assert "mul.f32 %qgf0, %qgf0, %qgf0;" in aggressive
  print("self_test=PASS")


def main() -> int:
  parser = argparse.ArgumentParser()
  parser.add_argument("provider", nargs="?", type=Path)
  parser.add_argument("--ptxas")
  parser.add_argument("--no-assemble", action="store_true", help="static audit only; never reports compile PASS")
  parser.add_argument("--target-sm", type=int, choices=(75, 86), default=86)
  parser.add_argument("--json", type=Path)
  parser.add_argument("--audit-provider", action="store_true",
                      help="qualify the exact SM89 provider compatibility programs as well")
  parser.add_argument("--self-test", action="store_true")
  args = parser.parse_args()

  if args.self_test:
    SelfTest()
    return 0
  if args.provider is None:
    parser.error("provider is required unless --self-test is used")

  report = Probe(args.provider, args.ptxas, args.target_sm, args.audit_provider, not args.no_assemble)
  print(f"provider={report['provider']}")
  print(f"provider_sha256={report['provider_sha256']}")
  print(f"pe_timestamp={report['pe_timestamp']} image_size={report['pe_image_size']}")
  print(f"target_sm={report['target_sm']}")
  counts = report["candidate_counts"]
  print("candidate_counts=" + ", ".join(f"{key}={value}" for key, value in counts.items()))
  prefix = f"sm{report['target_sm']}"
  for candidate in report["candidates"]:
    baseline = candidate[f"{prefix}_baseline"]
    line = (
        f"{candidate['role']}: kernel={candidate['kernel']} "
        f"fatbin={candidate['fatbin_offset']} refs={candidate['preferred_va_refs_offline']} "
        f"slot={candidate['ada_cubin_slot']['size']} "
        f"baseline={baseline['status']} size={baseline['size']} "
        f"fits_slot={'PASS' if baseline['fits_ada_slot'] else 'NO'}")
    print(line)
    if baseline.get("unsupported_features"):
      print(f"  unsupported_features={','.join(baseline['unsupported_features'])}")
    if baseline.get("detail") and baseline["status"] != "PASS":
      print(f"  baseline: {baseline['detail']}")
    if candidate["role"] == "motion_vector":
      isr = candidate[f"{prefix}_intermediate_scatter"]
      print(
          f"  boundary_off={isr['status']} size={isr.get('size', 0)} "
          f"fits_slot={'PASS' if isr.get('fits_ada_slot', False) else 'NO'}")
      for key, label in ((f"{prefix}_boundary_balanced", "boundary_balanced"),
                         (f"{prefix}_boundary_aggressive", "boundary_aggressive")):
        item = candidate[key]
        print(
            f"  {label}={item['status']} size={item.get('size', 0)} "
            f"registers={item.get('registers', 0)} "
            f"shared={item.get('shared_memory_bytes', 0)} "
            f"fits_slot={'PASS' if item.get('fits_ada_slot', False) else 'NO'}")
        if item.get("unsupported_features"):
          print(f"    unsupported_features={','.join(item['unsupported_features'])}")
        if item.get("detail") and item["status"] != "PASS":
          print(f"    {item['detail']}")
  print(f"full_temporal_probe={report['full_temporal_probe']}")

  if "provider_lowering_probe" in report:
    print("provider_lowering_probe=" + report["provider_lowering_probe"]["status"])
    print("  static_qualification=" + report["provider_lowering_probe"].get("static_qualification", ""))
    print("  packed_half_programs=" + report["provider_lowering_probe"]["packed_half_programs"])
    print("  mma_programs=" + report["provider_lowering_probe"].get("mma_programs", "NOT_RUN"))
    print("  half_minmax_programs=" + report["provider_lowering_probe"].get("half_minmax_programs", "NOT_RUN"))
    print("  lowered_mma_instruction_count=" + str(report["provider_lowering_probe"].get("lowered_mma_instruction_count", 0)))
    print("  lowered_half_minmax_instruction_count=" + str(report["provider_lowering_probe"].get("lowered_half_minmax_instruction_count", 0)))
    print("  existing_guard_rejections=" + str(len(report["provider_lowering_probe"]["unchanged_rejections"])))
    for item in report["provider_lowering_probe"]["changed_programs"]:
      print(f"  {item['entrypoint']}: {item['status']} kind={item.get('lowering_kind', '')} "
            f"lowered={item['lowered_instruction_count']} fits_physical_fatbin={item['fits_physical_fatbin']}" +
            (f" compressed={item.get('compressed_bytes', 0)}/{item.get('compressed_capacity', 0)}"
             f" mode={item.get('compression_mode', '')}"
             if item.get('lowering_kind') == 'mma_m16n8k16_plus_half_minmax' else ""))
      if item["status"] != "PASS":
        print(item["detail"])
  if args.json:
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"json={args.json}")

  passed = report["full_temporal_probe"] == "PASS"
  passed &= report.get("provider_lowering_probe", {}).get("status", "PASS") == "PASS"
  return 0 if passed else 2


if __name__ == "__main__":
  raise SystemExit(main())
