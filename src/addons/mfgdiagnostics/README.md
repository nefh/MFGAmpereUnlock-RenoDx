# MFG Diagnostics

`mfgdiagnostics` is a separate, opt-in, read-only companion for MFG Unlock.
It is not required for normal play and does not replace `mfgunlock`.

It observes the Streamline and NGX boundaries without changing the game's
arguments or results. Captures include:

- `slDLSSGSetOptions` and `slDLSSGGetState`;
- Streamline constants and resource tags;
- frame-count request/forward/read traffic from the core MfgProbe bridge;
- tracked D3D12 `NVSDK_NGX_D3D12_EvaluateFeature` begin/end events for DLSS-G;
- per-Evaluate `DLSSG.MultiFrameCount`, `DLSSG.MultiFrameIndex`, input reset,
  automode reset override, backbuffer frame ID, and known NGX input/output resource descriptors;
- swapchain format, color space and dimensions;
- loaded Streamline/NGX/MFG module candidates and file versions;
- read-only NVAPI sleep/latency/NGX-override state;
- a bounded UI-candidate summary for full-output render targets observed during
  capture.

The NGX Evaluate observer can classify a provider evaluation as a generated
candidate only when count, index and `DLSSG.OutputInterpolated` are all observed
consistently. `FG i/n` in the trace means **provider-generated candidate**, not
proof that Streamline's asynchronous pacer displayed that frame. Source/real
frames are not inferred from FPS, multiplier or host Present order.

Pixel generated-frame marking is intentionally not implemented: the diagnostic
boundary does not prove the resource state or the exact Streamline pacer surface
that is ultimately displayed. Writing into `OutputInterpolated` would therefore
risk changing provider/pacing semantics. The panel and schema-2 trace expose the
exact `FG i/n` classification instead.


## Schema 2 timeline

`trace.jsonl` is the versioned event stream. Every row contains `schema=2`, a
strictly increasing `sequence`, QPC begin/end timestamps, thread ID, and the
event type. Viewport/frame/result fields are `null` when that boundary does not
provide them. Typed payloads then add only observed data:

- `streamline_options`: requested mode/count and UI recomposition;
- `streamline_state`: runtime status, actual-present count, maximum count and
  Dynamic MFG support when exposed by the observed ABI;
- `frame_count`: backend, operation, origin, key, value, request and caller/callee
  module+RVA identity;
- `ngx_evaluate`: evaluation ID/phase, exact provider count/index/reset metadata,
  candidate classification and D3D12 resource descriptors;
- `host_present`: host swap-chain boundary only, never a generated-frame claim.

`summary.json` contains the QPC frequency, event counts, Dynamic-mode
transitions, generated/source/reset/unknown classification counts, bridge
coverage and a read-only snapshot of the core MFG state. `source_evaluations` is
currently expected to remain zero because source frames are deliberately not
inferred from host Present ordering.

## One-shot Evaluate capture

While a capture is recording, **Capture next provider Evaluate** stores one exact
D3D12 NGX Evaluate begin/end metadata pair. Export creates a bundle containing:

```text
trace.jsonl
summary.json
capture_manifest.json   # only after a completed one-shot Evaluate capture
```

The legacy aggregate schema-1 JSON is still written beside the bundle.
`capture_manifest.json` contains provider identity/SHA-256 when available,
Streamline module versions, read-only MFG Unlock state, and the resource
descriptors observed at the Evaluate boundary.

GPU resource contents are not read back in this phase. Resource state and
Streamline pacer synchronization are not verified at the observer boundary, so
all resources explicitly report `readback_available=false`. For the same reason,
standalone Evaluate replay remains gated rather than pretending that metadata is
a complete replay fixture.

Validate an exported bundle with:

```text
python tools/validate_mfg_diagnostics_capture.py <bundle-directory>
```

## Use

1. Put `renodx-mfgdiagnostics.addon64` beside the normal ReShade/RenoDX addons.
2. Open **Add-ons -> MFG Diagnostics**.
3. Enable **Enable observation hooks (restart required)** and restart the game.
4. Choose a label and press **Start capture**.
5. Reproduce the transition you want to observe.
6. Optionally press **Capture next provider Evaluate** and wait for the panel to
   report that the one-shot metadata capture completed.
7. Press **Stop & export**. Capture also stops automatically after 120 seconds or
   when the bounded event buffer fills; after an automatic stop use **Export capture**.
8. The paths are shown in the panel and are written under
   `%TEMP%\MFGUnlock-Diagnostics`.

The panel can also arm a one-shot **Dynamic MFG capability probe**. If the game
already requests a current `DLSSGState`, support is observed passively. For an
older state ABI, the next game-side `slDLSSGGetState` call triggers one extra
query into addon-owned current-version storage on the same thread. The game's
state and result are not changed.

Send the schema-2 bundle, legacy JSON and `ReShade.log` from the same run. Remove
the companion and restart before performance or frame-pacing measurements. Do
not hot-unload it while a game is running because the application can retain
wrapped function pointers.

Capture storage is bounded. Observation callbacks do no file I/O or module
enumeration; export happens only after capture on a worker thread. The expensive
NGX parameter/resource snapshot runs only while the core Evaluate observer is
registered for an active capture. A capture can drop events on lock contention,
and the export reports that explicitly.

## Backend coverage

- **D3D12:** Streamline trace, frame-count trace, exact tracked NGX Evaluate
  metadata/classification, one-shot metadata capture. No GPU readback/pixel
  marker yet.
- **Vulkan:** frame-count/parameter observation already exposed by existing NGX
  paths; no equivalent tracked Vulkan Evaluate/resource capture in this phase.
- **D3D11:** Streamline observations only where the game/plugin exposes them;
  no D3D12 NGX Evaluate capture.

## Build

Copy this folder beside `src/addons/mfgunlock` in the RenoDX tree. RenoDX creates
addon targets from `src/**/**/addon.cpp`, so no CMake changes are required.
Configure once after adding the new folder, then build `mfgdiagnostics`:

```powershell
cmake --preset vs-x64
cmake --build build.vs --config Release --target mfgdiagnostics
```

Output:

```text
build.vs\Release\renodx-mfgdiagnostics.addon64
```

## Runtime settings and lifecycle events

The optional `MfgUnlockSetLifecycleObserver` v1 export appends events to the existing
schema-2 trace, without changing schema-1 events or the Evaluate/state v1 contracts.
Events include native FG mode observations, NGX D3D12 Create/Release/zero handles,
tracking overflow, requested/superseded/frozen quality configuration and automatic
live-option attempts. `live_options_reapplied` records the API result; its name is
not proof of acceptance or of a presented frame.

The `lifecycle` payload carries observed handle/call counters, epoch, configuration
words and `cuda_modules_retired: null`. Missing counters/confidence remain null.
`prepared_quality` records the frozen configuration used to attempt startup
preparation, NOT an applied GPU configuration. Existing state snapshots and actual
module-patch records remain the source for committed Temporal/Warp/Boundary state.
Configuration bits: temporal=0, ISR=1, Boundary=2..3, Warp=4, Warp mode=5..6.

There is no `quality_reconfigure_commit` event in this implementation. A successful
Release/Create is observed but does not prove CUDA module retirement or authorize
live kernel replacement. The validator rejects invented cache-retirement claims.
