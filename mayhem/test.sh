#!/usr/bin/env bash
#
# scs/mayhem/test.sh — run scs's own functional suite (minunit) and report a CTRF summary.
# PATCH-grade oracle. Uses the project's NORMAL build flags (not the fuzz sanitizers), independent
# build. Writes a CTRF report (file + stdout marker) and exits non-zero if any test failed.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

# Run the test runner built (normal flags) and stashed by mayhem/build.sh. Do NOT rebuild here.
BIN=/mayhem/run_tests_direct
[ -x "$BIN" ] || { echo "missing $BIN — run mayhem/build.sh first" >&2; exit 2; }
out="$("$BIN" 2>&1)"; rc=$?
echo "$out"

# minunit: "Tests run: N" = non-skipped tests executed; stops at the first failure.
total=$(printf '%s\n' "$out" | sed -n 's/^Tests run: \([0-9][0-9]*\).*/\1/p' | tail -1); total=${total:-0}
skipped=$(printf '%s\n' "$out" | grep -c '^skipped$' || true)
if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -q 'ALL TESTS PASSED'; then
  passed=$total; failed=0
else
  failed=1; passed=$(( total > 0 ? total - 1 : 0 ))   # minunit halts at the first failure
fi

emit_ctrf "minunit" "$passed" "$failed" "$skipped"
