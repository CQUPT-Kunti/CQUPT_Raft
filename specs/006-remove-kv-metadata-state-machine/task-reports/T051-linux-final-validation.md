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

- 	 82 - MetadataFailoverTest.SameCommitRequestIdCanBeRetriedOnNewLeader (Failed)
- 	 92 - MetadataConcurrencyStressTest.ConcurrentDuplicateRequestIdProposalsShareOneLogEntryAndOneApply (Subprocess aborted)
- 	210 - IntegratedObjectStorageQuorumTest.ThreeVoterCommittedMembershipDoesNotShrinkQuorumWhenOnlyOneNodeRemainsLive (Failed)
- 	276 - storage_upload_coordinator (Failed)

## Logs

- Configure: tmp/test-logs/t051-linux-configure.log
- Build: tmp/test-logs/t051-linux-build.log
- CTest: tmp/test-logs/t051-linux-full-ctest-single-worker.log
- Failed tests summary: tmp/test-logs/t051-linux-failed-tests.md
