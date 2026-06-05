# T051 Linux Final Validation

## Result summary

- Configure: PASS
- Build: PASS
- CTest: FAIL

## Execution mode

- CTEST_PARALLEL_LEVEL=1
- ctest -j 1
- Tests run one by one

## Failed tests

- 	165 - RaftSnapshotDiagnosisTest.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot (Failed)

## Logs

- Configure: tmp/test-logs/t051-linux-configure.log
- Build: tmp/test-logs/t051-linux-build.log
- CTest: tmp/test-logs/t051-linux-full-ctest-single-worker.log
- Failed tests summary: tmp/test-logs/t051-linux-failed-tests.md
