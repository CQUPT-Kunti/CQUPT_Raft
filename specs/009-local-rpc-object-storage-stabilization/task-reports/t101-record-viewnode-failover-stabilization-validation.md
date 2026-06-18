# T101 Record ViewNode Failover Stabilization Validation

## 1. 本任务做了什么

- 收口整理 T099 与 T100 的最终验证结论。
- 复用已有 Linux 验证证据，记录当前 ViewNode failover、peer sync、availability、discovery、status 的实际行为边界。
- 同步修正文档状态，使 `tasks.md` 与 `validation-matrix.md` 反映当前 T101 语义。

## 2. 写入的报告文件

- `specs/009-local-rpc-object-storage-stabilization/task-reports/t101-record-viewnode-failover-stabilization-validation.md`

## 3. T099 修复了什么

- T099 修复的是 `examples/object-storage-local-009-dynamic/rpc_demo.sh failover-view` 的错误判定，不是 `modules/view/*` 里的生产状态聚合逻辑。
- 已确认不存在以下错误服务端传播链：
  - `peer sync failure -> available=false`
  - `peer unreachable -> node unavailable`
  - `cluster degraded -> node unavailable`
- 真正问题是脚本把 surviving ViewNode 的“ready”条件写得过严：
  - 先前错误要求固定 `metadata_nodes=3` 和 `storage_nodes=6`
  - 后续又把 partial registry 下的 `storage_nodes=0` 误判成 unavailable

## 4. T100 验证了什么

- T100 新增 `tests/view_failover_test.cpp` 并注册 `view_failover_test` 目标。
- 新增回归用例：
  - `ViewFailoverTest.SurvivingViewNodeRemainsAvailableWhenFailoverLeavesPartialRegistry`
  - `ViewFailoverTest.SurvivingViewNodeCanStayDegradedWithoutBecomingUnavailable`
- 覆盖结论：
  - surviving ViewNode 在 failover 后仍可返回 `GetClusterView()`
  - surviving ViewNode 在 peer sync 故障持续存在时仍可继续提供 discovery/status 服务
  - `degraded` / `partial` 不会被误传播为 `unavailable`
  - partial storage registry 返回 `kNotFound` 是允许语义，不等价于 `ServiceUnavailable`

## 5. 当前 surviving ViewNode 的正确行为

- 当 `view-1 down` 且 `view-2 alive` 时：
  - `view-2` 应保持 available
  - `view-2` 的 `liveness` 应保持 `live`
  - `view-2` 的 `health` 可为 `healthy`、`degraded` 或其他非 `unavailable` 状态
  - `GetClusterView()` 应继续返回正常状态
  - `DiscoverMetadata()` 应继续返回存活 metadata 观测
  - `DiscoverStorage()` 在 partial registry 场景下可返回空结果或 `kNotFound`，但不应把 self 视作 unavailable

## 6. 当前 peer sync failure 的正确行为

- `peer sync connection refused`
- `peer sync backoff`
- `peer sync pull failed`
- `peer sync push failed`

这些现象当前都属于允许的诊断信息，表示 peer sync 不健康或当前视图部分收敛，不表示 surviving ViewNode 自身停止服务。

## 7. discovery / status / availability 语义

- discovery：
  - surviving ViewNode 继续提供 metadata discovery
  - storage discovery 在 partial registry 下允许暂时不完整
- status：
  - surviving ViewNode 继续提供 status 服务
  - `GetClusterView()` 返回 `kOk` 且可见 survivor 自身状态
- availability：
  - 当前 availability 以 surviving ViewNode 自身是否 live、是否继续提供 discovery/status 为准
  - cluster partial / degraded 不应直接降格为 node unavailable

## 8. degraded / partial 的当前语义

- `degraded`：节点可继续服务，但存在 peer sync 或观测不完整等非致命问题。
- `partial`：集群观测尚未完全收敛，例如 failover 后 storage registry 暂时为空。
- 以上两者都不等价于：
  - `unavailable`
  - `dead`
  - `not serving`

## 9. ViewNode authority 边界

- 当前 ViewNode 仍然是 discovery / observation 层。
- ViewNode 不参与 metadata membership authority。
- ViewNode 不参与 learner / voter 决策。
- ViewNode 不影响 quorum 计算。
- ViewNode 暴露的 metadata 角色和 leader hint 仍然只是观测信息，不是 authority。

## 10. 当前已验证范围

- Linux targeted CTest：
  - `ViewNodeDiscoveryTest.*`
  - `ViewFailoverTest.*`
  - `ViewFailoverScriptValidation`
- Linux local RPC example：
  - `examples/object-storage-local-009-dynamic/rpc_demo.sh failover-view`
- 已覆盖的 failover 稳定化边界：
  - surviving ViewNode available
  - peer sync failure 不影响 self available
  - discovery/status 继续工作
  - `cluster degraded` / `partial` 不传播为 `node unavailable`

## 11. 当前未验证范围

- Windows 运行时验证
- macOS 运行时验证
- 长时间 peer sync 断连 / 回退 / 重连 soak
- failover 后完整重启场景下的 registry rehydration
  - 当前仍受 memory-only registry recovery boundary 限制

## 12. Linux 验证状态

- 复用 T099 证据：
  - targeted build：PASS
  - targeted CTest `ViewFailover|FailoverView|ViewNode`：PASS
  - `ViewFailoverScriptValidation`：PASS
  - `rpc_demo.sh failover-view`：PASS
- 复用 T100 证据：
  - `cmake --build --preset debug-ninja-low-parallel --target view_failover_test`：PASS
  - `ctest --preset debug-tests -R "ViewFailover|FailoverView|ViewNode" --output-on-failure`：PASS (`37/37`)

## 13. Windows 验证状态

- 未验证。
- 当前只能记录为 pending，不能宣称 PASS。

## 14. 文档同步更新情况

- 已同步更新：
  - `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
  - `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- 未更新：
  - `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- 原因：
  - `R22` 和 `R23` 已覆盖本轮 failover 误判与 example runtime state 风险，T101 未发现新的跨任务风险类别。

## 15. 结论

- 本任务状态：PASS
- T101 已勾选
- 可以进入下一任务

## 风险和后续注意事项

- 这轮收口确认的是 failover availability 语义与验证边界，不是新的生产功能扩展。
- 后续若继续调整 `failover-view` 判定或状态聚合，必须继续保持：
  - `peer sync failure != self unavailable`
  - `cluster degraded != node unavailable`
  - ViewNode 非 authority 边界不变
- 若后续增加更多 local RPC runtime 验证，应继续区分：
  - 脚本层 readiness 误判
  - 服务端真实 unavailable
