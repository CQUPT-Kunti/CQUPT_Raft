# T045 Read Replica Selection

- 修改文件
  - `modules/store/placement/replica_policy.h`
  - `modules/store/placement/replica_policy.cpp`
  - `modules/store/placement/module-notes.md`
  - `modules/store/node/storage_node_client.h`
  - `modules/store/node/storage_node_client.cpp`
  - `modules/store/node/module-notes.md`
  - `tests/store_placement_policy_test.cpp`
  - `tests/storage_read_integration_test.cpp`
  - `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - `specs/007-storage-node-data-plane/task-reports/t045-read-replica-selection.md`
  - `specs/007-storage-node-data-plane/tasks.md`

- 做了什么
  - 在 `ReplicaPolicySelector` 上新增 `SelectReadReplicas(...)`，把 committed manifest 的 `replica_nodes` 收口成稳定、可测试的读副本有序列表。
  - 读选择当前会跳过 `corrupted / unavailable / stale / overloaded`，也会跳过显式排除、空 `node_id`、manifest 重复节点和已知 missing 副本。
  - 在 `storage_node_client` 新增最小 committed-manifest 读 helper：manifest facts 到 `ReadChunkRequest` 的转换、单副本失败分类、逐副本 fallback 聚合。
  - 扩展 `storage_read_integration_test`，通过 metadata committed manifest + fake replica reader 固定 committed-only gate、fallback 顺序、错误聚合和 offset 拼接语义。
  - 扩展 `store_placement_policy_test`，固定读副本选择的筛除和排序 contract。

- read replica selection / fallback 输入、输出、成功/失败语义
  - 输入：
    - committed manifest `chunk_id`
    - committed manifest `replica_nodes`
    - 可选读副本事实：`health / stale / overloaded / known_corrupted / known_missing / load`
    - 上层已确认 committed 的 metadata object
  - 输出：
    - 有序可读副本列表
    - 被排除副本及原因
    - fallback 结果：成功响应、选中的副本节点、每次尝试记录
  - 成功语义：
    - 只在 metadata `HeadObject` 显示 committed 且 manifest 存在时进入 data-plane
    - 成功读取后 payload 按 manifest `offset` 顺序拼接
    - 每个 chunk 成功返回的 `payload / size / checksum` 必须与 committed manifest 一致
  - 失败语义：
    - `replica_nodes` 为空返回 `kInvalidArgument`，不读 data-plane
    - `pending / deleted` object 返回 metadata 映射错误，不读 data-plane
    - `timeout / node_unavailable / overloaded / io_error / not_found / conflict / checksum_mismatch / corrupted` 会尝试下一个副本
    - `invalid_argument / cancelled / unsupported / permission_denied` 立即停止 fallback
    - 所有可继续副本都失败时，优先聚合完整性错误（`checksum_mismatch / corrupted`），否则返回明确的最后主导失败

- 是否使用 `tests/test_file/test_file.deb`
  - 是。新增和已有的 `storage_read_integration_test` 场景都继续使用 `tests/test_file/test_file.deb` 的真实二进制 payload。

- 验证命令、PASS/FAIL、日志路径
  - `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - `ctest --test-dir build/linux -R "store_placement_policy|storage_read" --output-on-failure 2>&1 | tee tmp/007/t045-read-replica-selection.log`
  - PASS
  - 日志路径：`tmp/007/t045-read-replica-selection.log`
  - 说明：按仓库当前真实测试名执行，`tasks.md` 里旧的 `tests/storage_placement_test.cpp` 路径并不存在，实际使用 `tests/store_placement_policy_test.cpp` 和 `tests/storage_read_integration_test.cpp`

- 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志摘要
  - 本次验证最终 PASS，无失败项。

- Windows 验证判断
  - 当前未新增 Windows 专属文件行为。
  - 当前无 Windows 编译/测试环境，不宣称 Windows PASS。
  - 本任务不新增 `T045-WIN`。

- 是否通过 T045
  - 是。

- 是否可以进入 T046
  - 可以。T045 已补最小生产 helper 和集成验证，T046 可以继续把测试侧 committed-manifest 读 helper 抽到 `tests/support/` 共享位置。

- 当前任务发现的不合理点 / 警告 / 风险
  - `tasks.md` 中 T045 指向的 `tests/storage_placement_test.cpp` 为旧路径，仓库实际使用的是 `tests/store_placement_policy_test.cpp`。
  - 当前 read selection 仍主要依赖 manifest 顺序和调用方传入的最小副本事实，尚未接入真实 heartbeat / registry / failure cache。
  - 当前 fallback 只做读路径错误扩散，不做 repair、scrub、坏块自动回写或 replica health 持久化。
  - `best_effort_cancel` / 运行中 timeout cancellation 边界没有因为 T045 收紧，仍保持 T019/T020 已记录边界。

- 是否更新 `module-notes.md` / `AGENTS.md`
  - 更新了 `modules/store/placement/module-notes.md`
  - 更新了 `modules/store/node/module-notes.md`
  - 未更新 `AGENTS.md`

- `module-notes.md` 是否补充 `.cpp` 关键函数 / helper
  - 是。
  - `modules/store/placement/module-notes.md` 已补 `SelectReadReplicas(...)`、读副本候选结构、筛除和排序边界。
  - `modules/store/node/module-notes.md` 已补 manifest 到 read request helper、失败分类 helper、fallback loop helper。

- 是否修改高频文档及原因
  - 修改了 `specs/007-storage-node-data-plane/tasks.md`
    - 原因：标记 T045 完成，并把旧测试路径修正为当前仓库真实落点。
  - 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
    - 原因：关闭“尚未实现 read replica selection / fallback”的旧风险，保留 T045 之后仍存在的 registry / corruption-healing / cancellation 边界。

- `common-risk-notes.md` 读取结果
  - 已读取并保留以下仍未关闭风险：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027。
  - 重点确认：T024 corrupted 不自动回写、T019/T020 timeout/cancellation、Windows 待验证、restart rebuild/staging cleanup 风险均未误删。

- `common-risk-notes.md` 新增/删除/保留情况
  - 删除/替换了旧的 `T044` 风险条目，因为 “read replica selection / fallback 未实现” 已由 T045 解决。
  - 新增了 `T045` 风险条目，记录当前只实现最小 committed-manifest fallback，尚未接 registry facts、repair/scrub 和 corruption 自动沉淀。
  - 其余历史风险保持不变。
