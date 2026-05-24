#!/usr/bin/env bash
set -Eeuo pipefail

# Run Raft project tests in a stable, grouped order.
#
# test.sh section map:
#   Platform-neutral base regression groups:
#     unit snapshot-storage segment-basic election replication
#     integration snapshot-catchup snapshot-restart replicator
#   Shared restart / durability regression:
#     persistence
#   No-KV surface audit:
#     no-kv
#   Linux-specific / Linux-primary focus groups:
#     snapshot-recovery diagnosis segment-cluster
#   Linux Bash primary sweep:
#     all
#
# --keep-data:
#   Linux Bash-first retained-artifact mode for reruns that need
#   raft_data / raft_snapshots / build/linux/tests/raft_test_data for diagnosis.
#
# Non-Bash / cross-platform fallback:
#   ctest --preset debug-tests --output-on-failure

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build/linux"

DO_CLEAN=0
DO_CONFIGURE=1
DO_BUILD=1
KEEP_DATA=0
SELECTED_GROUP="all"

BUILD_JOBS="${RAFT_BUILD_JOBS:-1}"
LINK_JOBS="${RAFT_LINK_JOBS:-1}"
PROTO_JOBS="${RAFT_PROTO_JOBS:-1}"
TEST_JOBS="${CTEST_PARALLEL_LEVEL:-1}"

print_usage() {
  cat <<'EOF'
Usage:
  ./test.sh
  ./test.sh --clean
  ./test.sh --skip-configure
  ./test.sh --skip-build
  ./test.sh --keep-data
  ./test.sh --group persistence
  ./test.sh --group all

Platform-neutral base regression groups:
  unit               Basic unit tests for commands, scheduler, and thread pool
  snapshot-storage   Snapshot storage reliability coverage
  segment-basic      Focused segment storage persistence cases
  election           Leader election and split-brain related checks
  replication        Log replication and commit/apply progression
  integration        Multi-node Raft integration scenarios
  snapshot-catchup   Lagging follower catch-up and snapshot handoff
  snapshot-restart   Restart after compacted snapshot scenarios
  replicator         Single follower replication and catch-up behavior
  no-kv              Audit retired KV service/client/proto/doc surfaces

Shared restart / durability regression:
  persistence        Restart recovery and hard-state/log trusted-state checks
                     Recovery logic is platform-neutral; Linux-specific durability
                     interpretation remains documented separately.

Linux-specific / Linux-primary focus groups:
  snapshot-recovery  Snapshot/restart recovery hotspot
  diagnosis          Recovery diagnosis and snapshot fallback hotspot
  segment-cluster    Clustered segment/snapshot stress path

Linux Bash primary sweep:
  all                Runs platform-neutral base regression groups first,
                     then no-kv, then persistence, then Linux-specific / Linux-primary
                     focus groups, followed by a final full-suite check.

--keep-data:
  Keep Linux test artifacts under raft_data / raft_snapshots /
  build/linux/tests/raft_test_data for failure diagnosis.
  Use it when investigating restart, snapshot, catch-up, replicator, or
  segment-cluster failures. This is a Linux Bash-first workflow and is not
  replaced by the non-Bash fallback.

Failure rerun guide:
  Re-run the failed group with CTEST_PARALLEL_LEVEL=1.
  Add --keep-data when retained artifacts are required for diagnosis.
  High-risk rerun commands:
    CTEST_PARALLEL_LEVEL=1 ./test.sh --group snapshot-recovery --keep-data
    CTEST_PARALLEL_LEVEL=1 ./test.sh --group diagnosis --keep-data
    CTEST_PARALLEL_LEVEL=1 ./test.sh --group snapshot-catchup --keep-data
    CTEST_PARALLEL_LEVEL=1 ./test.sh --group replicator --keep-data
    CTEST_PARALLEL_LEVEL=1 ./test.sh --group segment-cluster --keep-data

Non-Bash / cross-platform fallback:
  ctest --preset debug-tests --output-on-failure
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean)
      DO_CLEAN=1
      shift
      ;;
    --skip-configure)
      DO_CONFIGURE=0
      shift
      ;;
    --skip-build)
      DO_BUILD=0
      shift
      ;;
    --keep-data)
      KEEP_DATA=1
      shift
      ;;
    --group)
      if [[ $# -lt 2 ]]; then
        echo "error: --group requires a value" >&2
        exit 2
      fi
      SELECTED_GROUP="$2"
      shift 2
      ;;
    -h|--help)
      print_usage
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      print_usage
      exit 2
      ;;
  esac
done

cd "${PROJECT_ROOT}"

log_section() {
  echo
  echo "============================================================"
  echo "$1"
  echo "============================================================"
}

print_group_catalog() {
  cat <<'EOF'
Platform-neutral base regression groups:
  unit snapshot-storage segment-basic election replication
  integration snapshot-catchup snapshot-restart replicator

Shared restart / durability regression:
  persistence
  Recovery logic is platform-neutral; Linux-specific durability interpretation
  remains documented separately.

No-KV surface audit:
  no-kv
  Enforces retired KV service/client/proto/doc surfaces stay removed while
  tolerating the documented legacy blockers.

Linux-specific / Linux-primary focus groups:
  snapshot-recovery diagnosis segment-cluster

Linux Bash primary sweep:
  all = platform-neutral base regression groups + no-kv + persistence +
        Linux-specific / Linux-primary focus groups + final full-suite check

--keep-data:
  Linux Bash-first retained-artifact mode for raft_data / raft_snapshots /
  build/linux/tests/raft_test_data.

Failure rerun guide:
  Re-run the failed group with CTEST_PARALLEL_LEVEL=1.
  Add --keep-data when retained artifacts are needed for diagnosis.

Non-Bash / cross-platform fallback:
  ctest --preset debug-tests --output-on-failure
EOF
}

print_group_classification() {
  local group="$1"

  case "${group}" in
    unit|snapshot-storage|segment-basic|election|replication|integration|snapshot-catchup|snapshot-restart|replicator)
      echo "Section: platform-neutral base regression."
      ;;
    no-kv)
      echo "Section: no-KV surface audit."
      ;;
    persistence)
      echo "Section: shared restart / durability regression."
      echo "Note: recovery logic is platform-neutral; Linux-specific durability interpretation remains separate."
      ;;
    snapshot-recovery|diagnosis|segment-cluster)
      echo "Section: Linux-specific / Linux-primary focus group."
      ;;
    all)
      echo "Section: Linux Bash primary sweep."
      echo "Order: no-kv -> platform-neutral base regression -> persistence -> Linux-specific / Linux-primary focus groups -> final full-suite check."
      ;;
  esac
}

print_group_guidance() {
  local group="$1"

  print_group_classification "${group}"

  case "${group}" in
    snapshot-recovery)
      echo "Purpose: snapshot / restart recovery hotspot (Linux primary)."
      echo "Hint: prefer CTEST_PARALLEL_LEVEL=1; add --keep-data to retain recovery artifacts."
      ;;
    diagnosis)
      echo "Purpose: recovery diagnosis, snapshot fallback, and failure localization hotspot (Linux primary)."
      echo "Hint: prefer CTEST_PARALLEL_LEVEL=1; add --keep-data when investigating snapshot skip / fallback."
      ;;
    snapshot-catchup)
      echo "Purpose: lagging follower catch-up and snapshot handoff validation."
      echo "Hint: prefer CTEST_PARALLEL_LEVEL=1; add --keep-data when follower catch-up state needs inspection."
      ;;
    replicator)
      echo "Purpose: single follower replication state machine and catch-up behavior."
      echo "Hint: prefer CTEST_PARALLEL_LEVEL=1; add --keep-data when diagnosing replication state drift."
      ;;
    no-kv)
      echo "Purpose: fail fast when retired KV service/client/proto/doc surfaces reappear."
      echo "Hint: this audit tolerates the documented legacy Command/KvStateMachine blockers and reports them separately."
      ;;
    segment-cluster)
      echo "Purpose: clustered segment / snapshot stress path."
      echo "Hint: prefer CTEST_PARALLEL_LEVEL=1; add --keep-data to retain generated segment and snapshot artifacts."
      ;;
  esac
}

print_failure_rerun_hint() {
  local group="$1"

  echo
  echo "Failure localization hints:"
  echo "  - Low-concurrency recommendation: CTEST_PARALLEL_LEVEL=1"
  echo "  - Keep Linux artifacts when needed: --keep-data"

  case "${group}" in
    snapshot-recovery)
      echo "  - Rerun: CTEST_PARALLEL_LEVEL=1 ./test.sh --group snapshot-recovery --keep-data"
      echo "  - CTest fallback: CTEST_PARALLEL_LEVEL=1 ctest --test-dir \"${BUILD_DIR}\" --output-on-failure -R '^RaftSnapshotRecoveryTest\\.'"
      echo "  - Failure focus: leader churn during recovery, snapshot restore, restart trusted-state."
      ;;
    diagnosis)
      echo "  - Rerun: CTEST_PARALLEL_LEVEL=1 ./test.sh --group diagnosis --keep-data"
      echo "  - CTest fallback: CTEST_PARALLEL_LEVEL=1 ctest --test-dir \"${BUILD_DIR}\" --output-on-failure -R '^RaftSnapshotDiagnosisTest\\.'"
      echo "  - Failure focus: snapshot skip/fallback, restart diagnostics, trusted-state explanation."
      ;;
    snapshot-catchup)
      echo "  - Rerun: CTEST_PARALLEL_LEVEL=1 ./test.sh --group snapshot-catchup --keep-data"
      echo "  - CTest fallback: CTEST_PARALLEL_LEVEL=1 ctest --test-dir \"${BUILD_DIR}\" --output-on-failure -R '^RaftSnapshotCatchupTest\\.'"
      echo "  - Failure focus: follower catch-up via log replay or snapshot handoff."
      ;;
    replicator)
      echo "  - Rerun: CTEST_PARALLEL_LEVEL=1 ./test.sh --group replicator --keep-data"
      echo "  - CTest fallback: CTEST_PARALLEL_LEVEL=1 ctest --test-dir \"${BUILD_DIR}\" --output-on-failure -R '^RaftReplicatorBehaviorTest\\.'"
      echo "  - Failure focus: follower replication state machine and catch-up behavior."
      ;;
    no-kv)
      echo "  - Rerun: CTEST_PARALLEL_LEVEL=1 ./test.sh --group no-kv"
      echo "  - CTest fallback: ctest --test-dir \"${BUILD_DIR}\" --output-on-failure -R '^NoKvSurfaceAudit$'"
      echo "  - Build target fallback: cmake --build \"${BUILD_DIR}\" --target no_kv_surface_audit"
      echo "  - Failure focus: retired KV service/client/proto/doc surfaces unexpectedly reappeared."
      ;;
    segment-cluster)
      echo "  - Rerun: CTEST_PARALLEL_LEVEL=1 ./test.sh --group segment-cluster --keep-data"
      echo "  - CTest fallback: CTEST_PARALLEL_LEVEL=1 ctest --test-dir \"${BUILD_DIR}\" --output-on-failure -R '^RaftSegmentStorageTest\\.RaftClusterGeneratesManySnapshotsAndSegmentLogsUnderBuildDirectory$'"
      echo "  - Failure focus: segment rollover, clustered snapshot generation, retained artifacts under load."
      ;;
    *)
      echo "  - Rerun: CTEST_PARALLEL_LEVEL=1 ./test.sh --group ${group}"
      echo "  - Platform-neutral fallback: ctest --preset debug-tests --output-on-failure"
      ;;
  esac
}

clean_test_data() {
  log_section "Cleaning old Raft test data"
  rm -rf "${PROJECT_ROOT}/raft_data"
  rm -rf "${PROJECT_ROOT}/raft_snapshots"
  rm -rf "${BUILD_DIR}/tests/raft_test_data"
  rm -rf /tmp/raftdemo_tests
  rm -rf /tmp/raftdemo_gtests
  rm -rf /tmp/raft_kv_gtest_*
}

configure_project() {
  log_section "Configuring CMake"
  cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
    -DRAFT_TEST_FULL_SUITE=ON \
    -DRAFT_BUILD_JOBS="${BUILD_JOBS}" \
    -DRAFT_LINK_JOBS="${LINK_JOBS}" \
    -DRAFT_PROTO_JOBS="${PROTO_JOBS}"
}

build_project() {
  log_section "Building project"
  cmake --build "${BUILD_DIR}" --parallel "${BUILD_JOBS}"
}

run_ctest_group() {
  local name="$1"
  local regex="$2"
  local status=0

  log_section "Running test group: ${name}"
  print_group_guidance "${name}"
  if [[ "${KEEP_DATA}" -eq 1 ]]; then
    if ! RAFT_TEST_KEEP_DATA=1 ctest --test-dir "${BUILD_DIR}" -j"${TEST_JOBS}" \
      --output-on-failure --stop-on-failure -R "${regex}"; then
      status=$?
    fi
  else
    if ! ctest --test-dir "${BUILD_DIR}" -j"${TEST_JOBS}" \
      --output-on-failure --stop-on-failure -R "${regex}"; then
      status=$?
    fi
  fi

  if [[ "${status}" -ne 0 ]]; then
    print_failure_rerun_hint "${name}"
    return "${status}"
  fi
}

run_group_by_name() {
  local group="$1"

  case "${group}" in
    unit)
      run_ctest_group "unit" "^(CommandTest|KvStateMachineTest|TimerSchedulerTest|ThreadPoolTest)\."
      ;;
    snapshot-storage)
      run_ctest_group "snapshot-storage" "^SnapshotStorageReliabilityTest\."
      ;;
    segment-basic)
      run_ctest_group "segment-basic" "^RaftSegmentStorageTest\.(WritesMultipleSegmentFilesUnderBuildDirectory|AutomaticallyDeletesObsoleteSegmentsAfterCompactionSave)$"
      ;;
    election)
      run_ctest_group "election" "^RaftElectionTest\."
      ;;
    replication)
      run_ctest_group "replication" "^(RaftLogReplicationTest|RaftCommitApplyTest)\."
      ;;
    persistence)
      run_ctest_group "persistence" "^PersistenceTest\."
      ;;
    snapshot-recovery)
      run_ctest_group "snapshot-recovery" "^RaftSnapshotRecoveryTest\."
      ;;
    integration)
      run_ctest_group "integration" "^RaftIntegrationTest\."
      ;;
    snapshot-catchup)
      run_ctest_group "snapshot-catchup" "^RaftSnapshotCatchupTest\."
      ;;
    snapshot-restart)
      run_ctest_group "snapshot-restart" "^RaftSnapshotRestartTest\."
      ;;
    diagnosis)
      run_ctest_group "diagnosis" "^RaftSnapshotDiagnosisTest\."
      ;;
    replicator)
      run_ctest_group "replicator" "^RaftReplicatorBehaviorTest\."
      ;;
    no-kv)
      run_ctest_group "no-kv" "^NoKvSurfaceAudit$"
      ;;
    segment-cluster)
      run_ctest_group "segment-cluster" "^RaftSegmentStorageTest\.RaftClusterGeneratesManySnapshotsAndSegmentLogsUnderBuildDirectory$"
      ;;
    all)
      run_group_by_name no-kv
      run_group_by_name unit
      run_group_by_name snapshot-storage
      run_group_by_name segment-basic
      run_group_by_name election
      run_group_by_name replication
      run_group_by_name persistence
      run_group_by_name snapshot-recovery
      run_group_by_name integration
      run_group_by_name snapshot-catchup
      run_group_by_name snapshot-restart
      run_group_by_name diagnosis
      run_group_by_name replicator
      run_group_by_name segment-cluster

      log_section "Running final full-suite check"
      if [[ "${KEEP_DATA}" -eq 1 ]]; then
        RAFT_TEST_KEEP_DATA=1 ctest --test-dir "${BUILD_DIR}" -j"${TEST_JOBS}" \
          --output-on-failure --stop-on-failure
      else
        ctest --test-dir "${BUILD_DIR}" -j"${TEST_JOBS}" \
          --output-on-failure --stop-on-failure
      fi
      ;;
    *)
      echo "error: unknown group: ${group}" >&2
      print_usage
      exit 2
      ;;
  esac
}

if [[ "${DO_CLEAN}" -eq 1 ]]; then
  clean_test_data
fi

if [[ "${DO_CONFIGURE}" -eq 1 ]]; then
  configure_project
fi

if [[ "${DO_BUILD}" -eq 1 ]]; then
  build_project
fi

log_section "test.sh section map"
print_group_catalog

run_group_by_name "${SELECTED_GROUP}"

log_section "All requested tests passed"
