#!/usr/bin/env bash
set -Eeuo pipefail

# Run Raft project tests in a stable, grouped order.
#
# Usage:
#   ./test.sh
#   ./test.sh --clean
#   ./test.sh --skip-configure
#   ./test.sh --skip-build
#   ./test.sh --keep-data
#   ./test.sh --group persistence
#   ./test.sh --group all
#
# Groups:
#   unit
#   snapshot-storage
#   kv-service
#   segment-basic
#   election
#   replication
#   persistence
#   snapshot-recovery
#   integration
#   snapshot-catchup
#   snapshot-restart
#   diagnosis
#   replicator
#   segment-cluster
#   all

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

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
  sed -n '1,34p' "$0"
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

  log_section "Running test group: ${name}"
  if [[ "${KEEP_DATA}" -eq 1 ]]; then
    RAFT_TEST_KEEP_DATA=1 ctest --test-dir "${BUILD_DIR}" -j"${TEST_JOBS}" \
      --output-on-failure --stop-on-failure -R "${regex}"
  else
    ctest --test-dir "${BUILD_DIR}" -j"${TEST_JOBS}" \
      --output-on-failure --stop-on-failure -R "${regex}"
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
    kv-service)
      run_ctest_group "kv-service" "^RaftKvServiceTest\."
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
    segment-cluster)
      run_ctest_group "segment-cluster" "^RaftSegmentStorageTest\.RaftClusterGeneratesManySnapshotsAndSegmentLogsUnderBuildDirectory$"
      ;;
    all)
      run_group_by_name unit
      run_group_by_name snapshot-storage
      run_group_by_name kv-service
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

run_group_by_name "${SELECTED_GROUP}"

log_section "All requested tests passed"
