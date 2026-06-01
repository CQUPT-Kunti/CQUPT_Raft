# T056 Cleanup Candidate Generation

## 修改文件

- `modules/store/maintenance/garbage_collector.h`
- `modules/store/maintenance/garbage_collector.cpp`
- `modules/store/maintenance/module-notes.md`
- `tests/storage_garbage_collector_test.cpp`
- `tests/storage_delete_gc_test.cpp`
- `tests/storage_upload_coordinator_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/task-reports/t056-cleanup-candidate-generation.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 为 GC 增加通用 `CleanupCandidate` / `CleanupChunkFact` 模型和四类生成入口：
  - pending timeout
  - failed upload
  - abort cleanup
  - deleted object cleanup
- 为 candidate generation 增加统一的 `metadata_boundary` 构造、稳定排序和去重逻辑
- 新增 `CleanupCandidateToGarbageCollectorTask(...)`，把 candidate 映射成后续可提交给 `GarbageCollector` 的任务
- 扩展测试，覆盖：
  - pending timeout 生成与未超时不生成
  - failed upload candidate -> GC task 转换
  - abort / deleted object 状态边界
  - committed object 不生成 failed-upload candidate
  - deleted cleanup candidate 进入 `GarbageCollector` 后仍受 T055 live-manifest safety gate 保护
  - `UploadCoordinator` 失败路径中的 durable cleanup facts 可以生成通用 cleanup candidates

## cleanup candidate 输入、输出和生成语义

- 输入：
  - object metadata 基本事实：
    - `bucket`
    - `object_key`
    - `object_id`
    - `version`
    - `object_state`
  - durable chunk facts：
    - `chunk_id` 或 `object_id + version + chunk_index`
    - `offset`
    - `size`
    - `checksum`
    - `replica_nodes`
  - source-specific 边界：
    - pending timeout：`created_at_unix_ms`、`now_unix_ms`、`timeout_ms`
    - failed upload / abort / deleted：`created_at_unix_ms`
- 输出：
  - `CleanupCandidate`
  - 保留：
    - `source`
    - `reason`
    - `object_state`
    - `bucket/object_key`
    - `chunk identity`
    - `size/checksum/replica_nodes`
    - `metadata_boundary`
    - `created_at_unix_ms`
    - `deadline_unix_ms`
- 生成语义：
  - pending timeout：
    - 仅 `object_state == kPending`
    - 且 `timeout_ms > 0`
    - 且 `now_unix_ms >= created_at_unix_ms + timeout_ms`
    - 生成 `kOrphanChunkCleanup`
  - failed upload：
    - 已 committed object 不生成
    - 其余 durable 残留生成 `kFailedUploadCleanup`
  - abort cleanup：
    - 仅 `kAborted` 或 `kDeleted` object 生成 `kAbortCleanup`
  - deleted object cleanup：
    - 仅 `kDeleted` object 生成 `kDeletedObjectCleanup`
  - 排序：
    - `chunk_index -> offset -> chunk_id -> bucket -> object_key -> source`
  - 去重：
    - `bucket + object_key + chunk_id + source`

## pending timeout / failed upload / abort cleanup 当前边界

- pending timeout 只负责 candidate generation，不直接改 metadata，也不直接触发删除
- failed upload 当前复用 upload 路径已经暴露的 durable cleanup facts，再转成 generic candidate
- abort cleanup 当前只固定 object state boundary 和 candidate 语义，不扩展生产 AbortObject 后台流程
- deleted object cleanup 也只生成 candidate，live manifest 保护仍由 T055 safety checker 决定
- 当前没有 cleanup persistence、延迟调度器或 restart resume

## candidate -> GarbageCollectorTask 转换语义

- `task_id = "gc-candidate/<Source>/<chunk_id>"`
- 保留：
  - `chunk_id`
  - `object_id`
  - `version`
  - `chunk_index`
  - `reason`
  - `metadata_boundary`
- 不在转换阶段做：
  - metadata / Raft 调用
  - safety decision
  - delete handler 调用
  - restart persistence

## 是否调用 metadata / Raft；是否真实删除 chunk

- `garbage_collector` 生产代码本身不直接调用 metadata / Raft
- `garbage_collector` 生产代码本身不直接执行 metadata 扫描
- 本次 candidate generation 不真实删除 chunk
- 本次集成测试中：
  - `storage_delete_gc` 把 deleted cleanup candidate 转成 task 后送入真实 `GarbageCollector`
  - 删除前仍经过真实 metadata-driven safety checker
  - live manifest 仍可阻止 delete handler

## 是否使用 tests/test_file/test_file.zip

- 否
- 本次新增 T056 测试使用 `MakeChunkPayload(...)`，没有新增二进制 fixture 依赖

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - 日志路径：未单独保存 build 日志
- `ctest --test-dir build/linux -R "cleanup_candidate|garbage_collector|storage_garbage_collector|storage_delete_gc|upload_coordinator" --output-on-failure 2>&1 | tee tmp/007/t056-cleanup-candidates.log`
  - PASS
  - 日志路径：`tmp/007/t056-cleanup-candidates.log`
  - 说明：实际匹配到的测试名为 `storage_delete_gc`、`storage_garbage_collector`、`storage_upload_coordinator`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志摘要

- 本次验证未失败

## Windows 验证判断

- T056 是平台无关的 candidate generation / GC safety 集成任务
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本任务未新增 `T056-WIN`

## 是否通过 T056

- 是

## 是否可以进入 T057

- 可以
- T057 应只继续补 cleanup persistence / restart resume，不回头扩展 candidate generation、proto 或生产 GC 范围

## 当前任务发现的不合理点 / 警告 / 风险

- candidate generation 当前仍是进程内事实生成，没有 cleanup persistence / restart resume
- candidate 生成的正确性仍依赖调用方提供的 metadata snapshot、object state 和 timeout 事实是否新鲜
- `next_retry_after_ms` 仍只是 task model 扩展点，没有真正的延迟重试调度器
- Windows 删除语义、timeout/cancellation 运行中传播、corruption 自动回写仍未解决
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/maintenance/module-notes.md`
- 未修改 `AGENTS.md`

## module-notes.md 是否补充了 .cpp 关键函数 / helper

- 是
- 已补：
  - `BuildMetadataBoundary(...)`
  - `NormalizeCleanupChunkFact(...)`
  - `NormalizeCleanupCandidates(...)`
  - `BuildCleanupCandidates(...)`
  - `BuildPendingTimeoutCleanupCandidates(...)`
  - `BuildFailedUploadCleanupCandidates(...)`
  - `BuildAbortCleanupCandidates(...)`
  - `BuildDeletedObjectCleanupCandidates(...)`
  - `CleanupCandidateToGarbageCollectorTask(...)`

## 是否修改高频文档及原因

- 修改了 `tasks.md`
  - 原因：标记 T056 完成，并把实际修改路径修正到本次真实范围
- 修改了 `common-risk-notes.md`
  - 原因：删除/收缩已解决风险，补充 candidate generation 仍存在的持久化与 metadata 新鲜度风险

## common-risk-notes.md 读取结果

- 已重新读取并核对现有风险项
- 保留：
  - `T001` prerequisites 脚本错误指向 006
  - `T014/T023/T025/T026` Windows 待验证
  - `T019` timeout/cancellation / owner-thread shutdown 边界
  - `T024` corruption 自动回写未实现
  - `T027` pending/orphan 失败路径仍需 restart 收口
  - `T045` heartbeat/registry/failure cache/read-side 真实接线未完成
  - `T049/T055` restart cleanup、metadata fact source、延迟重试等剩余风险

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T056` candidate generation 仍缺 cleanup persistence / restart resume，且依赖 metadata fact 新鲜度
- 删除：
  - 无整项删除
- 收缩：
  - `T049`、`T055` 已去掉“pending/failed upload/abort candidate generation 未实现”的表述
