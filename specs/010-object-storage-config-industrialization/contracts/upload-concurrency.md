# Contract: Upload Concurrency

## Scope

This contract defines bounded multi-chunk upload and parallel replica fan-out.

## Budgets

The upload session must enforce:

- `max_inflight_chunks`
- `max_inflight_bytes`
- per-chunk replica fan-out concurrency
- bounded retry/backoff attempts
- explicit timeout/deadline propagation where already supported

The system must acquire chunk and byte budget before reading chunk payload into memory. If either budget is exhausted, the reader waits or stops scheduling until existing work releases budget.

## Chunk Fan-Out

For each chunk:

- Writes to selected replica targets should run concurrently up to fan-out limit.
- A chunk becomes commit-eligible when durable successes reach `minimum_successful_writes`.
- Failed or slow replicas must not block the chunk indefinitely once success criteria and safe deadline handling are satisfied.
- Retryable failures must remain distinguishable from checksum mismatch, conflict and invalid argument failures.

## Error Aggregation

Upload result must preserve:

- chunk identity
- target node id and endpoint
- durable success count
- selected target count
- retryable failure facts
- non-retryable failure facts
- cleanup candidate facts for durable but uncommitted chunks

CommitObject must not be attempted if any required chunk is not commit-eligible.

## Cross-Platform Boundary

Concurrency must use standard C++ or existing project runtime abstractions. Stage 1 must not introduce Linux-only thread, timer, file descriptor or cancellation APIs in shared upload logic.
