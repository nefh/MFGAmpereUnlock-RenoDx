# Architecture

MFGAmpereUnlock keeps MFGAdaUnlock's native NVIDIA execution path. The Ampere
code exists to make a compatible provider and host capability path agree on the
same physical GPU; it does not implement frame generation itself.

## Runtime order

The important ordering is:

```text
real DXGI/NVAPI adapter
        |
        v
provider discovery and structural qualification
        |
        v
sm_89 PTX -> sm_86 mapped-image retarget
hide competing sm_89 cubin
lower provider minimum-architecture gate
        |
        v
MFGAdaUnlock temporal correction
        |
        v
open MFG comparison gates
        |
        v
NGX/Streamline capability evaluation
        |
        v
native sl.dlss_g lifecycle
        |
        v
native NVIDIA CreateFeature / EvaluateFeature
        |
        v
MFGAdaUnlock multiplier and pacing path
```

Provider preparation precedes the capability override. Higher multipliers are
not forwarded until provider retargeting, the temporal correction, and the MFG
comparison gates are all ready.

## Provider preparation

`ampere.hpp`, `ampere_ptx.hpp`, and `fatbin.hpp` implement the provider side.
They are conservative by design:

- only executable PE images with the expected provider ABI are candidates;
- the fatbin parser accepts only known container/header shapes;
- the PTX retargeter accepts only known selectable image layouts;
- the `.target sm_89` edit must map to an independent LZ4 literal;
- a full second decode must match the expected sm_86 PTX byte-for-byte;
- competing sm_89 cubins are hidden from the visible container when present;
- the architecture export and MFG comparisons are verified before writes;
- every write records its original byte and memory protection for rollback.

Provider state is based on **live mappings**, not historical discovery events.
A provider that was inspected and later unloaded does not poison a separately
prepared provider. A currently mapped, incompatible provider still blocks the
capability override because the active execution target would be ambiguous.

## Capability bridge

`ampere_policy.hpp` contains side-effect-free admission rules.
`ampere_ngx.hpp` binds those rules to DXGI, NVAPI, NGX, and D3DKMT.

The bridge deliberately does not expose a fake GPU process-wide:

- the adapter LUID remains real;
- vendor/device identity remains real;
- VRAM and CUDA capability remain real;
- HAGS is read from Windows and never spoofed;
- `NvAPI_GPU_GetArchInfo` is changed from Ampere (`0x170`) to Ada (`0x190`) only
  inside the matching DLSS-G requirement/lifecycle scope;
- unrelated NVAPI calls and other Streamline features see the original adapter.

The NGX requirement relaxation is similarly narrow. It is considered only after
one supported Ampere adapter and exactly one prepared provider have been proven.
Unknown flags, errors from the original call, unknown minimum architectures, or
ambiguous providers are preserved.

## Streamline lifecycle

`ampere_caps.hpp` observes public Streamline support/version/loaded-state calls
and wraps the native DLSS-G plugin lifecycle only to prepare the provider and
record results.

It intentionally does **not**:

- return `loaded=true` for a plugin that is not loaded;
- replace `slDLSSGSetOptions` or `slDLSSGGetState` with stubs;
- remove DLSS-G from `slInit` and substitute another backend;
- patch a private `SystemCaps` object after the native initialization decision;
- overwrite embedded plugin JSON to manufacture a successful lifecycle.

The capability change happens before Streamline derives its supported-adapter
state, so the native plugin builds a consistent state through its own code.

## Resolver and entry coverage

Games and Streamline versions do not all obtain NGX entry points at the same
moment. The addon therefore uses both the existing `GetProcAddress` interception
path and normal Detours entry hooks installed at a safe initialization boundary.
This covers pointers resolved after the hook and pointers cached earlier but
whose function entry is still patchable.

No new hook installation or heavy provider validation is performed from the DLL
load notification while the loader lock is held.

## Temporal correction and MFG

MFGAdaUnlock's midpoint correction remains the source of truth for MFG temporal
placement. The only Ampere-specific change to that logic is allowing the same
known temporal program after its PTX target has already been retargeted to
sm_86.

The existing MFGAdaUnlock frame-count and pacing code remains in control of
`numFramesToGenerate` and the optional software pacing fallback.

## Failure model

The Ampere path is fail-closed. A failed or unknown stage never authorizes the
next stage. Important examples:

- unknown GPU -> no Ampere capability override;
- more than one NVIDIA physical GPU -> no override;
- unknown provider ABI/layout -> no provider patch;
- incomplete transaction -> rollback and block;
- provider changed after qualification -> no override;
- more than one live provider candidate -> no override;
- unknown NGX requirement flags -> preserve the original result;
- new provider discovered after FG feature creation -> refuse late mutation.

This bias is intentional: a missing option is preferable to silently executing
an unverified provider configuration.
