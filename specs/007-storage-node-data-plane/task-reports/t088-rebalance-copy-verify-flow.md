# T088 Rebalance Copy Verify Flow

## 修改文件

- `modules/store/maintenance/rebalance_manager.h`
- `modules/store/maintenance/rebalance_manager.cpp`
- `modules/store/maintenance/module-notes.md`
- `tests/storage_rebalance_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t088-rebalance-copy-verify-flow.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在生产 `RebalanceManager` 中新增 `RunTask()`，把 rebalance 执行收口为注入式阶段流程：source read、target durable write、target verify、manifest coordination callback、source cleanup callback。
- 为 `RebalanceManager` 补齐 source/target/verify/manifest/source-cleanup/cleanup-candidate 的结果类型和 callback 配置。
- 为 task 增加阶段事实：`source_payload_verified`、`target_durable`、`target_verified`、`manifest_coordinated`、`source_cleanup_completed`、`orphan_candidate_recorded` 等，用于 retry 和 repeated rebalance 跳阶段。
- 调整 `CompleteTask()` 语义：只有 copy / verify / manifest / cleanup 阶段全部满足后才允许 completed。
- 扩展 `storage_rebalance` 测试覆盖 happy path、target durable 失败、verify 失败、manifest 失败 cleanup candidate、source cleanup retry、already_exists 幂等、already_exists conflict、source checksum mismatch 和 repeated rebalance 幂等。

## RebalanceManager copy / verify / manifest coordination 占位流程语义

- 当前生产流程固定为：
  - source read
  - target durable write
  - target verify
  - manifest coordination callback
  - source cleanup callback
- 任一前置阶段失败，后续阶段不会执行。
- 阶段执行都通过注入 callbacks 完成，当前不直接接 metadata / Raft。

## target durable before manifest coordination 当前边界

- `target_durable == true` 且 `target_verified == true` 后，才允许进入 manifest coordination callback。
- target durable 失败时，不执行 verify，不执行 manifest coordination，不清理 source。
- verify 失败时，不执行 manifest coordination，不清理 source。

## source cleanup after manifest coordination 当前边界

- 只有 manifest coordination callback 成功后，才允许 source cleanup。
- source cleanup 失败时，task 进入 `RetryPending` 或明确失败状态，不伪装 completed。
- cleanup retry 时会复用 task 内阶段事实，跳过已经成功的 target durable / verify / manifest 阶段。

## cleanup candidate / orphan candidate 当前边界

- manifest coordination 失败时，source 不清理。
- 同时会通过 cleanup candidate / orphan candidate recorder 把 target durable chunk 记为待后续处理。
- 当前只记录 candidate 事实，不做 candidate persistence，也不做 source cleanup crash recovery。

## 与真实 metadata/Raft manifest update 的边界

- manifest coordination 当前只是 callback 占位。
- 不调用 `RaftNode::ProposeMetadata()`。
- 不直接修改 `MetadataStateMachine`。
- 不决定 object committed/deleted 可见性。

## 是否调用 metadata / Raft；是否保存 payload

- 不调用 metadata / Raft。
- 不保存 object payload 到 metadata / Raft。

## 是否使用 tests/test_file/test_file.zip

- 未使用。

## 验证命令、PASS/FAIL、日志路径

- `cmake --build --preset debug-ninja-low-parallel`
- `ctest --test-dir build/linux -R "storage_rebalance|rebalance_manager|storage_scrub_repair" --output-on-failure 2>&1 | tee tmp/007/t088-rebalance-copy-verify-flow.log`
- 实际命中测试：`storage_scrub_repair`、`storage_rebalance`
- 结果：PASS
- 日志：`tmp/007/t088-rebalance-copy-verify-flow.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证为 PASS，无失败项。

## Windows 验证判断

- T088 涉及 target durable write 和 source cleanup 文件语义。
- 当前无 Windows 编译环境，本任务不宣称 Windows PASS。
- Windows 文件 copy / durable write / delete 语义风险继续保留到后续实机验证任务。

## 是否通过 T088

- 是。

## 是否可以进入 T089

- 可以。

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 manifest coordination 只是注入式 callback，不是真实 metadata manifest update / Raft 提交。
- cleanup candidate / orphan candidate 只有进程内事实，没有持久化协议。
- source cleanup retry 依赖内存 task state；进程崩溃后的恢复仍待后续任务处理。

## 是否更新 module-notes.md / AGENTS.md

- 已更新 `modules/store/maintenance/module-notes.md`。
- 未更新 `AGENTS.md`。

## 是否修改高频文档及原因

- 已修改 `specs/007-storage-node-data-plane/tasks.md`，把 T088 标记为完成并记录实际修改与验收。
- 已修改 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`，保留并补充 T088 后续风险。

## common-risk-notes.md 读取结果

- 已读取并保留 T080/T085/T087 及更早任务的未关闭风险。

## common-risk-notes.md 新增/删除/保留情况

- 新增：T088 已实现最小 copy/verify/coordination 占位流程，但真实 metadata manifest update / Raft 提交、cleanup candidate persistence、source cleanup crash recovery、rebalance task persistence 和自动调度仍未实现。
- 删除：无。
- 保留：真实 metadata/Raft manifest update 未实现、RebalanceManager 自动后台调度未实现、RebalanceManager 持久化未实现、source cleanup crash recovery 风险、repair/rebalance 后 manifest 更新一致性风险、Windows 文件语义待验证、read-side repair 未实现、metadata / registry facts 新鲜度风险。
