# T039 US1 Validation

## 修改文件

- `specs/007-storage-node-data-plane/task-reports/t039-us1-validation.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 运行了哪些 US1 验证

- 上传闭环 / upload coordinator：
  - `storage_upload_integration`
  - `storage_upload_coordinator`
- StorageNode service / client / WriteChunk contract：
  - `storage_write_chunk_contract`
  - `storage_node_service`
  - `storage_node_client`
- Placement / PlacementManager：
  - `store_placement_policy`
  - `store_placement_manager`
- no-KV upload path audit：
  - `no_kv_surface_audit`

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_upload|upload_coordinator" --output-on-failure 2>&1 | tee tmp/007/t039-us1-upload.log`
  - PASS
  - 日志路径：`tmp/007/t039-us1-upload.log`
- `ctest --test-dir build/linux -R "storage_node_service|storage_node_client|write_chunk_contract" --output-on-failure 2>&1 | tee tmp/007/t039-us1-storage-node.log`
  - PASS
  - 日志路径：`tmp/007/t039-us1-storage-node.log`
- `ctest --test-dir build/linux -R "store_placement|placement_policy|placement_manager" --output-on-failure 2>&1 | tee tmp/007/t039-us1-placement.log`
  - PASS
  - 日志路径：`tmp/007/t039-us1-placement.log`
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit 2>&1 | tee tmp/007/t039-us1-no-kv-audit.log`
  - PASS
  - 日志路径：`tmp/007/t039-us1-no-kv-audit.log`

## Windows 验证判断

- 本任务是 Linux 当前环境下的 US1 验证
- 未新增 `T039-WIN`
- 当前环境没有 Windows 编译/测试能力，不伪造 Windows PASS

## 是否通过 T039

- 是

## 是否可以进入 T040

- 是

## 当前任务发现的不合理点 / 警告 / 风险

- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`
- 当前 US1 验证仍以 Linux 当前环境下的测试、placement 单测和静态 no-KV audit 为主，不替代 Windows 待验证项，也不替代更高层的多节点端到端验证

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
- 原因：将 T039 标记完成

## common-risk-notes.md 读取结果

- 已读取
- T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027 风险仍存在

## common-risk-notes.md 新增/删除/保留情况

- 新增：无
- 删除：无
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027
- 变更：无
