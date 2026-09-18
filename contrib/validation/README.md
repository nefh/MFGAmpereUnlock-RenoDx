# Validation helpers

This directory is not part of the runtime addon. It contains portable regression
tests for the architecture profiles, early-load list handling, provider transforms
and native wrappers, plus a read-only provider inspector.

On Linux/macOS with CMake and a C++20 compiler:

```sh
cmake -S contrib/validation -B build.validation -G Ninja
cmake --build build.validation
ctest --test-dir build.validation --output-on-failure
```

For a warnings-as-errors run with GCC or Clang, pass the desired compiler flags
through `CMAKE_CXX_FLAGS`.

`inspect_provider` reads a provider or standalone fatbin without loading or
executing it. A positive result is structural qualification only; it is not a
substitute for native Create/Evaluate and presentation validation on a GPU.

The inspector accepts an optional target profile (default `Ampere`):

```sh
build.validation/inspect_provider /path/to/nvngx_dlssg.dll Turing
```

The temporal cases run against both the inspector helper and the runtime
`midpoint.hpp`. The frame-count test exercises the real `framecount.hpp` using
SDK doubles. Neither test installs machine-code hooks.

The Warp test also covers release/debug mode normalization. `fg_preset_test`
checks Default/A/B parsing, scoped model-setup overrides and native passthrough
using hook doubles. On non-Windows builds it can additionally inspect a local
copy of the supported provider and exercise the installation guards:

```sh
build.validation/fg_preset_test /path/to/nvngx_dlssg.dll
```

That optional check verifies the actual machine-code fingerprints and mocked
lifecycle only. It neither loads NVIDIA code nor validates Detours, the Windows
ABI, model B execution, or frame-generation quality. No NVIDIA DLL is included.

### `blackwell_temporal_test`

Checks the exact DLSS-G 310.9.1 Blackwell temporal PTX identities used by the
Ampere full-temporal path and the fail-closed Intermediate Scatter rewrite.


### Hardening and count observation

The non-Windows suite contains 18 tests. `lifecycle_test` exercises the production
Warp, midpoint and full-temporal redirect/restore code with deterministic memory
protection failures. Failed restores must retain replacement allocations; an
unsafe multi-role rollback must prevent a competing fallback. `hook_lifecycle_test`
uses the actual hook implementation with Windows/Detours doubles to cover missing
exports, failed thread enumeration, attach/commit failures, retries and address-hook
trampoline publication. `mfg_probe_test` checks count-key filtering, signed values,
failed reads, aliased exports, capture gating and exactly-once native calls.

`provider_state_test` contains a synthetic same-version sibling matrix. Names
such as DLSS, DLSSD, DLSSNR and DeepDVC label negative fixtures; these are not
bundled NVIDIA binaries. A filename, version or export alone is not proof of
DLSS-G identity. Renamed genuine-identity fixtures remain accepted.

The frame-count tests also verify that native option memory is not modified,
rejected fallback is not reported as restored, native Dynamic requests remain
Dynamic, and diagnostic request/forward events preserve retry/fallback origin.

The probe self-tests require only Python:

```sh
python3 tools/probe_blackwell_temporal.py --self-test
python3 tools/probe_intermediate_scatter.py --self-test
python3 tools/probe_validated_warp.py --self-test
```

The Blackwell temporal and Validated Warp probes can qualify either backport
target without changing production runtime gates:

```sh
python3 tools/probe_blackwell_temporal.py /path/to/nvngx_dlssg.dll \
  --ptxas /path/to/ptxas --target-sm 75 --json temporal-sm75.json
python3 tools/probe_validated_warp.py /path/to/nvngx_dlssg.dll \
  --ptxas /path/to/ptxas --target-sm 75 --json warp-sm75.json
```

A successful cubin build is offline qualification only. Turing runtime support
for the full Blackwell temporal, ISR/Boundary and Warp paths remains disabled
until those paths are separately validated on physical RTX 20 hardware.

### Native Windows small-stack tests

This is a separate opt-in harness, not the portable Windows API doubles. It uses
the Windows SDK and real Microsoft Detours v4.0.1. Configure from the source root
in PowerShell with Visual Studio 2022 installed:

```powershell
cmake -S contrib/validation -B build.validation.windows `
  -G "Visual Studio 17 2022" -A x64 -DMFG_NATIVE_STACK_TESTS=ON
cmake --build build.validation.windows --config Release --target small_stack_test
ctest --test-dir build.validation.windows -C Release -R "^small_stack_" --output-on-failure
```

The first configure downloads the pinned Detours tag. For an offline machine,
pass `-DMFG_DETOURS_SOURCE_DIR=C:/path/to/Detours` pointing at a reviewed checkout
with `src/detours.h`. No NVIDIA DLL, GPU or full RenoDX build is required.

Each test uses `_beginthreadex` with `STACK_SIZE_PARAM_IS_A_RESERVATION` and
verifies the actual reservation before running production module inspection,
loader notification, missing-export preflight and real entry/address detours.
The two reservations are 64 KiB and 128 KiB. Merely changing stack commit while
retaining the executable's default reserve would not test this property.

The harness does not simulate a game's complete GPU initialization, a ReShade
host or all loader-lock interleavings. Passing it is additional evidence, not a
guarantee that every startup/teardown path fits a small stack. The workflow
`windows-small-stack` builds only this native target and selects only these tests;
it does not try to run the portable API-double suite under Windows.

See [the regression checklist](ampere_regression.md) for in-game validation and
MfgProbe coverage. Restart games between addon binaries; hot-unload is not a
supported diagnostic workflow.


### SM75 packed-half qualification

`ptx_retarget_test` now covers strict packed FP16 lowering, reversed high/low
packing, exact counts, rejected modifiers/operands, transactional failure and
bounded single-image provider rebuilding. The existing temporal and lifecycle
tests cover explicit target metadata without enabling Turing quality dispatch.

Run `probe_blackwell_temporal.py` with `--audit-provider` in addition to
`--target-sm 75`. It inspects all readable non-executable sections, including the
SM89-only inference programs outside `.data`, and compiles only the five exact
packed-half programs changed by this patch. `full_temporal_probe` and
`provider_lowering_probe` are separate results: the former is NOT a claim that
the complete provider is ready.

`--no-assemble` writes a static report without a CUDA installation. Compilation
is recorded as `NOT_RUN`, the command exits nonzero, and the report cannot be
used as a runtime activation gate. It never substitutes a simulated compiler.

See [SM75 qualification](sm75_qualification.md) for the remaining provider gate.

## Advanced MFG diagnostics validation

`diagnostics_timeline_test` validates fail-closed generated-candidate
classification and one-shot NGX Evaluate metadata capture without a GPU. The
core `host_wrappers_test` also verifies that the existing D3D12 NGX Evaluate
wrapper publishes begin/end events, exact count/index metadata and the
`OutputInterpolated` descriptor only while an observer is attached.

Exported schema-2 diagnostic bundles can be checked without loading a provider:

```text
python tools/validate_mfg_diagnostics_capture.py --self-test
python tools/validate_mfg_diagnostics_capture.py <bundle-directory>
```

The validator deliberately rejects claims of replay support for the current
metadata-only capture format.

`diagnostics_trace_test` compiles the production bounded trace container and verifies monotonic sequence assignment, explicit sequence preservation, capacity stop, restart, and deadline handling.

## Runtime settings regression

`runtime_settings_test` exercises production Streamline/NGX wrapper headers against
portable API doubles: automatic options submission, thread/ABI/lifetime guards,
coalescing, rejection, multi-viewport, real concurrent try-lock deferral, NGX handle
inventory, exceptions and requested/frozen quality state. It does not execute CUDA
or prove a game's FG recreation behavior. The existing trace test covers lifecycle
schema export inputs; Python validation covers null/unknown semantics.

An optional additional fixture run, with the unmodified installed provider, is:
`fg_preset_test <nvngx_dlssg.dll>` (the file is parsed as data, hooks are mocked).
It tests applied-preset observer retry after our reader has modified its prologue.
`ptx_retarget_test` independently checks FP16 MMA fragment coordinates for all 32
lanes, in addition to the existing transformation/negative/mutation tests.

See `runtime_settings.md` for the runtime procedure and why kernel cycling remains
gated. Native small-stack and full MSVC builds must still be executed on Windows.
