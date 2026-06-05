# Quickstart: Storage Node Data Plane

**Feature**: `007-storage-node-data-plane`  
**Status**: Final Linux validation has passed with targeted pre-cleanup for the snapshot diagnosis test; Windows validation is still pending.

## Current Status

- StorageNode / chunk data-plane production path has been implemented through T092.
- US6 scrub / repair / rebalance low-concurrency validation has passed.
- no-KV audit has passed for the current 007 scope.
- storage-node-concurrency validation has passed.
- recovery / snapshot / catch-up low-concurrency validation has passed.
- T093 final Linux aggregation has now passed.
- The last unstable item was:
  - `RaftSnapshotDiagnosisTest.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot`
- The passing T093 rerun used:
  - full pre-clean of test snapshot/runtime directories
  - an extra cleanup immediately before test `165`
- Linux can now be treated as closed for T093.
- Windows validation has not been run in a real Windows environment.

## Build Entry

Preferred low-concurrency preset:

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel
```

More conservative preset:

```bash
cmake --preset debug-ninja-safe
cmake --build --preset debug-ninja-safe
```

## Verified Linux Validation Entry

US6 scrub / repair / rebalance low-concurrency validation:

```bash
CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux -R "storage_scrub_repair|storage_rebalance" --output-on-failure
```

no-KV audit:

```bash
cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit
```

storage concurrency validation:

```bash
ctest --test-dir build/linux -L storage-node-concurrency --output-on-failure
```

recovery / snapshot / catch-up low-concurrency validation:

```bash
CTEST_PARALLEL_LEVEL=1 ./test.sh --group recovery --skip-configure --skip-build
```

## T093 Validation Entry

The following commands describe the validation path that most recently passed.

First clean test runtime leftovers:

```bash
mkdir -p tmp/007
find tmp build/linux -type d \( -iname "*snapshot*" -o -iname "*recovery*" -o -iname "*restart*" -o -iname "*node-data*" -o -iname "*storage-node*" \) 2>/dev/null | tee tmp/007/t093-cleanup-candidates.log
```

Then remove only confirmed test runtime directories such as:

```bash
rm -rf tmp/test-logs build/linux/tests/raft_test_data raft_test_data
```

Current passing full-coverage Linux aggregation path:

```bash
CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux -I 1,164,1 --output-on-failure -j 1
CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux -I 165,165,1 --output-on-failure -j 1
CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux -I 166,225,1 --output-on-failure -j 1
```

Immediately before running test `165`, delete diagnosis runtime leftovers:

```bash
find build/linux/tests/raft_test_data -maxdepth 1 -type d -name 'raft_snapshot_diagnosis_*' -exec rm -rf {} +
find raft_test_data -maxdepth 1 -type d -name 'raft_snapshot_diagnosis_*' -exec rm -rf {} +
```

Latest observed result:

- PASS

Operational note:

- `RaftSnapshotDiagnosisTest.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot` has shown runtime-dir / recovery-timing sensitivity in earlier runs.
- When rerunning the final Linux aggregation, keep the same pre-cleanup step.

## Windows Validation Entry

Windows validation is still pending.

- No real Windows build/test environment is available in the current workspace.
- Do not claim Windows PASS.
- Keep Windows durability / file-semantics validation as follow-up work.

Suggested future Windows validation areas:

- `FlushFileBuffers`
- Windows file handle behavior
- `MoveFileEx` / `ReplaceFile` publish semantics
- Windows long path
- UTF-8 path
- disk full
- permission denied
- sharing violation
- partial write
- staging cleanup
- atomic publish
- checksum mismatch
- restart index rebuild

## No-KV Audit Reminder

007 follow-up work must not reintroduce:

- `CommandType::kSet`
- `CommandType::kDelete`
- `KvStateMachine`
- `KvService`
- `raft_kv_client`
- `DebugGetValue`
- KV proto
- KV target
- KV fallback
- KV regression-only path
- `SetCommand` / `DeleteCommand` / KV state machine assertions

## Test Log Rule Reminder

When running real validations:

- PASS output should only report command, PASS, and total duration.
- FAIL output should only report failing test names, key assertions, failure category, last 50 log lines, and full log path.
- Do not paste full Raft node logs into chat unless explicitly requested.
