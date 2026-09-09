# Validation helpers

This directory is not part of the runtime addon. It contains portable regression
tests for the Ampere-specific code and a read-only provider inspector.

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
