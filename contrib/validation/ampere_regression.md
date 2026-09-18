# Ampere regression checklist

## Scope and evidence

This iteration hardens the existing provider/admission/count/UI/temporal/Warp
boundaries. It does not add Boundary modes, change their PTX, promote Balanced to
default, change the driver/provider DLLs, add a presentation patch or start Turing.

The full Blackwell temporal backend is one transaction over three role-specific
rebuilt fatbins. It is not a single physical fatbin containing all three roles.

Keep separate evidence for source tests, native Windows builds, exact-provider
qualification, runtime application, visual quality and sustained frame pacing.
A successful SetOptions call is an accepted request, not measured displayed FPS.

## Installation and baseline

Build `mfgunlock` and `mfgdiagnostics` from the same source, then replace their
respective addon files with the game closed. Keep a backup outside the loader's
addon search path. Do not mix the new count observer with old diagnostics.

First run with diagnostic observation disabled. On the known RTX 3090 / exact
310.9.1 stack, check ISR + Warp, full Blackwell temporal ready and native 4x.
Keep the game's preset and UI configuration unchanged. Boundary remains Off by
default; a saved Balanced/Aggressive choice remains unchanged.

After checking the production path, enable observation hooks in the diagnostics
panel and restart the game. Do not hot-unload either addon.

## MfgProbe

Start capture, perform a short controlled count transition, then Stop & export.
The existing export directory is `%TEMP%/MFGUnlock-Diagnostics`. The panel
reports the actual written path. A finite event budget/120 s time budget
still applies; `dropped_events` and `capacity_exhausted` must be checked.

The panel reports `core bridge=connected` after capture starts when the matching
core addon is available under its normal filename. `NGX C hooks=0` is possible
and does not imply that the game never uses NGX: some integrations use the C++
parameter interface instead of exported C integer APIs.

The base capture schema remains 1. The `mfg_probe` section has its own version 1
and explicit coverage. New events append `kind=6` and a `frame_count` object:

| Field | Meaning |
| --- | --- |
| `backend` | `streamline`, `ngx_d3d12`, `ngx_vulkan`, or `ngx_c_api` |
| `operation` | original request, actual forward, read, advertised value, or write |
| `origin` | native, fixed override, retry, native fallback, Dynamic, backend fallback, capability policy, or unknown |
| `key` | generated count, maximum generated count, or generated index |
| `value` | signed API value; null after an unsuccessful/unknown read |
| `requested` | original Streamline request where known |
| `ui_fallback` | whether this forward used native options after rejecting UI options |
| `caller` / `callee` | captured module identity and RVA, not guessed from a current reused address |

The event's outer `result` retains its API result. A void NGX Set has no success
result and exports null. Generated counts exclude the source frame: value 3 means
4x output requested, not measured 4x delivery. Native parameter objects use
session-local identifiers; they are not dereferenced during export.

The core observer is optional and has no file I/O. It exposes the calls the core
actually forwards, which an outer slDLSSGSetOptions observer cannot distinguish
on its own. The diagnostics addon independently observes verified exported
`NVSDK_NGX_Parameter_SetUI/SetI/GetUI/GetI` calls. It forwards each call once and
never invents D3D12/Vulkan/override attribution for a C call without that context.
Aliased export addresses with incompatible signatures are not hooked.

This is not exhaustive instrumentation of arbitrary game-owned C++ parameter
vtables, Evaluate calls or GPU presents. No guessed vtable slots are patched.
Core capability reads/writes are covered; other direct-NGX C++ traffic may be
absent. Missing events are not evidence of missing MFG. Hook discovery occurs
at capture start; restart capture after a later module load to retry discovery.

After explicit opt-in, the involved addons stay resident until process exit.
Disabling observation requires a restart. The capture path is bounded but not
zero-overhead; remove/disable diagnostics and restart before performance tests.
Do not arm the pre-existing optional Dynamic capability probe unless that extra
GetState query is specifically needed.

## Runtime matrix

| Game / route | Required checks | Interpretation |
| --- | --- | --- |
| Cyberpunk 2077 / D3D12 modern SL | Native 2x/3x/4x, fixed allowed counts, Dynamic, A/B, UIR, Boundary + Warp, game exit/relaunch | Primary RTX 3090 regression; retain the exact known provider/runtime pair |
| Hogwarts Legacy / modern SL | Active plugin directory, structural ceiling, native selector, HUD/UI recovery | Check the actually loaded Engine/Plugins/Runtime/Nvidia/Streamline/Binaries/ThirdParty/Win64 files, not another copy |
| Clair Obscur: Expedition 33 | Native and forced allowed counts, scene/menu transitions, long run and exit | Independent game integration regression |
| A Plague Tale: Requiem / legacy SL1 | Admission, initialization, supported native FG and exit | Do not install modern SL2 DLLs as a substitute for the legacy ABI test |
| Portal RTX / Vulkan direct NGX | Provider/admission and native count behavior without SL, capability trace if available, exit | Existing Remix presentation/pacing issue remains separate; zero C hooks is not a failed admission result |

Use supported runtime combinations already available locally. Do not manufacture
a ceiling-3 test by modifying a DLL immediate or force 6x into an old pipeline.

For a genuine active plugin with structural generated ceiling 3, request native
2x/4x, then forced 6x: no forwarded generated=5 override is allowed. The original
game request must remain, and the panel must report the structural block. For a
ceiling-5 plugin, forced 6x may be submitted; observe acceptance or real fallback.
Do not provoke a live runtime failure merely to recreate a unit-test sequence.
Retry and rejection paths are covered deterministically by source tests.

The full quality comparison remains separate: same save, camera movement, preset
and native 4x; Dynamic off; Warp on; full restart between Boundary Off, Balanced
and Aggressive. Compare thin geometry and foreground/background boundaries as
well as pacing. A successful screenshot of Aggressive + Dynamic is an application
smoke test, not a controlled quality or performance comparison.

## Lifecycle and release gates

Failed descriptor restoration keeps the replacement allocation alive and returns
failure. A failed previous-role rollback prevents the midpoint fallback. Some
runtime failure paths deliberately keep memory until process exit; the successful
retry tested at helper level is not an automatic repair promise for a live game.
Normal process termination skips active restore/detach work in DllMain and leaves
mapped allocations to Windows. This does not establish a root cause or prove a
fix for every historical shutdown crash. Explicit hot-unload remains unsupported.

Before a release candidate, require:

- a new MSVC build of both addons and both native small-stack tests;
- exact-provider recognition and smoke tests after the identity/restore changes;
- the game matrix above, including real supported ceiling-3/ceiling-5 stacks;
- useful captures with the matching diagnostic build, then diagnostic-free pacing;
- controlled Boundary A/B/C and sustained gameplay/exit tests.

Portable tests and sanitizers cannot discharge these Windows/GPU gates. Leave
Boundary defaults unchanged until the visual comparison supports promotion.

## Generated-frame marking: design only

A ReShade Present callback does not establish which generated image is ultimately
scanned out. Painting `FG 1/3` there could mark a source frame, be interpolated into
other images, or miss the generated output. No marker has been implemented here.

A future developer-only prototype must first identify the exact generated output
resource and slot at a verified provider/Evaluate boundary, including reset/real
frame semantics and multi-viewport ownership. It then needs format-correct drawing
or a copied debug output, resource lifetime and state transitions, queue/fence
ordering, no writes to a real/reset frame, and proof that the marker is not fed
back into history. Capture/replay could validate this independently before any
in-game overlay. That is a separate, invasive feature, not a small read-only probe.

## Recovered external-audit roadmap

| Item | Status after this patch |
| --- | --- |
| Full Stage 2c backend | Existing implementation retained; new failure-path regressions |
| Never raise plugin structural ceiling | Previous fix retained; native options ownership/rejection status corrected |
| Boundary Off/Balanced/Aggressive | Previous implementation retained; real quality comparison still open |
| Hook retry, failed restore, sibling matrix | Implemented deterministic regression coverage; actual binary/game matrix still required |
| 64/128 KiB startup/preflight | Real Windows harness and CI supplied; local native execution still required |
| MfgProbe | Implemented bounded core + exported C count observation with explicit coverage limits |
| Generated-frame marking | Feasibility/design only; deliberately not implemented |
| Full Evaluate capture/replay | Valuable independent quality-research tool, deferred |
| Shadow temporal interpolation | Separate future research; not part of this hardening iteration |
| Broad SM86 inference-kernel optimization | Deferred independent research with licensing/provenance review |
| Turing sm_75 | Provider-wide ptxas qualified and production-dispatched; physical RTX20 runtime still required |
| OptiScaler-native port / installer packaging | Separate future integration/distribution work |

No new camera-matrix reconstruction, smooth-warp crossfade, hybrid FG backend,
streaming compositor helper or wholesale upstream UI redesign is included.
