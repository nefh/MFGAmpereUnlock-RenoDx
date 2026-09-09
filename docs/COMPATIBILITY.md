# Compatibility

The Ampere path is intentionally narrower than the rest of MFGAdaUnlock until
more hardware and games have been tested.

| Configuration | Status | Notes |
| --- | --- | --- |
| RTX 3090 / GA102 / D3D12 | Validated | Native DLSS-G/MFG runtime exercised successfully. |
| Other GA102/GA104-class Ampere | Admitted by current policy, not yet broadly validated | Additional reports are needed. |
| GA100 / sm_80 | Rejected | Not part of the current sm_86 provider path. |
| Multiple NVIDIA physical GPUs | Rejected | Adapter/provider ownership would be ambiguous. |
| Vulkan through the Ampere contribution | Not connected | The validated path is D3D12. |
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
for the Ampere path to refuse preparation and leave native support unchanged.

Do not mix arbitrary `sl.*.dll` versions as a compatibility strategy. Update a
provider independently only when its Streamline/NGX integration is known to be
compatible with the game.
