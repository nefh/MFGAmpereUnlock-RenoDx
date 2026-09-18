# Runtime settings, NGX lifecycle and kernel-reload safety

## Implemented scope

UI controls stay editable. A change updates the requested INI value immediately;
that is not a claim that the GPU or driver has accepted it. The existing bounded
viewport inventory caches a private copy of the game's last native DLSSGOptions.
It never caches our overridden copy.

At a later existing slSetConstants callback, or the primary ReShade Present
callback when exactly one viewport is known, pending live options may be submitted
through the existing fixed/Dynamic/UI policy. Requirements: a recognized options
ABI, no borrowed extension chain or error callback, successful native request,
unchanged endpoint/cache epoch, and the same thread observed for both native
SetOptions and Present. No work is issued from the UI callback. Multiple edits
coalesce into the latest request. No new submission occurs without a new revision.

Automatic submission preserves eOff, uses no GPU copies/fences, installs no pacing
patches, and does not call slSetFeatureLoaded. A failed attempt is not retried every
frame. Unknown ownership, another thread, multiple ambiguous viewports, output
invalidation, reentrancy or callback-bearing options defer to the next real native
SetOptions call. In those cases the UI recommends the game's FG off/on action.

## Settings matrix

| Setting | Implementation | Confirmation |
| --- | --- | --- |
| Fixed multiplier, Dynamic mode/target | Automatic setter under the guards above | Actual API result and existing fallback telemetry |
| Automatic UI Composition | Same automatic setter; existing resource guards retained | Accepted options, not proof of composed pixels |
| UI candidate injection | Existing tag path; options resync requested | Existing tag/eligibility observations |
| Reported maximum | Next existing capability query | Returned capability; not an emitted-frame count |
| Preset A/B | Next exact model-setting read | Provider applied-preset observer, never Create alone |
| Temporal Fix, ISR, Boundary, Warp/mode | Requested value editable; frozen after initial preparation | Existing committed patch records; later edits need restart |
| Architecture/runtime selection/early load/legacy pacing | Startup-only | Restart |
| Global Enabled | Stops host overrides through live native-options resubmission | Does not undo loaded provider kernels |

Inactive child choices can be edited. Boundary does not make the configuration
pending while ISR is off; Warp mode is irrelevant while Warp is off. Advanced
visibility and supported-architecture prerequisites remain unchanged.

## Why NGX Release/Create does not authorize kernel replacement

The earlier plan assumed a stronger contract than the SDK provides. ReleaseFeature
invalidates a feature handle; it does not document retirement of all CUDA modules
and CUfunction handles created from a provider's fatbins. CPU Evaluate completion
is not a GPU completion fence. The current addon retains provider modules for the
process lifetime, and provider preparation rewrites source fatbins. Restoring a
quality descriptor does not reconstruct pristine sources or prove driver cache
invalidation.

Consequently this implementation DOES NOT restore/rewrite provider kernels at an
NGX handle-count boundary. Zero handles, Release and Create remain observable facts,
not a fabricated cache-unload guarantee. Initial configuration is frozen coherently
before the first preparation. Later maintenance cannot accidentally consume an
unrelated pending quality edit. A pre-Create barrier prevents racing our ongoing
preparation; contention fails that Create request before forwarding it, rather than
waiting indefinitely or publishing partial patches. The maintenance lock is released
before any NVIDIA Create call.

The UI reports one kernel-quality pending status and a separate live-options action
only when necessary. It does not tell a player that an unverified FG cycle applied
Boundary/Warp. A full kernel reload would require independent evidence of module
retirement, synchronization and a pristine source/rebuild strategy. None is assumed.

## Lifecycle and failure behavior

NGX D3D12 uses its existing four runtime slots and 16-handle inventories. Create,
Evaluate and Release in-flight counters are exception-safe. Successful last release
records an observed zero-handle boundary; failed release retains the handle. Capacity
exhaustion, duplicate handles or exceptional exits mark tracking uncertain. Actual
feature_created/feature_active flags are cleared on release instead of remaining
historical 'ever seen' flags. Epoch changes on a known zero-to-nonzero transition.
No NVIDIA API is executed under the inventory lock. Observer callbacks run after
releasing it. There is no spin-wait, GPU flush, CUDA unload or plugin unload.

Failed quality commit owners are quarantined separately from applied-feature records.
Restore removes only fully restored records; incomplete records retain allocation
ownership and descriptor rollback metadata. This extends the existing low-level
VirtualProtect/VirtualFree invariant through its callers. Diagnostic state is not
reported as valid when the provider registry is failed.

Preset observer retry recognizes its own installed reader prologue only for the
retained module and exact hook target; the observer and selection code must still
match pristine fingerprints. This fixes a reader-success/observer-failure state in
which later retries previously rejected our own Detours-modified reader bytes.

## Diagnostics

The optional Lifecycle v1 POD observer extends the existing schema-2 stream. It does
not alter Evaluate/State v1 ABI or schema-1 events. NGX lifecycle, accepted mode calls,
requested/superseded/prepared quality settings and automatic setter attempts share
the capture's sequence. Absent counters remain null. cuda_modules_retired is always
null. prepared_quality is not applied_quality. No kernel-reconfigure-commit is emitted.

## Backend boundaries

D3D12 with recognized Streamline exposes automatic options and NGX lifecycle tracking.
The setter itself is architecture-neutral and retains the existing Turing/Ampere
capability gates. For Streamline Vulkan, only already-supported options/ABI/thread
paths may be resubmitted; Dynamic retains its D3D12 restriction. Direct NGX Vulkan
has no new lifecycle reload or automatic Streamline setter. No D3D11 path is added.
No game/backend runtime has been executed for this patch in the Linux container.

## Runtime procedure

1. Build both addons and run native small-stack tests. Install with the game closed.
   First use diagnostics observation OFF, current working provider/configuration and
   a supported native FG multiplier. Verify no regression in startup/exit.
2. With the ReShade UI visible, change fixed multiplier without touching the game's
   FG switch. Observe accepted/rejected/native fallback. If the cached native options
   or thread is unsuitable, expect the explicit off/on message, not an unsafe setter.
3. Test Dynamic and its target without changing kernel quality. Exact runtime support
   remains required. Test changes while the game itself has FG off: it must stay off.
4. Select several Boundary/Warp changes, then return to the prepared choice. Controls
   stay editable; pending clears only for an equivalent prepared configuration. A
   different kernel configuration remains pending even after NGX feature recreation.
5. Select Preset A/B and distinguish requested/supplied/provider-observed values. FG
   off/on is a possible model-setup trigger, not proof of acceptance.
6. Enable diagnostic observations, restart as required for those hooks, capture the
   same sequence, and export. Inspect live_options_reapplied.result, mode calls,
   handle counts, epoch, and quality pending events. No cuda_modules_retired=true or
   quality_reconfigure_commit should appear. Validate the bundle with
   tools/validate_mfg_diagnostics_capture.py.
7. Repeat a short Ampere game regression. Physical RTX20 remains a separate pending
   qualification; no compiled test or handle trace replaces that run.

## Primary documentation consulted (2026-09-18)

- https://raw.githubusercontent.com/NVIDIA-RTX/Streamline/main/docs/ProgrammingGuideDLSS_G.md
  Section 6: next-Present options and thread ordering; eOff/retain-resources distinction.
- https://raw.githubusercontent.com/NVIDIA/DLSS/main/include/nvsdk_ngx.h
  ReleaseFeature invalidation and separate SDK shutdown.
- https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__MODULE.html
  Image loading into a context, CUmodule/CUfunction lifetime and explicit module unload.
- https://docs.nvidia.com/cuda/archive/11.7.0/parallel-thread-execution/index.html
  Sections 9.7.13.4.7/.8: FP16 MMA fragment coordinates used in the independent test.

These sources do not prove a particular game's CUDA cache reload behavior.
