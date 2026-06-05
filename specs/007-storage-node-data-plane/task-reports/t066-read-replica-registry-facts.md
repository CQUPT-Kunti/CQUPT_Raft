# T066 Read Replica Registry Facts

- 修改文件
  - `modules/store/placement/replica_policy.h`
  - `modules/store/placement/replica_policy.cpp`
  - `modules/store/placement/module-notes.md`
  - `tests/support/storage_read_test_utils.h`
  - `tests/storage_read_integration_test.cpp`
  - `tests/store_placement_policy_test.cpp`
  - `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - `specs/007-storage-node-data-plane/task-reports/t066-read-replica-registry-facts.md`
  - `specs/007-storage-node-data-plane/tasks.md`

- 做了什么
  - 在 `ReplicaPolicySelector` 新增 read-side registry snapshot 入口：
    - `SelectReadReplicas(request, registry_snapshot, supplemental_candidates)`
  - 将生产 `StorageNodeRegistrySnapshotResult` 的 `liveness / health / disk_pressure / load / read_admission_overloaded` 映射成 `ReadReplicaCandidate`。
  - registry node facts 负责 node-level `health / disk_pressure / load / stale`，读路径补充事实继续承接 `known_corrupted / known_missing`。
  - 保留 T045 旧路径：没有 registry facts 时，仍按 manifest + 可选最小候选事实执行 fallback。
  - 扩展 `ReadObjectByManifest(...)` 测试 helper，使其可消费真实 registry snapshot。
  - 扩展 `store_placement_policy` / `storage_read_integration`，固定 registry-aware 的排序、排除和 fallback contract。

- registry facts -> read replica candidate 映射语义
  - `snapshot.node_id -> candidate.node_id`
  - `snapshot.facts.health.health -> candidate.health`
  - `snapshot.facts.health.disk_pressure -> candidate.disk_pressure`
  - `snapshot.facts.load.load -> candidate.load`
  - `snapshot.facts.load.read_admission_overloaded -> candidate.read_admission_overloaded`
  - `snapshot.liveness != kLive -> candidate.stale = true`
  - registry 中缺失的 manifest replica 不会被硬删除，仍作为 unknown fallback 保留 manifest 顺序兼容语义

- health / liveness / load / disk pressure / corruption eligibility 语义
  - `known_corrupted` / `known_missing`：直接跳过
  - `stale`、`read_admission_overloaded`：直接跳过
  - `health = Unavailable / Draining`：直接跳过
  - `health = Healthy / ReadOnly / Degraded`：仍可参与排序，但 `Healthy` 优先
  - `disk_pressure = High / Full`：不作为首选，按排序降权
  - `active_reads` / `TotalInflight()` 更低者优先
  - registry snapshot 不可用时返回对应错误，不伪装成“无 facts 的成功路径”

- read fallback 与 registry facts 当前边界
  - committed metadata gate、逐副本 fallback、失败分类仍保持 T045/T047 既有语义
  - T066 只改变“先尝试谁”的顺序和排除逻辑，不实现 repair、scrub、failure cache 写入或 corruption 自动回写
  - unknown registry facts 当前按中性 fallback 处理，不当场发明更激进的拒绝策略

- 是否调用 metadata / Raft；是否保存 payload
  - 不调用 Raft
  - 不写 metadata
  - 不保存 payload 到 metadata / Raft
  - 读取测试 helper 仍先经过 metadata committed gate，再进入 data-plane

- 是否使用 tests/test_file/test_file.zip
  - 否
  - 继续沿用现有 `tests/test_file/test_file.deb`

- 验证命令、PASS/FAIL、日志路径
  - `mkdir -p tmp/007`
    - PASS
  - `cmake --build --preset debug-ninja-low-parallel`
    - PASS
  - `ctest --test-dir build/linux -R "storage_read|store_placement_policy|storage_heartbeat_registry|storage_node_registry" --output-on-failure 2>&1 | tee tmp/007/t066-read-replica-registry-facts.log`
    - PASS
    - 实际匹配到的测试名为 `storage_read_integration`、`storage_read_chunk_contract`、`store_placement_policy`、`storage_heartbeat_registry`
    - 日志路径：`tmp/007/t066-read-replica-registry-facts.log`

- 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志
  - 本次验证未失败

- Windows 验证判断
  - T066 是平台无关的 registry facts -> read replica selection 接线任务
  - 当前无 Windows 编译/测试环境，不伪造 Windows PASS
  - 本任务未新增 `T066-WIN`

- 是否通过 T066
  - 是

- 是否可以进入 T067
  - 可以
  - 前提：T067 只做 US4 验证，不把 T066 扩展成 repair / scrub / corruption 自动治理

- 当前任务发现的不合理点 / 警告 / 风险
  - read-side 仍没有 failure cache、recent failure scoring 或 corruption 自动状态回写
  - registry freshness 仍依赖调用方传入的 `observed_at/now`
  - `disk_pressure = High / Full` 当前采用降权而非硬拒绝；这是本次固定下来的 read-side contract

- 是否更新 module-notes.md / AGENTS.md
  - 更新了 `modules/store/placement/module-notes.md`
  - 未更新 `AGENTS.md`

- module-notes.md 是否补充 .cpp 关键函数 / helper
  - 是
  - 已补充：
    - registry snapshot 转 read replica candidate helper
    - manifest replica + registry facts merge helper
    - liveness / health / overload / disk pressure 映射 helper
    - read replica ordering helper
    - read fallback 与 registry facts 的边界说明

- 是否修改高频文档及原因
  - 修改了 `specs/007-storage-node-data-plane/tasks.md`
    - 原因：将 T066 标记完成，并记录真实影响文件与验收语义
  - 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
    - 原因：收缩“read replica selection 未接生产 registry facts”风险，并保留 freshness / failure cache / corruption 治理未完成风险

- common-risk-notes.md 读取结果
  - 已读取并维护
  - T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027、T045、T049、T055、T056、T057、T059、T060、T061、T062、T063、T064、T065 风险继续保留或收缩

- common-risk-notes.md 新增/删除/保留情况
  - 新增：`T066`，记录 registry-aware read selection 已完成，但 failure cache、corruption 自动沉淀、snapshot freshness 独立时钟源仍未收口
  - 删除：无整项删除
  - 收缩：
    - `T045` 从“尚未接真实 registry”收缩为“已接 registry，但 failure cache / corruption 自动回写未完成”
    - `T059`、`T060`、`T061`、`T062`、`T065` 从“read-side 未接线”收缩为“read-side 已接线，但 freshness / failure-domain / scoring 等后续语义未完备”
