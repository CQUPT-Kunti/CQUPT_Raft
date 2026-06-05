# T047 Read Fallback Quarantine

- 修改文件
  - `tests/storage_read_integration_test.cpp`
  - `tests/local_disk_chunk_store_test.cpp`
  - `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - `specs/007-storage-node-data-plane/task-reports/t047-read-fallback-quarantine.md`
  - `specs/007-storage-node-data-plane/tasks.md`

- 做了什么
  - 基于 T046 的 `ReadObjectByManifest(...)` helper，补了读取失败路径集成测试。
  - 新增首选副本 `not_found`、`timeout` 后 fallback 到备用副本成功的覆盖。
  - 强化首选副本 `checksum_mismatch` 场景：即使失败副本携带了伪造 payload，也不会被 helper 拼接到最终对象结果里。
  - 新增“已知坏副本事实”覆盖：当候选副本被标记为 `known_corrupted` 时，helper/selector 会在读取前跳过它，不把它当作有效读取源。
  - 扩展 `local_disk_chunk_store_test.cpp`：
    - 覆盖 `ChunkState::kQuarantined` 明确拒读
    - 覆盖坏文件读取后仍不自动把 index 状态改写成 quarantine，固定当前“只报错、不自动回写”的边界

- 副本失败 fallback / checksum mismatch quarantine 覆盖场景
  - 首选副本 `unavailable` 后 fallback 到备用副本成功
  - 首选副本 `not_found` 后 fallback 到备用副本成功
  - 首选副本 `timeout` 后 fallback 到备用副本成功
  - 首选副本 `checksum_mismatch` 后 fallback 到备用副本成功
  - `checksum_mismatch` 的失败副本不会把损坏 payload 拼接到最终对象结果
  - 所有副本失败时返回明确错误
  - 带 `known_corrupted` 事实的副本不会被选为有效读取源
  - `LocalDiskChunkStore` 对 `ChunkState::kQuarantined` 明确拒读
  - `LocalDiskChunkStore` 当前仍不会在 checksum mismatch / tampered file 后自动回写 quarantine；这次只把测试边界固定下来，没有实现真实后台 quarantine 动作

- 是否使用 `tests/test_file/test_file.deb`
  - 是

- 验证命令、PASS/FAIL、日志路径
  - `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - `ctest --test-dir build/linux -R "storage_read|local_disk_chunk_store" --output-on-failure 2>&1 | tee tmp/007/t047-read-fallback-quarantine.log`
  - PASS
  - 日志路径：`tmp/007/t047-read-fallback-quarantine.log`
  - 说明：按仓库当前真实测试名执行，实际命中 `local_disk_chunk_store`、`storage_read_integration`、`storage_read_chunk_contract`

- 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志摘要
  - 本次验证最终 PASS，无失败项

- Windows 验证判断
  - 当前无 Windows 编译/测试环境，不伪造 Windows PASS
  - 本任务未新增 `T047-WIN`

- 是否通过 T047
  - 是

- 是否可以进入 T048
  - 可以

- 当前任务发现的不合理点 / 警告 / 风险
  - 当前只把 quarantine / corrupted 的测试边界固定住了，没有实现真实 scrub、repair、后台 quarantine cleanup 或自动状态回写
  - `ReadReplicaCandidate` 当前只有 `known_corrupted` 这一类坏副本事实表达，没有单独的 runtime quarantine registry 通道；更细粒度节点事实仍要等后续 registry / heartbeat 任务
  - `LocalDiskChunkStore::ReadChunk()` 当前仍是“显式返回 checksum mismatch / corrupted，不自动回写 `CORRUPTED` / `QUARANTINED`”，这次没有也不应该强行扩成生产隔离动作
  - `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`
