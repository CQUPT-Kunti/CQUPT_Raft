# T006 任务报告：008 测试 labels 与 guarded target placeholders

## 1. 修改了哪些文件

- `tests/CMakeLists.txt`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t006-008-test-label-placeholders.md`

## 2. 新增或预留了哪些 CTest labels

本次在 `tests/CMakeLists.txt` 中为 008 阶段预留了以下标签集合：

- `integrated-object-storage`
- `integrated-object-storage-e2e`
- `integrated-object-storage-quorum`
- `integrated-object-storage-recovery`
- `integrated-object-storage-concurrency`
- `view-node`
- `node-identity`
- `storage-transfer`
- `platform-neutral`
- `durability-boundary`
- `linux-primary-diagnosis`
- `windows-adaptation`

说明：

- 我优先沿用了仓库现有 label 风格，因此没有新造 `linux-primary`，而是继续使用现有的 `linux-primary-diagnosis`。
- `windows-adaptation` 作为 008 跨平台 identity / startup / config contract 的预留筛选标签，当前先挂在 `node_identity` 方向的 planned target 上。

## 3. 新增或预留了哪些 guarded test target entries

本次使用项目现有的 `add_raft_gtest(...)` helper 为以下 planned 测试文件预留了 guarded target entry：

- `test_integrated_object_storage_e2e` -> `integrated_object_storage_e2e_test.cpp`
- `test_integrated_object_storage_quorum` -> `integrated_object_storage_quorum_test.cpp`
- `test_integrated_object_storage_recovery` -> `integrated_object_storage_recovery_test.cpp`
- `test_integrated_object_storage_concurrency` -> `integrated_object_storage_concurrency_test.cpp`
- `test_view_node_discovery` -> `view_node_discovery_test.cpp`
- `test_node_identity` -> `node_identity_test.cpp`

guard 语义：

- 如果文件存在，则正常生成 gtest target 并注册 labels。
- 如果文件不存在，则沿用现有 helper 的 `Skip <target>: <file> not found` 行为，不会导致 configure/build 失败。

额外说明：

- 本次还预留了 `RAFT_008_LABELS_STORAGE_TRANSFER`，供后续 upload / download / placement-to-transfer 相关测试继续复用。
- 没有提前创建空测试文件，也没有提前接入 T026 / T050 / T080 的真实测试逻辑。

## 4. 是否保持已有测试 target、label、group 不变

- 是。
- 未修改已有测试 target 名称。
- 未删除已有 labels。
- 未改变现有 `platform-neutral`、`durability-boundary`、`linux-primary-diagnosis`、`storage-node-*` 等标签语义。
- 未改动 `./test.sh --group unit`、`persistence`、`all` 等现有入口行为。
- 008 planned 测试文件当前不存在，因此不会被强行加入现有必跑集合。

## 5. 是否发现不合理点 / 警告 / 风险

- `tests/CMakeLists.txt` 当前没有专门的“planned placeholder test helper”，但已有 `add_raft_gtest(...)` 已经自带 `if (NOT EXISTS ...) return()` guard，因此复用它是最小、最稳妥的做法。
- `storage placement / upload / transfer` 的未来测试文件名在 tasks.md 中还没有完全固定；本次先预留了统一 label 入口，没有擅自发明更多 target 名称。
- 现有 `FetchContent` 仍会在 configure 时产生一条 `CMP0135` / `DOWNLOAD_EXTRACT_TIMESTAMP` 的 dev warning，这不是本次变更引入的问题。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`。
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`。
- 原因：本次仅补充测试 label 和 guarded target placeholder，没有引入新的设计风险，也没有改变现有测试行为边界。

## 7. 验证命令和结果

### 验证命令

```bash
git diff -- tests/CMakeLists.txt specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t006-008-test-label-placeholders.md
cmake --preset debug-ninja-low-parallel
ctest --test-dir build/linux -N
```

### 验证结果

- `git diff` 范围符合预期，只包含 `tests/CMakeLists.txt`、`tasks.md` 的 T006 勾选状态和本任务报告。
- `cmake --preset debug-ninja-low-parallel` 实际执行成功。
  - configure 过程中按预期输出了：
    - `Skip test_integrated_object_storage_e2e: integrated_object_storage_e2e_test.cpp not found`
    - `Skip test_integrated_object_storage_quorum: integrated_object_storage_quorum_test.cpp not found`
    - `Skip test_integrated_object_storage_recovery: integrated_object_storage_recovery_test.cpp not found`
    - `Skip test_integrated_object_storage_concurrency: integrated_object_storage_concurrency_test.cpp not found`
    - `Skip test_view_node_discovery: view_node_discovery_test.cpp not found`
    - `Skip test_node_identity: node_identity_test.cpp not found`
  - 说明 008 planned 测试入口已经被安全占位，但不会因为文件缺失导致 configure 失败。
- `ctest --test-dir build/linux -N` 实际执行成功，当前列出了 220 个已有测试；由于 008 计划测试文件尚不存在，本次没有把新的 planned target 混入现有测试集合。
- configure 过程中仍有一条既有 `FetchContent` / `CMP0135` dev warning，来自 `tests/CMakeLists.txt` 现有逻辑，不是本次改动引入的问题。

## 结论

- T006 已完成。
- 从测试标签和 guarded placeholder 角度看，可以进入 T007。
