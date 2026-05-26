# Contract Draft: Cross-Platform Chunk Durability

**Feature**: `007-storage-node-data-plane`  
**Status**: Draft planning contract. This file does not change production code, proto, CMake, or tests.  
**Scope**: StorageNode chunk data durability on Linux and Windows.

## Non-Negotiable Rules

- StorageNode chunk durability is independent from Raft log/snapshot durability.
- Required durability operations must not silently degrade.
- A platform branch may return explicit `UNSUPPORTED` or document a weaker contract, but it must not return success after a no-op durability operation.
- Object payload bytes must never be written to Raft log, Raft snapshot, or metadata snapshot.
- Staging files must never be served as committed chunk data.

## Write Publish Contract

The future `LocalDiskChunkStore::WriteChunk` implementation should follow this durable publish sequence:

1. Validate `chunk_id`, expected size, expected checksum, and normalized path.
2. Create a staging file in the same filesystem as the final chunk path.
3. Write bytes while detecting short write, cancellation, disk full, permission denied, and IO error.
4. Compute checksum while writing or before publish.
5. Compare computed checksum and size with expected values.
6. Flush staging file data using the platform durable file operation.
7. Atomically publish staging to final chunk path.
8. Flush/sync the parent directory or return/document explicit weaker semantics.
9. Update `ChunkIndex` only after publish boundary succeeds.

## Linux Requirements

| Operation | Requirement | Failure Handling |
|-----------|-------------|------------------|
| File write | Handle short write, `EINTR`, `ENOSPC`, `EACCES`, `EIO` | Return explicit error and keep chunk out of live index |
| Data flush | Use `fdatasync` or `fsync` on staging file before publish | Return explicit IO/durability error |
| Atomic publish | Use same-filesystem rename from staging to final | Cross-device rename is invalid for publish |
| Directory sync | Open and `fsync` parent directory after publish | Return explicit error if required contract cannot be met |
| Staging cleanup | Startup scan removes stale/incomplete staging files | Never promote staging to live without final checksum validation |
| Path handling | Use normalized relative paths under data root | Reject traversal, absolute escape, invalid chunk id path |

## Windows Requirements

| Operation | Requirement | Failure Handling |
|-----------|-------------|------------------|
| File write | Use Windows file handle semantics and classify Win32 errors | Map disk full, access denied, IO error clearly |
| Data flush | Use `FlushFileBuffers` before publish | Return explicit IO/durability error |
| Atomic publish | Use `MoveFileEx` / `ReplaceFile` style semantics with documented flags | Handle target exists, sharing violation, handle lifecycle |
| Directory durability | Define whether parent directory durability is supported; if not, record weaker contract or explicit unsupported | No no-op success |
| Long path | Support or explicitly reject Windows long path with clear error | No silent truncation or path rewrite |
| UTF-8 path | Normalize UTF-8 input and convert safely to Windows path form | Invalid conversion returns explicit path error |
| Staging cleanup | Cleanup must tolerate leftover handles and retryable sharing violations | Never expose staging as live chunk |

## Shared Error Classification

The durable file layer should map platform-specific failures to shared categories:

- `disk_full`
- `permission_denied`
- `io_error`
- `checksum_mismatch`
- `corrupted`
- `partial_write`
- `path_invalid`
- `atomic_publish_failed`
- `directory_sync_failed`
- `timeout`
- `cancelled`
- `unsupported`

These categories feed `StorageNodeService` responses and StorageNode health counters.

## Crash Windows To Validate

| Crash Window | Expected Recovery |
|--------------|-------------------|
| Before staging flush | No live chunk; stale staging cleanup |
| After staging flush before rename | No live chunk unless final publish exists; staging cleanup or safe retry |
| After rename before parent directory sync | Recovery result follows explicit platform contract; must not treat uncertainty as silent success |
| During ChunkIndex update | Rebuild index from disk facts on restart |
| During delete tombstone | Delete retry remains idempotent |
| During physical delete | Missing/deleted chunk is idempotent success only if metadata permits cleanup |

## Validation Matrix

| Test Area | Linux | Windows |
|-----------|-------|---------|
| file flush | `fsync` / `fdatasync` failure and success | `FlushFileBuffers` failure and success |
| directory sync | parent directory sync after rename | supported contract or explicit unsupported/weaker contract |
| atomic publish | rename staging to final | `MoveFileEx` / `ReplaceFile` publish |
| path handling | UTF-8, traversal rejection | UTF-8, long path, reserved names |
| error mapping | disk full, permission denied, IO error | disk full, access denied, sharing violation, IO error |
| recovery | stale staging, partial write, corrupted quarantine | stale staging, partial write, corrupted quarantine |
| checksum | mismatch on write/read/scrub | mismatch on write/read/scrub |
| concurrency | high-concurrency chunk IO | high-concurrency chunk IO |

## Test Scheduling Guidance

- Storage high-concurrency tests may run in parallel.
- Crash/recovery/snapshot/catch-up class tests should run low-concurrency.
- Use `CTEST_PARALLEL_LEVEL=1` for recovery-heavy groups when needed.
- This contract does not modify CTest filters or CMakePresets in plan phase.
