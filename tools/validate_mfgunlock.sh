#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# One invocation; optional DLL is strictly input data. Never loads NVIDIA code.
set -u
set -o pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROVIDER="${1:-}"
PARALLEL="${MFG_VALIDATION_JOBS:-4}"
BUILD_ROOT="${MFG_BUILD_ROOT:-$ROOT}"
LOG_ROOT="${MFG_VALIDATION_LOG_DIR:-$ROOT/validation-logs}"
FAIL=0
SKIP_ASAN=0
GCC_READY=0
RESULTS=()
mkdir -p "$BUILD_ROOT" "$LOG_ROOT" || exit 1
TEMP="$(mktemp -d)" || exit 1
trap 'rm -rf "$TEMP"' EXIT
cd "$ROOT" || exit 1
for cmd in cmake ninja g++ clang++ python3 timeout; do
  command -v "$cmd" >/dev/null || { echo "MISSING: $cmd"; exit 1; }
done
[[ "$PARALLEL" =~ ^[1-9][0-9]*$ ]] || { echo 'Invalid MFG_VALIDATION_JOBS'; exit 1; }
if [[ -n "$PROVIDER" && ! -f "$PROVIDER" ]]; then
  echo "PROVIDER_NOT_FOUND=$PROVIDER"; exit 1
fi
record() { RESULTS+=("$1: $2"); [[ "$2" != FAIL ]] || FAIL=1; }
run_logged() {
  local label="$1"; shift
  printf '\n===== %s =====\n' "$label"
  "$@" >"$LOG_ROOT/$label.log" 2>&1
  local rc=$?
  tail -n 12 "$LOG_ROOT/$label.log"
  if (( rc != 0 )); then
    echo "Full log: $LOG_ROOT/$label.log"
    record "$label" FAIL
    return "$rc"
  fi
  record "$label" PASS
}
build_suite() {
  local name="$1" compiler="$2" mode="$3" flags="$4" links="${5:-}"
  local build="$BUILD_ROOT/build.validation.$name"
  cmake -S "$ROOT/contrib/validation" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE="$mode" -DCMAKE_CXX_COMPILER="$compiler" \
    -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$links" &&
  cmake --build "$build" --parallel "$PARALLEL" &&
  ctest --test-dir "$build" --output-on-failure --parallel "$PARALLEL" --timeout 60
}
whitespace_check() {
  # ZIP users have no project index. This is not misreported as a Git comparison.
  local top
  top="$(git rev-parse --show-toplevel 2>/dev/null || true)"
  if [[ "$top" == "$ROOT" ]]; then
    git diff --check
  else
    echo 'Git worktree unavailable: checking source-file trailing whitespace instead.'
    python3 - <<'PY'
from pathlib import Path
bad = []
for root in ('src', 'tools', 'contrib/validation'):
    for p in Path(root).rglob('*'):
        if p.suffix not in ('.hpp', '.h', '.cpp', '.py', '.sh', '.ps1') or not p.is_file():
            continue
        for i, line in enumerate(p.read_text(encoding='utf-8').splitlines(), 1):
            if line.rstrip(' \t') != line:
                bad.append(f'{p}:{i}: trailing whitespace')
print('\n'.join(bad[:40]))
raise SystemExit(bool(bad))
PY
  fi
}
printf '\n===== TOOLCHAIN =====\n'
cmake --version | head -n 1
g++ --version | head -n 1
clang++ --version | head -n 1
python3 --version
run_logged diff-check whitespace_check || true
STRICT='-Wall -Wextra -Wpedantic -Werror'
if run_logged gcc-strict build_suite gcc "$(command -v g++)" Release "$STRICT"; then
  GCC_READY=1
fi
run_logged clang-strict build_suite clang "$(command -v clang++)" Release "$STRICT" || true
printf '\n===== ASAN RUNTIME SMOKE =====\n'
printf 'int main() { return 0; }\n' > "$TEMP/smoke.cpp"
export ASAN_OPTIONS='detect_leaks=1:halt_on_error=1:abort_on_error=1:symbolize=1'
export UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1'
if clang++ -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
     "$TEMP/smoke.cpp" -o "$TEMP/smoke" >"$LOG_ROOT/asan-smoke.log" 2>&1; then
  for attempt in 1 2 3; do
    if ! (ulimit -c 0; ulimit -f 2048; timeout --signal=TERM --kill-after=2s 10s "$TEMP/smoke") >>"$LOG_ROOT/asan-smoke.log" 2>&1; then
      SKIP_ASAN=1; break
    fi
  done
else
  # A missing sanitizer library/compiler error is a failed prerequisite, not a runtime skip.
  SKIP_ASAN=1
  record asan-compile FAIL
fi
if (( SKIP_ASAN == 0 )); then
  record asan-smoke PASS
  run_logged asan-ubsan build_suite asan "$(command -v clang++)" Debug \
    "$STRICT -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" \
    '-fsanitize=address,undefined' || true
else
  record asan 'SKIPPED_ENVIRONMENT (minimal smoke failed; no ASan project PASS)'
  if [[ "${MFG_REQUIRE_ASAN:-0}" == 1 ]]; then record asan-required FAIL; fi
  run_logged ubsan build_suite ubsan "$(command -v g++)" Debug \
    "$STRICT -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" \
    '-fsanitize=undefined' || true
fi
for tool in probe_blackwell_temporal probe_intermediate_scatter probe_validated_warp validate_mfg_diagnostics_capture; do
  run_logged "$tool" timeout --kill-after=2s 120s python3 "tools/$tool.py" --self-test || true
done
if (( GCC_READY )); then
  run_logged registry-and-qualifier python3 tools/qualify_provider.py --self-test \
    --check-registry "$BUILD_ROOT/build.validation.gcc/export_provider_profile" || true
else
  record registry-and-qualifier "NOT_RUN (GCC suite failed; stale executables rejected)"
fi
if [[ -n "$PROVIDER" ]]; then
  run_logged exact-provider-qualification timeout --kill-after=2s 120s \
    python3 tools/qualify_provider.py "$PROVIDER" || true
  run_logged exact-provider-negative-tests timeout --kill-after=2s 120s \
    python3 tools/qualify_provider.py --self-test "$PROVIDER" || true
  if (( GCC_READY )); then
    run_logged exact-provider-preparation timeout --kill-after=2s 120s \
      "$BUILD_ROOT/build.validation.gcc/endpoint_backend_test" "$PROVIDER" || true
  else
    record exact-provider-preparation "NOT_RUN (GCC suite failed; stale executables rejected)"
  fi
else
  record exact-provider 'NOT_RUN (no local DLL supplied)'
fi
printf '\n===== SUMMARY =====\n'
printf '%s\n' "${RESULTS[@]}" | tee "$LOG_ROOT/summary.txt"
printf 'ASAN_SKIPPED_ENVIRONMENT=%s\n' "$SKIP_ASAN" | tee -a "$LOG_ROOT/summary.txt"
if (( FAIL )); then
  echo 'MFGAmpereUnlock validation: FAIL' | tee -a "$LOG_ROOT/summary.txt"
  exit 1
fi
if (( SKIP_ASAN )); then
  echo 'MFGAmpereUnlock validation: PASS_WITH_ENVIRONMENT_SKIP' | tee -a "$LOG_ROOT/summary.txt"
else
  echo 'MFGAmpereUnlock validation: PASS' | tee -a "$LOG_ROOT/summary.txt"
fi
