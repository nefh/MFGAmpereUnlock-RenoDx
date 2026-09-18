# SM75 provider and quality qualification

## Scope

Exact DLSS-G provider SHA-256:
`ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82`.

The Turing path uses only fingerprinted PTX compatibility transforms. Unknown
sources, instruction forms, counts, register layouts, provider layouts, or
targets fail closed. The complete provider-wide `ptxas` gate is now green, so
production full Blackwell temporal / ISR / Boundary / Validated Warp dispatch
uses `sm_75`; midpoint remains the fail-closed fallback.

## Already qualified by real ptxas

The existing SM75 quality probes already pass for:

- `Kernel_EstimateIntermMvecsScatter` baseline;
- ISR / Boundary Off;
- Boundary Balanced and Aggressive;
- `Kernel_Prev2CurrUnpackPull`;
- `Kernel_OutputPull` after strict packed-half lowering;
- `Kernel_BlendCandidatesFused` baseline and Validated Warp after strict
  packed-half lowering.

The packed `cvt.rn.f16x2.f32` compatibility path is shared with five exact
provider programs and has also passed real `ptxas`.

## m16n8k16 compatibility split

Twenty-seven exact single-image SM89 provider programs contain 670 instances of
one qualified instruction form:

```text
mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16
```

For its inspected f16 fragment layout, K=16 is split into two K=8 operations:

```text
mma.m16n8k8 D, A0..A1, B0, C
mma.m16n8k8 D, A2..A3, B1, D
```

The second instruction accumulates the upper K-half onto the first result.
Output/input hazards for the second half are rejected. This is a functional
clean-room compatibility transformation; FP16 accumulation ordering is not
claimed bit-identical to a single K=16 operation.

A real SM75 `ptxas` run accepted the MMA split far enough to expose the next
instruction class in the same exact programs: f16/f16x2 `min` and `max`.

## f16 min/max compatibility lowering

Across the same 27 fingerprints there are exactly 294 qualified half-precision
min/max instructions:

```text
232  max.f16
  8  min.f16
 36  max.f16x2
 18  min.f16x2
```

Only the modifier-free forms observed in this provider are accepted. For SM75,
each half operand is widened exactly to f32, the corresponding default f32
`min`/`max` is executed, and the selected value is converted back to f16.
Packed forms are unpacked and processed lane-by-lane before being repacked.

This preserves the relevant default PTX semantics: one NaN selects the numeric
operand, two NaNs produce NaN, +0 orders above -0, and subnormals are not
explicitly flushed. NaN payload identity is not claimed. Unknown modifiers,
register forms, counts, or temporary-register collisions fail closed.

## Compressed provider rebuild

The compatibility programs are compressed single-image PTX fatbins. The normal
startup-only LZ4 encoder is tried first. Three exact programs become tight after
the combined MMA + half-min/max lowering, so a deterministic bounded
high-compression parser is used only when the fast encoder does not fit.

The fallback caps match-chain depth and lookahead, preserves the 64 KiB LZ4
history limit, and must fit the original payload. Both compressors round-trip
their output through the existing independent decoder before any replacement is
published. No external compression dependency or second fatbin framework is
introduced.

Static qualification on the exact provider reports:

```text
fatbins=70
retargetable=70
rejected=0
temporal_matches=1
offline_mfg_candidate=true

MMA instructions lowered=670
half min/max instructions lowered=294
remaining static unsupported features=0
```

The final real CUDA 12.8 `ptxas` run on the same provider also passed the
complete SM75 set:

```text
full_temporal_probe=PASS
provider_lowering_probe=PASS
packed_half_programs=PASS
mma_programs=PASS
half_minmax_programs=PASS
lowered_mma_instruction_count=670
lowered_half_minmax_instruction_count=294
existing_guard_rejections=0
validated_warp_probe=PASS
```

SM86 was rerun in parallel and remained free of compatibility lowering. Real
`ptxas` qualification is not a substitute for RTX20 execution.

## Reproducing the ptxas qualification

From the repository root on Windows:

```powershell
$Provider = "C:\Program Files (x86)\GOG Galaxy\Games\Cyberpunk 2077\bin\x64\nvngx_dlssg.dll"
$Ptxas = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\ptxas.exe"
$Out = "$env:USERPROFILE\Downloads"

python .\tools\probe_blackwell_temporal.py $Provider `
  --ptxas $Ptxas --target-sm 75 --audit-provider `
  --json "$Out\TEMPORAL_MINMAX_SM75.json"

python .\tools\probe_validated_warp.py $Provider `
  --ptxas $Ptxas --target-sm 75 `
  --json "$Out\WARP_MINMAX_SM75.json"
```

Expected SM75 result:

```text
full_temporal_probe=PASS
provider_lowering_probe=PASS
  packed_half_programs=PASS
  mma_programs=PASS
  half_minmax_programs=PASS
  lowered_mma_instruction_count=670
  lowered_half_minmax_instruction_count=294
  existing_guard_rejections=0
validated_warp_probe=PASS
```

Repeat both probes with `--target-sm 86`. SM86 must remain free of compatibility
lowering and retain the established temporal/Warp outputs.

`--no-assemble` is static audit only. It validates exact identities, rewrite
counts, compression fit and round-trip, but reports compile states as `NOT_RUN`.

## Production activation and remaining runtime gate

Production dispatch now reuses the existing builders with the active profile
`target_sm`: `86` on Ampere and `75` on Turing. ISR and Boundary
Off/Balanced/Aggressive are unchanged, Validated Warp uses the same exact
310.9.1 source qualification, and midpoint remains the fail-closed Turing
fallback when a full temporal plan cannot be prepared or committed safely.

The remaining support gate is physical RTX20 execution: fixed MFG, full
temporal, ISR/Boundary, Warp, then Dynamic MFG, including restart/restore,
artifact checks and longer-session stability.
