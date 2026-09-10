# Architecture

MFGAmpereUnlock keeps MFGAdaUnlock's native NVIDIA execution path.
`architecture.hpp` selects one profile for the existing provider, capability,
temporal and frame-count code.

| Profile | Provider minimum / MFG compares | PTX target | Scoped NVAPI architecture |
| --- | --- | --- | --- |
| Ada | `0x190` | `sm_89`, no backport | unchanged |
| Ampere | `0x170` | `sm_86` | `0x190` |
| Turing | `0x160` | `sm_75` | `0x190` |

Missing `Architecture` selects Auto. Explicit profiles do not query the GPU
architecture. `Auto` uses the original NVAPI result and caches the resolved
profile. Unknown architectures remain unmodified; GA100 (`sm_80`) has no profile.
The profile is fixed for the lifetime of the addon; changes require a restart.

The backport capability hooks also need to be present during ReShade startup. If
`[ADDON] LoadFromDllMain` does not already contain this addon, the first normal
load appends it without replacing existing entries and asks for one game restart.

## Runtime order

The important ordering is:

```text
Architecture setting (default Auto) / one-time hardware detection
        |
        v
provider discovery and structural qualification
        |
        v
sm_89 PTX -> selected SM (Ada skips retargeting)
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
- each changed digit in `.target sm_89` must map to an independent LZ4 literal;
- a full second decode must match the expected target PTX byte-for-byte;
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
- backport profiles expose `0x190` through `NvAPI_GPU_GetArchInfo` only inside
  the matching DLSS-G requirement/lifecycle scope;
- unrelated NVAPI calls and other Streamline features see the original adapter.

The NGX requirement relaxation is similarly narrow. It is considered only after
the game adapter has been associated by LUID and exactly one provider is prepared.
This association scopes the hook; it does not confirm the manually selected
architecture. Early discovery is used only with a single NVIDIA GPU. With more
than one, the bridge waits for the adapter passed to NGX.
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
placement. The same known temporal program is accepted after retargeting to either
`sm_86` or `sm_75`. Both the PTX directive and the fatbin entry retain that target
when the temporal program is rebuilt.

The existing MFGAdaUnlock frame-count and pacing code remains in control of
`numFramesToGenerate` and the optional software pacing fallback.

## Failure model

Profile selection does not bypass provider validation:

- unknown architecture in Auto -> no backport or multiplier override;
- unresolved adapter LUID -> no scoped capability override;
- unknown provider ABI/layout -> no provider patch;
- incomplete transaction -> rollback and block;
- provider changed after qualification -> no override;
- more than one live provider candidate -> no override;
- unknown NGX requirement flags -> preserve the original result;
- new provider discovered after FG feature creation -> refuse late mutation.

The original `Enabled` switch controls all profiles. There is no separate
backport enable flag. `Enabled=0` and unresolved profiles leave native game
requests and state values unchanged.
