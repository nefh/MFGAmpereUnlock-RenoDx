# MFG Diagnostics

`mfgdiagnostics` is a separate, opt-in, read-only companion for MFG Unlock.
It is not required for normal play and does not replace `mfgunlock`.

It observes the Streamline boundary without changing the game's arguments or
results. Captures include:

- `slDLSSGSetOptions` and `slDLSSGGetState`;
- Streamline constants and resource tags;
- swapchain format, color space and dimensions;
- the actually loaded Streamline/NGX/MFG module candidates and file versions;
- read-only NVAPI sleep/latency/NGX-override state, including reported FG
  multiplier, game sleep, iFlip and Reflex-related latency state when available;
- a bounded UI-candidate summary for full-output render targets observed during
  capture, with post-HUD-less activity tracked separately. Candidate metadata
  includes render-target binds, transparent
  clears, opportunistic push-descriptor SRV use and timing relative to HUD-less
  and Present.

It does not set NVAPI state, spoof the GPU architecture, alter Streamline
options, patch kernels, retag resources, add latency markers or change pacing.

The panel can also arm a one-shot **Dynamic MFG capability probe**. If the game
already requests a current `DLSSGState`, support is observed passively. For an
older state ABI, the next game-side `slDLSSGGetState` call triggers one extra
query into addon-owned current-version storage on the same thread. The game's
state and result are not changed.

## Use

1. Put `renodx-mfgdiagnostics.addon64` beside the normal ReShade/RenoDX addons.
2. Open **Add-ons -> MFG Diagnostics**.
3. Enable **Enable observation hooks (restart required)** and restart the game.
4. Choose a label and press **Start capture**.
5. Reproduce the transition you want to observe. For UI discovery, switch native
   Frame Generation off/on and leave the game running for a few seconds after
   HUD-less tagging begins.
6. Press **Stop & export** when finished. Capture also stops automatically after
   120 seconds or when the bounded event buffer fills. A stopped capture can be
   exported again with **Export stopped capture**.
7. For Dynamic MFG testing, optionally press **Arm one-shot Dynamic MFG capability probe**
   and let the game make its next DLSS-G state query.
8. The JSON path is shown in the panel. Files are written under
   `%TEMP%\MFGUnlock-Diagnostics`.

Send the JSON together with the `ReShade.log` from the same run. Remove the
companion and restart before performance or frame-pacing measurements. Do not
hot-unload it while a game is running because the application can retain wrapped
Streamline function pointers.

Capture storage is bounded. Observation callbacks do no file I/O or module
enumeration; export happens only after capture on a worker thread. A capture can
drop events on lock contention, and the JSON reports that explicitly.

`ui_candidates` is discovery evidence, not proof that a texture is the game's UI
buffer. The observer records only full-output resources that are used as render
targets during capture. Post-HUD-less activity is tracked separately and
push-descriptor SRV use is recorded when visible, but descriptor-table SRV use is
not exhaustive. Swapchain and resources that match observed HUD-less tags are
reported explicitly so they can be excluded before considering a candidate for
future tagging.

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
