# T039 Metadata Snapshot V2 Format

## 变更摘要
- 升级 `modules/raft/state_machine/metadata_state_machine.cpp` 的 `MetadataStateMachine` snapshot 数据文件头为显式 V2：
  - `magic = "MDS2"`
  - `version = 2`
  - `last_applied_index`
  - `last_applied_term`
  - `bucket_count`
  - `object_count`
  - `object_index_count`
  - `chunk_ref_index_count`
  - `request_count`
  - `request_fingerprint_count`
  - `tombstone_count`
- `SaveSnapshot()` / `LoadSnapshot()` 继续只作用于状态机数据文件，不触碰 `RaftNode` replay、`SnapshotStorage` publish/fsync/checksum 逻辑。

## V2 格式覆盖字段
- `buckets`
- `objects`
- `object_index`
- `chunk_ref_index`
- `request_table`
- `request_fingerprints`
- `tombstones`
- `last_applied_index`
- `last_applied_term`

## SaveSnapshot / LoadSnapshot 处理方式
- `SaveSnapshot()` 先在读锁下复制临时快照视图，再按 V2 header + 各表内容写入临时文件，最后 rename 覆盖目标文件。
- `LoadSnapshot()` 先把 header 和各表加载到临时容器，再做一致性校验；只有校验全部通过，才替换内存状态。
- 明确失败场景：
  - 未知/错误 magic
  - 不支持的 version
  - header 字段缺失
  - body 截断
  - 重复 key
  - 表间索引不一致

## 一致性校验覆盖
- `request_table`：
  - `request_id` 非空且与 key 一致
  - 每条 request 都有非空 fingerprint
  - request/fingerprint 数量一致
  - `applied_index` 在 `last_applied_index` 边界内
- `object_table / object_index`：
  - object identity 必须与 record 匹配
  - live object 必须存在唯一 index entry
  - deleted object 不得留在 `object_index`
  - deleted bucket 不得仍含 active object
- `chunk_ref_index`：
  - 只能指向 committed object
  - 内容必须与 object 自身 `chunks` 一致
- `tombstones`：
  - 只能指向 deleted object
  - `deleted_at_log_index` 不能越过 `last_applied_index`
- 恢复语义：
  - committed object 的 `ChunkRef` 可恢复
  - deleted object 不复活
  - request_id 幂等事实恢复后仍有效

## 测试更新
- 扩展 `tests/metadata_state_machine_snapshot_test.cpp`：
  - 校验 V2 header magic/version/counts
  - 校验未知 version 明确失败且不污染旧内存状态
  - 校验截断 snapshot 明确失败且不污染旧内存状态
  - 校验 object index 不一致明确失败

## Linux 验证
- `cmake --preset debug-ninja-low-parallel`
- `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine test_metadata_snapshot`
- `ctest --test-dir build/linux --output-on-failure -R "(MetadataStateMachineTest|MetadataSnapshotTest)"`
- 结果：PASS
- 日志：`tmp/test-logs/t039-metadata-snapshot-v2.log`

## 剩余风险
- 本任务只覆盖状态机数据文件；`snapshot.meta` 与状态机 V2 header 的外层边界联动仍留给 T040/T041。
- 当前 `StrongConsistencyMetadataStateMachine` 仍保留旧 `MDS1`/V1 格式，属于并存过渡逻辑，不在本任务清理范围。
