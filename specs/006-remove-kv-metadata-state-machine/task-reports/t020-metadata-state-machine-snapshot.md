# T020 MetadataStateMachine Snapshot 保存 / 加载最小逻辑

## 结论

- T020 已完成。
- `MetadataStateMachine::SaveSnapshot / LoadSnapshot` 已实现最小可用逻辑。
- Snapshot 已覆盖 `last_applied_index_`、`last_applied_term_`、`buckets_`、`objects_`、`object_index_`、`chunk_ref_index_`、`requests_`、`request_fingerprints_`、`tombstones_`。
- Linux 下已完成最小必要 configure / build / 定向 CTest 验证。
- 未接入 `RaftNode` 默认 wiring，未进入 T021。

## 实际修改

- 更新 `modules/raft/state_machine/metadata_state_machine.cpp`
  - 为 `MetadataStateMachine` 新增 bucket / object / chunk ref / request 的二进制序列化与反序列化 helper。
  - 实现 `SaveSnapshot(...)`：
    - 保存 applied boundary。
    - 保存 bucket / object / object_index / chunk_ref_index / request / fingerprint / tombstone 全量状态。
    - 采用 `*.tmp` 临时文件写入后 rename 的最小原子替换语义。
    - 增加空路径、建目录失败、打开文件失败、写入失败、flush 失败、rename 失败等错误返回。
  - 实现 `LoadSnapshot(...)`：
    - 校验 snapshot magic / version。
    - 恢复 applied boundary、bucket、object、index、request、fingerprint、tombstone。
    - 先加载到临时容器，全部成功后再一次性替换当前内存状态，避免半恢复。
    - 增加文件不存在、格式损坏、截断内容、未知版本、重复 key、状态不一致等错误返回。
  - 新增加载后的一致性校验：
    - 校验 `objects_` 与 `object_index_` 一致。
    - 校验 `committed object` 与 `chunk_ref_index_` 一致。
    - 校验 `deleted object` 与 `tombstones_` 一致。
    - 校验 `requests_` 与 `request_fingerprints_` 一致。
  - 同步把 `StrongConsistencyMetadataStateMachine` 旧 snapshot 常量显式改为独立版本号，避免与新格式混淆。
- 更新 `tests/metadata_state_machine_test.cpp`
  - 调整原 skeleton snapshot 测试：空状态 `SaveSnapshot` 现在应成功。
  - 新增 roundtrip 恢复测试，验证：
    - applied boundary 恢复。
    - bucket / object / object_index / chunk_ref_index 恢复。
    - request table / fingerprint table 恢复。
    - tombstone 恢复。
    - `HeadObject / ListObjects` 与恢复后内存状态一致。
    - LoadSnapshot 后重复 `request_id` 仍保持幂等，不重复 apply。
    - deleted / aborted 对象不会复活。
    - committed object 的 chunk refs 可恢复。
  - 新增损坏 snapshot 测试，验证 `LoadSnapshot` 失败且不污染已有内存状态。
  - 新增未知版本 snapshot 测试，验证 `LoadSnapshot` 返回 `VersionMismatch` 且不污染已有内存状态。
- 更新 `modules/raft/state_machine/AGENTS.md`
  - 同步 `MetadataStateMachine` 已具备最小 snapshot save/load 责任说明。

## Linux 验证

- 选择原因
  - 本任务修改了 `MetadataStateMachine` 源码和对应测试，属于状态机 snapshot 边界变更。
  - 按最小闭环执行 Linux configure + 受影响 target build + 对应测试过滤。
  - 未跑无关 target，未默认跑全量 CTest。

- 实际命令
  - `cmake --preset debug-ninja-low-parallel`
  - `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine`
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R "^MetadataStateMachineTest\\."`

- 结果
  - `cmake --preset debug-ninja-low-parallel`：PASS
  - `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine`：PASS
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R "^MetadataStateMachineTest\\."`：PASS，`28/28` 通过

- 日志
  - configure 结果记录：`tmp/test-logs/t020-configure.log`
  - build 日志：`tmp/test-logs/t020-build.log`
  - ctest 日志：`tmp/test-logs/t020-ctest.log`

## 未跑全量 CTest 的说明

- 本任务仅运行相关个别测试/构建验证，未运行全量 CTest。
- 原因：本次改动只影响 `MetadataStateMachine` 的 snapshot 保存/加载逻辑及其单测，不涉及默认 `RaftNode` wiring、service 路径、DataNode 或全局构建图。

## 风险与后续接入点

- 当前只实现 `MetadataStateMachine` 自身的 snapshot save/load。
- 尚未接入 `SnapshotStorage` 的 staging / publish 流程。
- 尚未接入 `RaftNode` 默认 snapshot/replay 主路径。
- Snapshot 格式已加 version，但后续若继续扩展字段，需要显式维护版本升级兼容策略。

## 验收结果

- `SaveSnapshot / LoadSnapshot` 最小逻辑：已完成
- 恢复 `buckets / objects / object_index / chunk_ref_index`：已验证
- 恢复 `requests / request_fingerprints`：已验证
- 恢复 `tombstones`：已验证
- 恢复 `last_applied_index / last_applied_term`：已验证
- 恢复后 `Head/List` 与 `object_table / object_index / tombstone` 一致：已验证
- 恢复后重复 `request_id` 仍保持幂等：已验证
- 恢复后 deleted object 不复活：已验证
- 恢复后 committed object 的 `ChunkRef` 仍可查询：已验证
- 损坏或非法 snapshot 有明确错误：已验证
- `MetadataStateMachine` 不依赖 KV：保持成立
- 未修改 `RaftNode` 默认 wiring：保持成立
- 未删除 KV：保持成立
- 未实现 service / DataNode：保持成立
- 未进入 T021：保持成立
