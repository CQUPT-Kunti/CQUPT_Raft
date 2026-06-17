# T105 Final ViewNode Self-Refresh And Peer-Sync Contract Validation

## 1. 本任务做了什么

- 对 `contracts/view-node-self-refresh-and-peer-sync.md`、`modules/view/*`、`apps/view_node_app.cpp`、`tests/view_node_discovery_test.cpp`、`tests/view_failover_test.cpp`、`validation-matrix.md`、`module-notes.md` 做了最终一致性核对。
- 确认当前阶段不需要新增功能，主要收口点是修正文档对真实实现和真实测试入口的滞后描述。
- 同步更新 contract、validation matrix、module notes 和任务状态，使其与当前实现和回归测试保持一致。

## 2. 修改了哪些文件

- `specs/009-local-rpc-object-storage-stabilization/contracts/view-node-self-refresh-and-peer-sync.md`
- `specs/009-local-rpc-object-storage-stabilization/module-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t105-final-viewnode-self-refresh-and-peer-sync-contract-validation.md`

## 3. contract 与实现是否一致

- 一致。
- 已确认：
  - self refresh loop 已在 `apps/view_node_app.cpp` 中启动。
  - peer sync background loop、retry、backoff 已在 `apps/view_node_app.cpp` 中实现。
  - `view_registry.*` 的 merge 顺序保持 incarnation 优先、同 incarnation 再按 sequence。
  - `observed_time` 只用于 TTL/liveness/diagnostics，不单独决定覆盖顺序。
  - `view_service_impl.*` / `view_client.*` 暴露的仍是 discovery/observation 语义，不扩张为 authority。
  - ViewNode runtime registry 仍是 memory-only；重启后依靠 startup register + self refresh + peer sync 重新收敛。

## 4. contract 与测试是否一致

- 一致。
- `tests/view_node_discovery_test.cpp` 已覆盖：
  - self refresh 超过 TTL 后仍保持 `LIVE`
  - self refresh 停止后的 `STALE` / `SUSPECT` / `DEAD`
  - self refresh payload 完整性
  - higher incarnation / higher sequence merge
  - `observed_time` 不能单独覆盖新状态
  - peer snapshot pull/push RPC
  - old-incarnation peer snapshot rejection
  - discovery/status RPC 语义
- `tests/view_failover_test.cpp` 已覆盖：
  - surviving ViewNode availability
  - peer sync failure 不导致 self unavailable
  - failover 后 discovery/status 继续可用
  - recovery 后 peer sync reconvergence
  - 多 ViewNode final convergence
  - 恢复旧 registry snapshot 后不会回退新 incarnation / 新状态

## 5. 本阶段最终覆盖了哪些能力

- ViewNode self refresh
- TTL liveness transition
- peer snapshot pull/push RPC
- peer sync active-active convergence
- failover 后 surviving ViewNode 可用性
- peer sync failure 下 self availability 保持
- restart 后通过 peer sync 重新收敛
- restored snapshot merge safety
- incarnation-aware merge
- discovery-only / observation-only 边界
- metadata leader hint 与 storage candidate discovery
- `GetClusterView()` status 语义
- ViewNode 不参与：
  - authority 决策
  - metadata membership
  - learner/voter 决策
  - quorum

## 6. 是否发现缺口

- 未发现当前阶段必须新增功能才能闭环的缺口。
- 发现并修正的缺口是文档滞后：
  - `module-notes.md` 之前仍写成“没有 peer sync background loop / retry / active-active runtime convergence”
  - contract 和 validation matrix 对 `tests/view_failover_test.cpp` 的覆盖面描述不完整
  - restart boundary 需要更明确地区分：
    - runtime 仍是 memory-only
    - 恢复旧 snapshot 的 merge/reconvergence 语义已被 targeted tests 覆盖

## 7. 验证结果

- 构建：
  - `(
       flock -n 9 || exit 99
       cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery view_failover_test
     ) 9>/tmp/cqupt_raft_build.lock`
  - PASS
- 测试：
  - `ctest --preset debug-tests -R "ViewNode|ViewFailover" --output-on-failure`
  - PASS
  - `41/41` tests passed

## 8. 是否 PASS / FAIL / SKIPPED

- PASS

## 9. 是否已勾选 T105

- 已勾选

## 10. 是否完成 ViewNode self-refresh 与 peer-sync 阶段收口

- 已完成当前阶段收口。

## 本阶段遗留风险

- runtime ViewNode registry 仍然不是 durable persistence；当前闭环的是 memory-only runtime boundary 与恢复后 reconvergence 语义。
- Windows/macOS 仍未做对应 runtime 验证，只能保持 pending。
- 长时间 repeated disconnect/retry 的 soak 仍不在本阶段 PASS 范围内。

## 后续阶段注意事项

- 如果未来引入真正的 registry persistence/load，必须复用当前已验证的 snapshot/import merge contract，不能新造第二套恢复规则。
- 后续任何 Metadata learner/voter/quorum 相关工作都必须继续保持 ViewNode 的 discovery-only 边界，不要把 observed membership 状态提升成 authority。
