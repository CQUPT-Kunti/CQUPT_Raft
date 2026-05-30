# T038 Upload No-KV Regression

## 修改文件

- `tests/no_kv_surface_audit.cmake`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 扩展 `no_kv_surface_audit`，显式把当前 007 上传链路相关文件纳入 upload-path focused audit
- 为上传路径相关文件增加更明确的旧 KV surface 禁用检查：
  - `SetCommand(`
  - `DeleteCommand(`
  - `DebugGetValue`
  - `KvStateMachine`
  - `KvService`
  - `raft_kv_client`
- 保持 audit 只禁止旧 KV demo surface，不误伤 metadata-only control-plane 的正常 metadata 提交流程

## no-KV 上传路径覆盖范围

- `tests/storage_upload_integration_test.cpp`
- `tests/storage_upload_coordinator_test.cpp`
- `tests/storage_write_chunk_contract_test.cpp`
- `tests/storage_node_service_test.cpp`
- `tests/storage_node_client_test.cpp`
- `tests/support/storage_upload_test_utils.h`
- 以及现有 `modules/store/*`、`proto/storage_node.proto`、`tests/storage*_test.cpp` / `tests/support/storage_*` 的通用 no-KV 审计范围

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit 2>&1 | tee tmp/007/t038-upload-no-kv-audit.log`
  - PASS
  - 日志路径：`tmp/007/t038-upload-no-kv-audit.log`
- `ctest --test-dir build/linux -R "upload_coordinator|storage_upload|write_chunk_contract|storage_node_service|storage_node_client" --output-on-failure 2>&1 | tee tmp/007/t038-upload-test.log`
  - PASS
  - 日志路径：`tmp/007/t038-upload-test.log`

## Windows 验证判断

- 本任务是 no-KV 审计和上传链路回归，不涉及 Windows 专属逻辑
- 未新增 `T038-WIN`
- 当前环境没有 Windows 编译/测试能力，不伪造 Windows PASS

## 是否通过 T038

- 是

## 是否可以进入 T039

- 是

## 当前任务发现的不合理点 / 警告 / 风险

- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`
- 当前 no-KV audit 是静态 surface 审计，不能替代更高层的语义级回归或多节点端到端验证

## 是否更新 module-notes.md / AGENTS.md

- 未更新

## module-notes.md 是否需要补充 .cpp 关键函数 / helper

- 不需要
- 本任务未修改 `modules/store/*` 生产代码

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
- 原因：将 T038 标记完成

## common-risk-notes.md 读取结果

- 已读取
- T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027 风险仍存在

## common-risk-notes.md 新增/删除/保留情况

- 新增：无
- 删除：无
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027
- 变更：无
