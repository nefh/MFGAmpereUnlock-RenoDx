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
