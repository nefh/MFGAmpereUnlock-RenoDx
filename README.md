# MFGAmpereUnlock

Enables native NVIDIA DLSS Frame Generation / Multi Frame Generation on **Ada,
Ampere, and Turing** GPUs through a ReShade/RenoDX addon.

<p align="center">
  <a href="docs/media/ampere-screenshot.jpg">
    <img src="docs/media/ampere-screenshot.jpg"
         alt="MFGAmpereUnlock in Cyberpunk 2077"
         width="900">
  </a>
</p>

The addon makes changes in game memory; it does not modify or redistribute
NVIDIA DLLs. Close the game before updating the addon.

## Usage

1. Install [ReShade](https://reshade.me/) with addon support, or the appropriate
   [RenoDX](https://github.com/clshortfuse/renodx) mod for the game.
2. Download the [latest release](../../releases/latest).
3. Place `renodx-mfgunlock.addon64` in the ReShade addon location used by the
   game, commonly next to the game executable. A game-specific RenoDX package
   may use its own addon folder. Close the game before replacing the addon.
4. **Append the following to `ReShade.ini`:**

   ```ini
   [ADDON]
   LoadFromDllMain=renodx-mfgunlock.addon64
   ```

   **If omitted, MFG Unlock will append itself automatically on supported
   ReShade versions; restart the game once afterwards for MFG to be detected.**
5. Start with the game's original DLSS-G and Streamline libraries. **You do not
   need to replace them with the latest versions for MFG Unlock to work.** Newer
   versions are recommended for advanced features. You can find the
   [latest DLSS-G DLL](https://www.techpowerup.com/download/nvidia-dlss-3-frame-generation-dll/)
   on TechPowerUp.
6. MFG Unlock supports **Streamline 1.x and 2.x** without requiring a
   specific Streamline/DLSS-G version pair. For newer NVIDIA features,
   [Streamline 2.14.1 with DLSS-G 310.9.1](https://www.nexusmods.com/site/mods/1282?tab=files)
   is an optional update where the game supports it, not a requirement.
   **Do not replace Streamline 1.x with 2.x** in older games such as
   **A Plague Tale: Requiem**.
7. Use the game's multiplier selector when available. Otherwise use **Force frame
   multiplier** in the ReShade **MFG Unlock** panel.

### Dynamic MFG and Validated Warp Blend

- **Dynamic MFG** lets NVIDIA adjust the multiplier to reach the selected
  output FPS. It requires a Streamline/DLSS-G version that supports native Dynamic MFG.
  After enabling it, toggle Frame Generation off and on in the game's menu.
- **Validated Warp Blend** ports the quality improvement from
  [MFGAdaUnlock](https://github.com/mavismmg/MFGAdaUnlock-RenoDx) to compatible
  DLSS-G **310.9.1** setups. Enable it and restart the game.
- Warp diagnostic modes are available under advanced/debug options; normal use
  keeps **Validated Warp**.

### Intermediate Scatter Retention and Boundary Artifact Mitigation

- **Intermediate Scatter Retention** helps preserve thin geometry and small
  moving details with compatible DLSS-G **310.9.1** setups.
- **Boundary Artifact Mitigation** controls how ISR behaves near object edges.
  **Off** keeps the `0.10-pre1` ISR behavior; **Balanced** and **Aggressive** add
  progressively stronger boundary handling.
- ISR, Boundary and Warp quality changes require a game restart.

### Frame Generation Presets A/B

- Choose **Application / driver default**, **Preset A**, or **Preset B**.
- A/B overrides are process-local and do not change NVIDIA App or driver profiles.
- Preset overrides are available with compatible **DLSS-G 310.9.1** versions.
- Preset selection is independent from multiplier, Dynamic MFG, Warp, Temporal
  Fix and UI Composition. Restart the game if a preset change is not picked up.

### UI Recomposition

- **Automatic UI Composition** is enabled by default on SDR and keeps the native
  HUD-less path when no usable UI buffer is available.
- **Inject detected UI Color+Alpha** is enabled by default for D3D12 games that
  do not provide a Streamline UI tag. If no usable target is available, the
  native HUD-less path is preserved.
- UI targets are refreshed when dialogs or menus change.
- If UI composition looks wrong, disable UI Color+Alpha injection; Automatic UI
  Composition can stay enabled.

### SDR, HDR and Vulkan

- **SDR/HDR:** improved output color-space detection and UI resource handling.
  HDR output and UI behavior depend on the game and its graphics pipeline.
- **Vulkan:** expanded NGX integration, resource handling and Fixed MFG support.
  Check the Vulkan section below for games that block third-party layers.

## Game compatibility

*The following games have been tested with MFGAmpereUnlock on Ampere.
Results may vary with game, driver, Streamline and DLSS-G versions.*

| Game | Ampere | Comment |
| --- | --- | --- |
| A Plague Tale: Requiem | Working | Legacy Streamline 1.x; do not replace with Streamline 2.x |
| Alan Wake 2 | Working | |
| Assetto Corsa Rally | Working | |
| Clair Obscur: Expedition 33 | Working | |
| Control Resonant | Working | |
| Cyberpunk 2077 | Working | Dynamic MFG, Validated Warp Blend and Preset B (tested with Streamline 2.14.1 / DLSS-G 310.9.1) |
| Grand Theft Auto V Enhanced | Working | |
| Hogwarts Legacy | Working | |
| JDM: Japanese Drift Master | Working | |
| Portal with RTX | Working | RTX Remix |
| Red Dead Redemption 2 | Working | PureDark Frame Generation mod |
| The Witcher 3: Wild Hunt — Remastered | Working | |

*So far, I haven't encountered a game that doesn't work with MFG Unlock.
Try it in other DLSS Frame Generation titles and share
what you find!*

## Requirements

- GeForce RTX 20-, 30-, or 40-series GPU.
- ReShade with addon support.
- A game with NVIDIA DLSS Frame Generation through Streamline or NGX.
- The game's original DLSS-G and Streamline libraries are sufficient to start.
- **DLSS-G 310.9.1** is recommended for Presets A/B, Intermediate Scatter
  Retention, Boundary Artifact Mitigation and Validated Warp Blend.
- Native Dynamic MFG and higher multipliers depend on what the game's
  Streamline/DLSS-G combination supports.

## Vulkan

The addon includes Vulkan NGX adapter and resource support. Some Vulkan games
block third-party layers by default. Where applicable, ReShade may require:

```text
+r_allowBlackListedLayers 1
```

## RenoDX DLSS5

MFG Unlock can coexist with RenoDX DLSS5. Keep Streamline and NVIDIA NGX/DLSS
runtime files from matching releases together; do not update only one DLL in the
set.

| Configuration | Result |
|---|---|
| Streamline 2.12.129 with corresponding 310.7.129 NVIDIA DLLs | Working together and individually |
| Streamline 2.14.0 with 310.9 NVIDIA DLLs | Severe menu slowdown reported in STALKER 2 and Cyberpunk 2077 when both addons were loaded |

If the combined setup performs badly, test each addon separately before changing
MFG settings.

## Settings

Settings use `[RenoDX.MFGUnlock]` in `ReShade.ini`.

Most multiplier, Dynamic MFG and UI controls can update while the game is running.
If a safe update point is unavailable, toggle Frame Generation off and on in the
in-game menu. Preset A/B is shown as applied only after the provider reports it.
Temporal Fix, ISR, Boundary and Warp changes require a game restart.

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | `1` | Enables the addon |
| `Architecture` | `Auto` | `Auto`, `Ada`, `Ampere`, or `Turing` |
| `MaxCount` | `4` | Reported `DLSSG.MultiFrameCountMax` |
| `ForceMultiplier` | `0` | `0` uses the game's choice; `2`–`6` requests that multiplier, subject to game/provider limits |
| `DynamicMFG` | `0` | Enables native Dynamic MFG on Ada/Ampere/Turing when the runtime reports support; takes priority over `ForceMultiplier` |
| `DynamicTargetFPS` | `0` | Dynamic output target; `0` follows display refresh |
| `IntermediateScatterRetention` | `0` | Helps preserve fine moving details with compatible DLSS-G 310.9.1; restart required |
| `BoundaryArtifactMitigationMode` | `0` | `0` Off, `1` Balanced, `2` Aggressive; restart required |
| `ValidatedWarpBlend` | `0` | Warp Blend quality improvement for compatible DLSS-G 310.9.1; restart required |
| `ShowDebugOptions` | `0` | Shows diagnostic and advanced compatibility controls |
| `WarpDiagnosticMode` | `2` | `0` relocation control, `1` baseline rebuild, `2` normal Warp; advanced/debug only changes visibility |
| `FrameGenerationPreset` | `0` | `0` application/driver default, `1` A, `2` B; supported 310.9.1 build only |
| `UIComposition` | `1` | Automatic UI recomposition on SDR |
| `UICandidateInjection` | `1` | D3D12 UI Color+Alpha injection when needed |
| `TemporalFix` | `1` | Corrects generated-frame temporal positions |
| `ForceFlipMeteringOff` | `0` | Legacy software pacing fallback |
| `RuntimeSelectionMode` | `0` | `0` keeps the game policy, `1` prefers local Streamline plugins, `2` forces NVIDIA OTA flags |

When updating from `0.10-pre1`, an existing `IntermediateScatterRetention=1`
configuration without a boundary setting keeps the previous ISR behavior until
**Balanced** or **Aggressive** is selected.

## Troubleshooting

### The addon does not appear in ReShade

- Confirm ReShade was installed with full addon support.
- Confirm `renodx-mfgunlock.addon64` is in the addon location used by the game.
- Check the ReShade log for addon loading errors.

### Only Automatic or 2x appears

- Toggle Frame Generation off and on after reaching the graphics menu.
- Confirm the expected `nvngx_dlssg.dll` version is loaded.
- Check the MFG Unlock panel for DLSS-G and Frame Generation status.

### Dynamic MFG is unavailable or Warp Blend is not applied

- Check whether the loaded Streamline/DLSS-G combination supports native
  Dynamic MFG. DLSS-G 310.9.1 is recommended for Warp Blend.
- After enabling Dynamic MFG, toggle Frame Generation off/on in the game.
- Warp Blend requires a restart. Keep the original Streamline files in legacy
  Streamline 1.x games.

### UI Recomposition problems

- Dialog/menu changes should be reacquired automatically after a short handover.
- If UIR does not return or UI artifacts appear, disable **Inject detected UI
  Color+Alpha**. Automatic UI Composition can remain enabled.

### 3x/4x freezes or pacing becomes unusable

- Leave `ForceFlipMeteringOff=0` with current Streamline builds.
- If higher multipliers specifically freeze, set `ForceFlipMeteringOff=1` and restart.

### Performance collapses with RenoDX DLSS5

- Test each addon separately.
- Use a matched Streamline/NVIDIA runtime set.
- Restart after addon or runtime changes.

## How it works

MFGAmpereUnlock keeps NVIDIA's native DLSS Frame Generation and adapts it to
the selected GPU architecture rather than replacing it with another frame
generator:

1. `Auto` detects Ada, Ampere or Turing; the architecture can also be selected
   manually if needed.
2. On **Ada**, NVIDIA's DLSS-G provider already contains compatible `sm_89`
   code. The addon adjusts Frame Generation availability and MFG settings.
3. On **Ampere** and **Turing**, compatible provider PTX is adapted from
   `sm_89` to `sm_86` or `sm_75` so the NVIDIA driver can JIT-compile it.
4. The addon examines the loaded provider and its embedded CUDA code before
   applying changes, rather than requiring a specific hardcoded DLL version.
5. Streamline/NGX capability checks are adapted for the selected GPU so the
   game's native Frame Generation path can become available.
6. At higher multipliers, **Temporal Fix** preserves the intended temporal
   position of each generated frame.
7. With compatible Streamline/DLSS-G versions, **Dynamic MFG** uses NVIDIA's
   native mode and frame pacing.
8. **Validated Warp Blend** improves later-stage reprojection for compatible
   DLSS-G 310.9.1 setups.
9. **UI Recomposition** uses the game's native HUD-less/UI resources where
   available, with optional D3D12 Color+Alpha injection and fallback.

## Building from source

MFGAmpereUnlock is built through RenoDX. You need **Visual Studio 2022** with
the **Desktop development with C++** workload, **CMake**, and **Git**.

From the MFGAmpereUnlock repository root, run:

```powershell
.\tools\build_mfgunlock.ps1
```

On the first run, the helper automatically downloads the pinned RenoDX and NVAPI
source dependencies into `external\RenoDX`, initializes RenoDX submodules, and
sets up the repo-local shader compiler tools required by the build. System-wide
prerequisites such as Visual Studio and the Windows SDK are not installed automatically.

The compiled addon is written under:

```text
dist\mfgunlock\reshade-api18\renodx-mfgunlock.addon64
```

To build for any ReShade API version available in upstream ReShade history, pass the API number directly. Older API versions can be useful for compatibility with legacy ReShade releases required by some closed-source mods, such as PureDark mods:

```powershell
.\tools\build_mfgunlock.ps1 -ReShadeApi 15
.\tools\build_mfgunlock.ps1 -ReShadeApi 14,15,17,18,20
```

The helper resolves the matching ReShade source revision automatically, detects the callback ABI from those headers and keeps every per-API ReShade checkout inside the repository at `external\reshade-api\api<version>`. API-specific builds are written to `dist\mfgunlock\reshade-api<version>\`. At the end of a build the helper prints the complete absolute path and SHA256 for every generated addon. If the requested API never existed upstream, the build stops with a clear error.

To re-check and refresh the managed build dependencies, use `-RefreshDependencies`.

## Credits

- https://github.com/dashdogy/RTX40MFG-Unlock
  provided the foundational reverse engineering and original working ASI
  implementation. Dashdogy diagnosed the higher-multiplier temporal compaction
  bug, demonstrated the corrected slot-9 temporal program, established the verified
  Streamline/NGX interception strategy, and showed how to apply the fix only to
  mapped process memory without modifying NVIDIA DLLs on disk.
- Dashdogy's project is published under the
  [MIT License](https://github.com/dashdogy/RTX40MFG-Unlock/blob/main/LICENSE).
  The temporal correction implementation remains independently written for the
  ReShade-addon format and was verified by reproducing the original patcher's
  output digest byte-for-byte.
- [Dreamt](https://github.com/ImDreamt) created the original ReShade/RenoDX addon
  adaptation and repository on which MFGAdaUnlock is based.
- [mavismmg](https://github.com/mavismmg) developed and maintained the
  [MFGAdaUnlock-RenoDx](https://github.com/mavismmg/MFGAdaUnlock-RenoDx) fork,
  extending the Ada implementation with broader game and DLSS-G provider
  compatibility, lifecycle and pacing fixes, Vulkan support, runtime diagnostics,
  Dynamic MFG, UI recomposition and the Validated Warp Blend path. Selected paths
  were later adapted here for Ampere.
- Tony Joaca, author of DLSSG-Transfusion, publicly identified
  `Kernel_BlendCandidatesFused` as the useful intervention point behind the
  `qualityValidWarp` option. That public research informed MFGAdaUnlock's
  Validated Warp Blend path, which this repository ports to Ampere and Turing.
- Special thanks to [Coldwood1026](https://github.com/Coldwood1026) for sharing
  `ptx_out.zip` with the DLSS-G PTX and demonstrating successful cubin
  compilation for SM 8.6 and SM 7.5, providing the starting point for the
  Ampere/Turing backport work.
- Special thanks to [mugensc](https://next.nexusmods.com/profile/mugensc) for the
  RenoDX DLSS5 compatibility testing and known-good runtime combination.
- Built on [RenoDX](https://github.com/clshortfuse/renodx) by clshortfuse, and
  [ReShade](https://github.com/crosire/reshade) by crosire.

## Disclaimer

Not affiliated with or endorsed by NVIDIA. This modifies process memory of a
running game. Use it at your own risk; anti-cheat systems may object to injected
addons or modified process memory.

## License

MIT — see [LICENSE](LICENSE).
