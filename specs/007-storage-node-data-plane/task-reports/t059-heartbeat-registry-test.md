# T059 Heartbeat Registry Test

## 修改文件

- `tests/storage_heartbeat_registry_test.cpp`
- `tests/CMakeLists.txt`
- `specs/007-storage-node-data-plane/task-reports/t059-heartbeat-registry-test.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `tests/storage_heartbeat_registry_test.cpp`
- 在测试内实现 test-only `TestStorageNodeHeartbeatRegistry`
- 复用现有 `storedemo::StorageNodePlacementCandidate`、`StorageNodeHealth`、`StorageNodeDiskPressure`、`StorageNodeLoadSnapshot` 作为 data-plane facts 容器
- 固定 heartbeat / registry 的最小 contract：
  - 注册新节点
  - 同 endpoint 重复注册幂等
  - 不同 endpoint 重复注册返回 conflict
  - heartbeat 用 sequence 更新节点事实
  - stale heartbeat 不覆盖更新后的事实
  - 同 sequence heartbeat 幂等
  - liveness 由 `last_seen + timeout` 推导
  - list / lookup 稳定可读

## heartbeat / registry contract 覆盖场景

- register 新节点成功并保存 endpoint / capacity / chunk_count / io_error_count
- duplicate register 同 endpoint 幂等，不产生重复记录
- duplicate register 不同 endpoint 返回明确 conflict
- heartbeat 更新 capacity / used / available / health / disk pressure / load / chunk_count / io_error_count
- stale heartbeat（更旧 sequence）不覆盖 newer facts
- 同 sequence heartbeat 幂等
- fresh node 为 live，超过 timeout 后转 stale
- invalid node_id / invalid endpoint / invalid capacity facts 返回 `kInvalidArgument`
- unknown node heartbeat 返回 `kNotFound`
- registry list 按 `node_id` 稳定排序

## test-only adapter 与生产 registry 当前边界

- 当前只新增 test-only adapter，不实现生产 `StorageNodeRegistry`
- 当前不新增 proto，不实现 service/client heartbeat/report/register RPC
- 当前不接入 `PlacementManager` eligibility，也不接入 read replica selection
- 当前不调用 metadata / Raft，不保存 object payload，不决定 object committed/deleted 可见性

## 是否使用 tests/test_file/test_file.zip

- 否

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - 日志路径：未单独保存 build 日志
- `ctest --test-dir build/linux -R "storage_heartbeat_registry|heartbeat_registry" --output-on-failure 2>&1 | tee tmp/007/t059-heartbeat-registry.log`
  - PASS
  - 日志路径：`tmp/007/t059-heartbeat-registry.log`
  - 说明：实际匹配到的测试名为 `storage_heartbeat_registry`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志摘要

- 本次验证未失败

## Windows 验证判断

- T059 是平台无关 heartbeat/registry contract 测试
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本任务未新增 `T059-WIN`

## 是否通过 T059

- 是

## 是否可以进入 T060

- 可以

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 heartbeat / registry 只在 test-only adapter 中固定 contract，不代表生产 registry 已完成
- clock / sequence / liveness 语义还需要在后续 T061-T066 的真实 proto、service、client、registry 接线中统一收口

## 是否更新 module-notes.md / AGENTS.md

- 否

## module-notes.md 是否需要补充 .cpp 关键函数 / helper

- 否
- 本任务只新增测试，没有修改生产 `.cpp`

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T059 完成
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：补充 T059 固定的是 test-only heartbeat/registry contract，生产接线与 clock/sequence 新鲜度风险仍待后续任务收口

## common-risk-notes.md 读取结果

- 已读取并核对现有风险项
- 原有 Windows 删除语义、timeout/cancellation、corruption 自动回写、registry/failure cache、GC schema migration、多进程 persistence root 等风险继续保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：`T059` test-only heartbeat/registry contract 与后续生产接线、clock/sequence 新鲜度风险
- 删除：无
- 保留：其余现有风险全部保留
