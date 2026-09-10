# Compatibility

Profiles select the provider target independently of the game integration.
Auto is the default. Explicit profiles are not checked against the detected
GPU; `Auto` performs that selection from the original NVAPI result. The addon
self-configures ReShade early loading on its first run; that initial bootstrap
needs one restart before the Ampere/Turing capability path can be relied on.

| Configuration | Status | Notes |
| --- | --- | --- |
| RTX 3090 / GA102 / D3D12 | Validated | Native DLSS-G/MFG runtime exercised successfully. |
| Other RTX 30-series / sm_86 | Ampere profile | Additional runtime reports are welcome. |
| RTX 20-series / sm_75 | Turing profile implemented | No Turing GPU runtime result in this source update. |
| RTX 40-series / sm_89 | Ada profile | Uses the upstream provider path without backporting. |
| GA100 / sm_80 | No automatic profile | The Ampere profile targets sm_86, not sm_80. |
| Multiple NVIDIA physical GPUs | Bound by NGX adapter LUID | Early detection defers until the game supplies its adapter; one active binding per process. |
| Vulkan through the backport profiles | Not connected | The validated path is D3D12. |
| DLSS-G provider 310.7.0.0 release | Validated | 70 known fatbins were retargeted in the validated configuration. |
| DLSS-G provider 310.1.0.0 | Not suitable for current MFG temporal path | PTX is retargetable, but its temporal kernel ABI does not match the current MFGAdaUnlock profile. |
| Unknown provider layouts | Rejected | No heuristic or hash-only patching. |

## Streamline

The capability/lifecycle code accepts known Streamline 2.x families used during
validation, while the actual API results remain authoritative. Version admission
is a guard against interpreting a private lifecycle ABI as an unrelated build;
it is not a promise that every version in the range has been tested in every game.

## Provider updates

The provider is qualified from the mapped image at runtime. Hashes are not the
sole gate, but new layouts still need structural review. If NVIDIA changes the
fatbin layout, provider ABI, or temporal program, the safe expected behavior is
for the backport to refuse preparation and leave native support unchanged.

Do not mix arbitrary `sl.*.dll` versions as a compatibility strategy. Update a
provider independently only when its Streamline/NGX integration is known to be
compatible with the game.
