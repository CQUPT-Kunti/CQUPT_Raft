#!/usr/bin/env bash
set -uo pipefail

mkdir -p tmp/test-logs
mkdir -p specs/006-remove-kv-metadata-state-machine/task-reports

CONFIG_LOG="tmp/test-logs/t051-linux-configure.log"
BUILD_LOG="tmp/test-logs/t051-linux-build.log"
CTEST_LOG="tmp/test-logs/t051-linux-full-ctest-single-worker.log"
FAILED_FILE="tmp/test-logs/t051-linux-failed-tests.md"
REPORT_FILE="specs/006-remove-kv-metadata-state-machine/task-reports/T051-linux-final-validation.md"

run_step() {
  local name="$1"
  local log="$2"
  shift 2

  echo
  echo "==== ${name} ===="
  "$@" 2>&1 | tee "$log"
  return "${PIPESTATUS[0]}"
}

run_step "Configure" "$CONFIG_LOG" cmake --preset debug-ninja-low-parallel
CONFIG_EXIT=$?

run_step "Build" "$BUILD_LOG" cmake --build --preset debug-ninja-low-parallel
BUILD_EXIT=$?

echo
echo "==== CTest Full Single Worker ===="
export CTEST_PARALLEL_LEVEL=1

ctest \
  --test-dir build/linux \
  --output-on-failure \
  --progress \
  -j 1 \
  2>&1 | tee "$CTEST_LOG"

CTEST_EXIT=${PIPESTATUS[0]}

{
  echo "# T051 Linux Failed Tests"
  echo
  echo "## Result"
  echo
  if [ "$CTEST_EXIT" -eq 0 ]; then
    echo "- CTest: PASS"
  else
    echo "- CTest: FAIL"
    echo "- Exit code: $CTEST_EXIT"
  fi

  echo
  echo "## Failed tests"
  echo

  if grep -q "The following tests FAILED:" "$CTEST_LOG"; then
    awk '
      /The following tests FAILED:/ {flag=1; next}
      flag && /^[[:space:]]*[0-9]+ - / {print "- " $0; next}
      flag && !/^[[:space:]]*[0-9]+ - / {flag=0}
    ' "$CTEST_LOG"
  else
    echo "- No failed tests"
  fi

  echo
  echo "## Full CTest log"
  echo
  echo "- $CTEST_LOG"
} > "$FAILED_FILE"

{
  echo "# T051 Linux Final Validation"
  echo
  echo "## Result summary"
  echo
  echo "- Configure: $([ "$CONFIG_EXIT" -eq 0 ] && echo PASS || echo FAIL)"
  echo "- Build: $([ "$BUILD_EXIT" -eq 0 ] && echo PASS || echo FAIL)"
  echo "- CTest: $([ "$CTEST_EXIT" -eq 0 ] && echo PASS || echo FAIL)"
  echo
  echo "## Execution mode"
  echo
  echo "- CTEST_PARALLEL_LEVEL=1"
  echo "- ctest -j 1"
  echo "- Tests run one by one"
  echo
  echo "## Failed tests"
  echo

  if grep -q "The following tests FAILED:" "$CTEST_LOG"; then
    awk '
      /The following tests FAILED:/ {flag=1; next}
      flag && /^[[:space:]]*[0-9]+ - / {print "- " $0; next}
      flag && !/^[[:space:]]*[0-9]+ - / {flag=0}
    ' "$CTEST_LOG"
  else
    echo "- No failed tests"
  fi

  echo
  echo "## Logs"
  echo
  echo "- Configure: $CONFIG_LOG"
  echo "- Build: $BUILD_LOG"
  echo "- CTest: $CTEST_LOG"
  echo "- Failed tests summary: $FAILED_FILE"
} > "$REPORT_FILE"

echo
echo "==== Summary ===="
echo "Configure: $([ "$CONFIG_EXIT" -eq 0 ] && echo PASS || echo FAIL)"
echo "Build: $([ "$BUILD_EXIT" -eq 0 ] && echo PASS || echo FAIL)"
echo "CTest: $([ "$CTEST_EXIT" -eq 0 ] && echo PASS || echo FAIL)"
echo
echo "Full CTest log: $CTEST_LOG"
echo "Failed tests file: $FAILED_FILE"
echo "T051 report: $REPORT_FILE"

if [ "$CONFIG_EXIT" -ne 0 ] || [ "$BUILD_EXIT" -ne 0 ] || [ "$CTEST_EXIT" -ne 0 ]; then
  exit 1
fi

exit 0