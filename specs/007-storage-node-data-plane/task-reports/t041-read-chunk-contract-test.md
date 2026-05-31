# T041 ReadChunk Contract Test

## 修改文件

- `tests/storage_read_chunk_contract_test.cpp`
- `tests/CMakeLists.txt`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/task-reports/t041-read-chunk-contract-test.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `storage_read_chunk_contract` 独立 contract 测试文件，不提前实现生产 `ReadChunk` proto / service / client。
- 在测试内使用 `LocalDiskChunkStore` 作为真实底层 chunk store，固定 full read、range boundary、checksum verify、live-only 读取和 metadata 可见性边界。
- 新增 test-only `ReadChunkContractAdapter` 和 `ReadChunkClientContractAdapter`，在没有生产 `ReadChunk` RPC 的前提下固定未来 `StorageNodeService::ReadChunk` / `StorageNodeClient::ReadChunk` 的 contract。
- 在 `tests/CMakeLists.txt` 注册 `test_storage_read_chunk_contract` / `storage_read_chunk_contract`。
- 将 `tasks.md` 的 T041 标记完成，并把实际测试文件路径修正为 `tests/storage_read_chunk_contract_test.cpp`。
- 在 `common-risk-notes.md` 新增 T041 风险说明：当前只固定 contract，真实 proto/service/client/read replica selection 仍待 T042-T045。

## ReadChunk contract 覆盖场景

- full read 成功：
  - 使用真实 fixture 二进制 payload
  - 断言 payload / checksum / size / `verified` / live state 一致
- range read 当前边界：
  - 对已存在 live chunk 发起 range read
  - 当前阶段必须返回 `unsupported` 或 `invalid_argument`
  - 不允许 silent partial success
- checksum verify：
  - `expected_checksum` 不匹配时返回 `checksum_mismatch`
- 非 live 状态拒绝：
  - `quarantined` -> `corrupted`
  - `corrupted` -> `corrupted`
  - `deleted` -> `not_found`
  - `staging` -> `conflict`
- 只读取 live chunk：
  - live final 文件缺失时，即使 staging 文件存在，也返回 `not_found`
  - 不回退读取 staging
- missing chunk：
  - index 中不存在时返回 `not_found`
- transport / client mapping：
  - `not_found`
  - `invalid_argument`
  - `timeout`
  - `unavailable`
  - `io_error`
  通过 fake adapter 固定映射边界
- 不决定 object committed 可见性：
  - metadata 仅 `CreateObject`
  - chunk 已 durable 可直接按 chunk id 读回
  - `HeadObject` 仍保持 not visible，不因 `ReadChunk` 成功而变成 committed 可见

## 是否使用 tests/test_file/test_file.deb

- 是

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --preset debug-ninja-low-parallel`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_read_chunk_contract|storage_node_service|storage_read" --output-on-failure 2>&1 | tee tmp/007/t041-read-chunk-contract.log`
  - PASS
  - 日志路径：`tmp/007/t041-read-chunk-contract.log`

## Windows 验证判断

- T041 只新增平台无关 contract 测试和 Linux 当前环境下的真实本地文件读取验证。
- 当前没有 Windows 编译/测试环境，不伪造 Windows PASS。
- 本任务未引入新的 Windows 专属行为要求，不新增 `T041-WIN`。

## 是否通过 T041

- 是

## 是否可以进入 T042

- 是

## 当前任务发现的不合理点 / 警告 / 风险

- `ReadChunk` 目前仍只有 contract test，没有真实 `proto/storage_node.proto` 字段、`StorageNodeService::ReadChunk`、`StorageNodeClient::ReadChunk` 或 read replica selection。
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`，与本次 007 任务不一致。
- T024 的 corrupted 状态不自动回写、T019/T020 的 timeout/cancellation 边界、Windows 待验证、restart rebuild / staging cleanup 风险仍存在，本任务未关闭。

## 是否更新 module-notes.md / AGENTS.md

- 未更新

## module-notes.md 是否补充了 .cpp 关键函数 / helper

- 不需要
- 本任务未修改 `modules/store/*` 生产代码

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：将 T041 标记完成，并修正实际测试文件路径
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：补充 T041 当前仅固定 contract、尚未落地真实 read RPC 的风险说明

## common-risk-notes.md 读取结果

- 已读取
- T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027 风险仍存在

## common-risk-notes.md 新增/删除/保留情况

- 新增：T041
- 删除：无
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027
- 变更：无
