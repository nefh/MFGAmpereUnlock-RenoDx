# Validation

## Validated runtime configuration

The initial runtime validation used:

- GeForce RTX 3090 (GA102, sm_86)
- NVIDIA driver 610.47
- Windows 11
- Direct3D 12
- Cyberpunk 2077 as the first integration host
- NVIDIA DLSS-G provider 310.7.0.0 release
- ReShade 6.8 with addon support

The following milestones were observed in one continuous native run:

1. The real adapter was identified as NVAPI architecture `0x170`.
2. The DLSS-G provider was prepared in mapped memory: 70 fatbins retargeted,
   31 competing cubins hidden, and the provider architecture gate lowered.
3. The temporal correction redirected eight `dlfg_kernel` descriptors to the
   corrected program.
4. Provider/MFG comparison gates were opened only after provider and temporal
   preparation completed.
5. The NGX minimum-architecture requirement was relaxed for the qualified
   Ampere adapter with one prepared provider and no live conflicting provider.
6. The native DLSS-G plugin completed `slOnPluginLoad` and `slOnPluginStartup`.
7. `slIsFeatureSupported(DLSS-G)` returned success from the native Streamline path.
8. Native `slDLSSGSetOptions` and `slDLSSGGetState` were reached.
9. Native FG feature creation returned success with a non-null feature handle.
10. Native FG evaluation continued successfully for more than 42,000 calls.
11. Streamline state telemetry reported non-zero actual presentation activity.
12. Native 2x, 3x, and 4x options were selectable and the higher-multiplier modes
    were manually observed to run smoothly.

## What this proves

This establishes substantially more than menu availability: the validated
Ampere configuration reaches and repeatedly executes NVIDIA's native DLSS-G
Create/Evaluate path with the prepared provider.

## What this does not prove

The current instrumentation does not independently capture and compare every
presented image, trace every CUDA kernel launch, or establish a universal frame-
pacing bound. Manual visual validation is therefore still part of the current
compatibility evidence.

The result also does not imply compatibility with every RTX 30-series model,
game, Streamline build, DLSS-G provider, multi-GPU configuration, or Vulkan.
Those should be added to the compatibility matrix only after equivalent runtime
validation.

## Regression tests

`contrib/validation` contains tests for:

- profile parsing, default Auto, explicit selection and one-time hardware detection;
- ReShade early-load list matching and non-destructive append behavior;
- conservative fatbin parsing and sm_89 -> sm_86 / sm_75 PTX retargeting;
- temporal-patch composition after retargeting, also using the real runtime header;
- native frame-count behavior for Ada, disabled and unresolved profiles;
- capability policy matrices and HAGS decoding;
- Streamline/NGX wrapper behavior against deterministic API doubles;
- live provider-state handling, including stale rejection records, active
  conflicting providers, rollback, remapping, and late-mutation refusal.

These tests verify host-side invariants. They do not replace a Windows/MSVC build
or a real GPU runtime test.
