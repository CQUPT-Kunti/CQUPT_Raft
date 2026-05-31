# T048 US2 Read Validation

## 修改文件

- `specs/007-storage-node-data-plane/task-reports/t048-us2-read-validation.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 运行了哪些 US2 验证

- 构建验证：
  - `cmake --build --preset debug-ninja-low-parallel`
- 读取路径验证：
  - `storage_read_integration`
  - `storage_read_chunk_contract`
  - `storage_node_service`
  - `storage_node_client`
  - `local_disk_chunk_store`
  - `store_placement_policy`
  - `store_placement_manager`
- 说明：
  - 按 `tests/CMakeLists.txt` 当前真实测试名执行。
  - placement 相关建议命令中的 `store_placement|placement_policy|placement_manager` 在当前仓库里实际收敛为 `store_placement_policy|store_placement_manager`。

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - 日志路径：未单独保存（按任务要求仅执行并报告结果）
- `ctest --test-dir build/linux -R "storage_read|storage_read_chunk_contract" --output-on-failure 2>&1 | tee tmp/007/t048-us2-read.log`
  - PASS
  - 日志路径：`tmp/007/t048-us2-read.log`
- `ctest --test-dir build/linux -R "storage_node_service|storage_node_client" --output-on-failure 2>&1 | tee tmp/007/t048-us2-storage-node-read.log`
  - PASS
  - 日志路径：`tmp/007/t048-us2-storage-node-read.log`
- `ctest --test-dir build/linux -R "local_disk_chunk_store" --output-on-failure 2>&1 | tee tmp/007/t048-us2-local-store.log`
  - PASS
  - 日志路径：`tmp/007/t048-us2-local-store.log`
- `ctest --test-dir build/linux -R "store_placement_policy|store_placement_manager" --output-on-failure 2>&1 | tee tmp/007/t048-us2-read-placement.log`
  - PASS
  - 日志路径：`tmp/007/t048-us2-read-placement.log`

## Windows 验证判断

- T048 仅在当前 Linux 环境执行 US2 读取路径验证。
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS。
- 不新增 `T048-WIN`；既有 Windows 待验证风险继续保留。

## 是否通过 T048

- 是

## 是否可以进入 T049

- 可以。
- 当前 US2 读取路径相关构建与定向验证均通过，未发现阻塞 T049 的新增失败。

## 当前任务发现的不合理点 / 警告 / 风险

- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 当前仍返回 `specs/006-remove-kv-metadata-state-machine`，与本次 007/T048 执行目标不一致。
- 本次验证只覆盖 Linux 当前环境，不关闭 Windows 实机验证相关风险。
- 本次验证没有也不应关闭已有读取路径剩余风险：registry / heartbeat / failure cache 未接入、timeout/cancellation 运行中传播未实现、corrupted 自动状态回写未实现、restart rebuild / staging cleanup 未实现。

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：将 T048 标记完成。
- 新增 `specs/007-storage-node-data-plane/task-reports/t048-us2-read-validation.md`
  - 原因：记录本次 US2 读取路径验证范围、命令、结果和风险判断。
- 未修改 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：本次验证未解决既有风险，也未暴露新的 US2 风险。

## common-risk-notes.md 读取结果

- 已读取。
- 现有风险项仍包括：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027、T045。
- 已确认以下风险在 T048 中不应误删：
  - corrupted 自动状态回写未实现
  - registry / heartbeat / failure cache 未接入
  - timeout / cancellation 运行中传播未实现
  - Windows 待验证
  - restart rebuild / staging cleanup 未实现

## common-risk-notes.md 新增/删除/保留情况

- 新增：无
- 删除：无
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027、T045
- 变更：无
