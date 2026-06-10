#!/usr/bin/env bash
set -uo pipefail

mkdir -p tmp/test-logs
mkdir -p specs/006-remove-kv-metadata-state-machine/task-reports

PRESET="debug-ninja-low-parallel"
BUILD_DIR="build/linux"
NO_KV_REGEX="^NoKvSurfaceAudit$"
RECOVERY_REGEX="^(RaftSnapshotRestartTest|RaftSnapshotRecoveryTest|RaftSnapshotCatchupTest)\\."

GROUP=""
SKIP_CONFIGURE=0
SKIP_BUILD=0

show_usage() {
  cat <<'EOF'
Usage:
  ./test.sh
  ./test.sh --group all [--skip-configure] [--skip-build]
  ./test.sh --group no-kv [--skip-configure] [--skip-build]
  ./test.sh --group recovery [--skip-configure] [--skip-build]
  ./test.sh --help

Groups:
  all
    统一全量入口；等价于默认不传 --group 的 configure/build/single-worker CTest 流程。
  no-kv
    轻量 no-KV 审计入口；只构建 no_kv_surface_audit target，并运行 NoKvSurfaceAudit。
  recovery
    snapshot / recovery / catch-up 低并发入口；固定 CTEST_PARALLEL_LEVEL=1，不与 no-kv 混跑。

Default:
  不传 --group 时，保持原 T051 Linux 全量 configure/build/single-worker CTest 流程。
EOF
}

run_step() {
  local name="$1"
  local log="$2"
  shift 2

  echo
  echo "==== ${name} ===="
  "$@" 2>&1 | tee "$log"
  return "${PIPESTATUS[0]}"
}

run_no_kv_group() {
  local config_log="tmp/test-logs/t059-no-kv-configure.log"
  local build_log="tmp/test-logs/t059-no-kv-build.log"
  local ctest_log="tmp/test-logs/t059-no-kv-ctest.log"
  local config_exit=0
  local build_exit=0
  local ctest_exit=0

  echo "==== no-kv audit ===="
  echo "Mode: lightweight no-KV audit"
  echo "Build target: no_kv_surface_audit"
  echo "CTest regex: ${NO_KV_REGEX}"
  echo "Recovery/snapshot restart tests are excluded from this group."

  if [ "${SKIP_CONFIGURE}" -eq 0 ]; then
    run_step "Configure (no-kv)" "${config_log}" cmake --preset "${PRESET}"
    config_exit=$?
  fi

  if [ "${SKIP_BUILD}" -eq 0 ]; then
    run_step "Build no_kv_surface_audit" "${build_log}" \
      cmake --build --preset "${PRESET}" --target no_kv_surface_audit
    build_exit=$?
  fi

  echo
  echo "==== CTest NoKvSurfaceAudit ===="
  ctest \
    --test-dir "${BUILD_DIR}" \
    --output-on-failure \
    -R "${NO_KV_REGEX}" \
    2>&1 | tee "${ctest_log}"
  ctest_exit=${PIPESTATUS[0]}

  echo
  echo "==== no-kv summary ===="
  if [ "${SKIP_CONFIGURE}" -eq 0 ]; then
    echo "Configure: $([ "${config_exit}" -eq 0 ] && echo PASS || echo FAIL)"
  else
    echo "Configure: SKIPPED"
  fi
  if [ "${SKIP_BUILD}" -eq 0 ]; then
    echo "Build: $([ "${build_exit}" -eq 0 ] && echo PASS || echo FAIL)"
  else
    echo "Build: SKIPPED"
  fi
  echo "CTest: $([ "${ctest_exit}" -eq 0 ] && echo PASS || echo FAIL)"
  echo "CTest log: ${ctest_log}"

  if [ "${config_exit}" -ne 0 ] || [ "${build_exit}" -ne 0 ] || [ "${ctest_exit}" -ne 0 ]; then
    exit 1
  fi

  exit 0
}

run_recovery_group() {
  local config_log="tmp/test-logs/t059-recovery-configure.log"
  local build_log="tmp/test-logs/t059-recovery-build.log"
  local ctest_log="tmp/test-logs/t059-recovery-ctest.log"
  local config_exit=0
  local build_exit=0
  local ctest_exit=0

  echo "==== recovery validation ===="
  echo "Mode: low-concurrency snapshot/recovery/catch-up validation"
  echo "CTest regex: ${RECOVERY_REGEX}"
  echo "CTEST_PARALLEL_LEVEL=1"

  if [ "${SKIP_CONFIGURE}" -eq 0 ]; then
    run_step "Configure (recovery)" "${config_log}" cmake --preset "${PRESET}"
    config_exit=$?
  fi

  if [ "${SKIP_BUILD}" -eq 0 ]; then
    run_step "Build recovery targets" "${build_log}" \
      cmake --build --preset "${PRESET}" \
      --target snapshot_test test_raft_snapshot_catchup test_raft_snapshot_restart test_metadata_recovery_stress
    build_exit=$?
  fi

  echo
  echo "==== CTest recovery single worker ===="
  export CTEST_PARALLEL_LEVEL=1
  ctest \
    --test-dir "${BUILD_DIR}" \
    --output-on-failure \
    -j 1 \
    -R "${RECOVERY_REGEX}" \
    2>&1 | tee "${ctest_log}"
  ctest_exit=${PIPESTATUS[0]}

  echo
  echo "==== recovery summary ===="
  if [ "${SKIP_CONFIGURE}" -eq 0 ]; then
    echo "Configure: $([ "${config_exit}" -eq 0 ] && echo PASS || echo FAIL)"
  else
    echo "Configure: SKIPPED"
  fi
  if [ "${SKIP_BUILD}" -eq 0 ]; then
    echo "Build: $([ "${build_exit}" -eq 0 ] && echo PASS || echo FAIL)"
  else
    echo "Build: SKIPPED"
  fi
  echo "CTest: $([ "${ctest_exit}" -eq 0 ] && echo PASS || echo FAIL)"
  echo "CTest log: ${ctest_log}"

  if [ "${config_exit}" -ne 0 ] || [ "${build_exit}" -ne 0 ] || [ "${ctest_exit}" -ne 0 ]; then
    exit 1
  fi

  exit 0
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --group)
      if [ "$#" -lt 2 ]; then
        echo "Missing value for --group" >&2
        exit 1
      fi
      GROUP="$2"
      shift 2
      ;;
    --skip-configure)
      SKIP_CONFIGURE=1
      shift
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    -h|--help)
      show_usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      show_usage >&2
      exit 1
      ;;
  esac
done

case "${GROUP}" in
  "")
    ;;
  all)
    ;;
  no-kv)
    run_no_kv_group
    ;;
  recovery)
    run_recovery_group
    ;;
  *)
    echo "Unsupported group: ${GROUP}" >&2
    show_usage >&2
    exit 1
    ;;
esac

CONFIG_LOG="tmp/test-logs/t051-linux-configure.log"
BUILD_LOG="tmp/test-logs/t051-linux-build.log"
CTEST_LOG="tmp/test-logs/t051-linux-full-ctest-single-worker.log"
FAILED_FILE="tmp/test-logs/t051-linux-failed-tests.md"
REPORT_FILE="specs/006-remove-kv-metadata-state-machine/task-reports/T051-linux-final-validation.md"

run_step "Configure" "$CONFIG_LOG" cmake --preset "${PRESET}"
CONFIG_EXIT=$?

run_step "Build" "$BUILD_LOG" cmake --build --preset "${PRESET}"
BUILD_EXIT=$?

echo
echo "==== CTest Full Single Worker ===="
export CTEST_PARALLEL_LEVEL=1

ctest \
  --test-dir "${BUILD_DIR}" \
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
