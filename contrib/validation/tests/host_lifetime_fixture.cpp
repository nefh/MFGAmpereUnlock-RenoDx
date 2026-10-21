// SPDX-License-Identifier: MIT
// Test-only host, no ReShade/NVIDIA code. Loaded by a separate CTest process.
extern "C" __declspec(dllexport) int HostLifetimeFixture() { return 317; }
