# T089 Diagnostic Consistency

## 1. 修改了哪些文件

- `apps/storage_client.cpp`
- `modules/view/view_service_impl.cpp`
- `modules/raft/service/metadata_service_impl.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`

## 2. storage_client.cpp 的 request_id / node_id / leader-hint 诊断一致性做了什么

- 新增统一的 CLI 诊断输出 helper，收口 `FAILED/OK + command + request_id + status + message` 的基本格式。
- `generate-config` 现在在成功、配置校验失败、文件写入失败场景都会输出一致的 `request_id`；未显式传入时会生成本地 request_id。
- `PrintClusterConfigIssues(...)` 现在会把同一个 `request_id` 带到逐条 issue 诊断里，便于和命令主输出对齐。
- `status` 的 transport failure 现在显式输出 `status=GRPC_TRANSPORT_ERROR`，并保留 `grpc_code`、`retryable`、`message`。
- `main()` 中的 `ClientConfigError` 和通用异常改为统一的命令级失败输出，不再使用单独的裸字符串格式。
- 本任务没有新增 CLI 命令，也没有改变 exit code 语义。

## 3. view_service_impl.cpp 的诊断一致性做了什么

- 扩展 `SetInternalSummary(...)`，让 internal error summary 也能回填 `request_id`、`cluster_id`、`node_id`。
- `ValidateRpcState(...)` 现在在 registry 未配置场景下，会带上当前 RPC 已知的上下文标识，而不是只返回一条无上下文的 internal message。
- `RegisterNode`、`HeartbeatNode`、`DiscoverMetadata`、`DiscoverStorage`、`GetClusterView` 的异常路径都补了上下文字段回填。
- `RegisterNode` 对 `generated_new` / `confirmed_existing` 不再粗暴覆盖 registry summary message，而是在原始 message 后追加 `service_note=...`，保留 registry 诊断语义。

## 4. metadata_service_impl.cpp 的诊断一致性做了什么

- 新增 `BuildContextDiagnosticMessage(...)` / `DecorateSummaryWithContext(...)`，统一把本地 MetadataService 节点身份和 leader hint 诊断附加到 summary message。
- 由于 `MetadataResponseSummary` proto 当前没有 `node_id` 字段，本任务没有改 proto，而是把 `local_node_id` / `local_node_address` 通过 message 诊断补齐。
- 写路径 `FillWriteSummary(...)` 现在无论成功还是失败，都会统一填充本地节点和 leader hint 上下文；需要 quorum 诊断时再追加 committed membership / quorum summary。
- 读路径和校验失败路径（`FinishReadError(...)`、`FinishValidationError(...)`、`HeadObject(...)`、`ListObjects(...)`）现在也会补齐本地节点和 leader hint 诊断。
- `HeadObjectRequest` / `ListObjectsRequest` 仍然没有 request_id 字段；本任务保持 proto 不变，因此未强行新增 request_id。

## 5. 是否保持 ViewNode non-authority 和 Raft quorum / leader election 安全边界

- 保持。
- ViewNode 侧只补充 observation/discovery 响应和 internal summary 的诊断上下文，没有把 ViewNode 写成 Raft membership authority 或 object visibility authority。
- MetadataService 侧只读取本地 `NodeStatusSnapshot` 和 committed membership quorum summary 做诊断拼接，没有改变 leader election、quorum、commit、membership 规则。
- storage_client 侧只统一输出格式，没有新增 retry/backoff，也没有改变业务判断条件。

## 6. 是否发现不合理点 / 警告 / 风险

- `MetadataResponseSummary` 缺少结构化 `node_id` 字段，当前只能通过 message 补齐 `local_node_id` / `local_node_address`。如果后续需要更稳定的机器解析能力，建议在后续独立任务中评估 additive proto 扩展。
- `HeadObjectRequest` / `ListObjectsRequest` 没有 request_id；本任务按约束不改 proto，因此这两条读接口仍只能返回空 request_id 或内部派生为空。
- `ctest --preset debug-tests` 当前目录下未发现测试；本次改用 `build/linux/safe` 下的 focused tests 做最小验证。

## 7. 是否修改 risk-register.md 或 module-notes.md；如未修改，明确说明未修改

- 未修改 `risk-register.md`。
- 未修改 `modules/view/module-notes.md`。
- 未修改 `modules/store/transfer/module-notes.md`。
- 未修改 `specs/008-integrated-object-storage-system/contracts/app-cli.md`。

## 8. 验证命令和结果

### diff / 静态检查

```bash
git diff -- apps/storage_client.cpp modules/view/view_service_impl.cpp modules/raft/service/metadata_service_impl.cpp
git diff --check -- apps/storage_client.cpp modules/view/view_service_impl.cpp modules/raft/service/metadata_service_impl.cpp
```

- 结果：diff 符合预期，`git diff --check` 通过。

### 最小构建

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target raft_core storage_client'
```

- 结果：通过。

### ctest 预设检查

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R "view_node_discovery|integrated_object_storage_quorum|integrated_object_storage_e2e" --output-on-failure'
```

- 结果：`No tests were found!!!`。当前实际 configure/build 目录在 `build/linux/safe`，因此改用该目录做 focused tests。

### focused tests

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --test-dir build/linux/safe -R "MetadataClientScenarioTest\\.(ClientShowsRetryableAdmissionStatuses|ReadCommandsShowRetryableAdmissionStatuses)|MetadataFailoverTest\\.(FollowerWriteReturnsNotLeader|FollowerHeadAndListReturnNotLeader)" --output-on-failure'
```

- 结果：4/4 通过。

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --test-dir build/linux/safe -R "ViewNodeDiscoveryTest\\.(DuplicateRegisterIsIdempotentAndConflictsOnEndpointOrFingerprintMismatch|IntegrationStorageDiscoveryReturnsEndpointAndObservedState|ClusterViewCanExcludeDeadNodesAndEmitWarnings)" --output-on-failure'
```

- 结果：3/3 通过。
