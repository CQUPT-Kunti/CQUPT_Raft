# T038 Add Peer ViewNode Seed Configuration Parsing

## 做了什么

在 `cluster_config` 中为 `ViewNode` 增加了独立的 `peer_seeds` 配置字段，并补齐了 JSON 解析、序列化、配置校验以及按 `role + node_id` 解析后的结果暴露。

本任务只处理配置模型和解析验证，没有实现 peer sync 网络逻辑，也没有修改 registry merge、Raft membership 或 failover client 行为。

## 修改文件

- `modules/cluster/cluster_config.h`
- `modules/cluster/cluster_config.cpp`
- `tests/cluster_config_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/module-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## cluster config 现在如何支持 ViewNode peer seed

### 配置模型

- `ViewNodeConfig` 新增 `peer_seeds`
  - 类型：`std::vector<std::string>`
  - 语义：当前 ViewNode 可尝试连接的 peer ViewNode endpoint seed 列表

- `ResolvedClusterNodeConfig` 新增 `view_peer_seed_endpoints`
  - 仅当按 `ClusterNodeType::kView` 解析节点时返回
  - 供后续 app wiring / peer sync loop 直接消费

### JSON 读写

- `LoadClusterConfigFromJsonFile(...)` 现在支持解析：

```json
"view_nodes": [
  {
    "node_id": "view-1",
    "endpoint": "127.0.0.1:7301",
    "peer_seeds": ["127.0.0.1:7302"],
    "data_dir": "nodes/view-1/data"
  }
]
```

- `peer_seeds` 缺失或为 `null` 时按空列表处理，保持单 ViewNode baseline 兼容
- `SerializeClusterConfigToJson(...)` 会把 `peer_seeds` 显式写回 JSON

### 校验规则

- `peer_seeds` 中每个条目必须是合法 `host:port`
- 同一个 ViewNode 的 `peer_seeds` 不能重复
- `peer_seeds` 不能指向该 ViewNode 自己的 `endpoint`
- `peer_seeds` 必须匹配另一个已配置的 ViewNode endpoint
- `peer_seeds` 只作为 ViewNode peer sync seed 输入，不参与 `initial_raft_membership`，也不会影响 Metadata/Raft quorum 校验

## 单 ViewNode baseline 兼容性

保持兼容。

- 现有单 ViewNode 配置可以继续不写 `peer_seeds`
- 当前 `examples/object-storage-local-3meta-6store/cluster.json` 无需修改即可继续解析
- Metadata / Storage 配置解析路径没有退化

## 新增或更新的测试

在 `tests/cluster_config_test.cpp` 新增：

- `cluster_config_validation_test.parses_single_view_config_without_peer_seeds_and_keeps_baseline_compatibility`
- `cluster_config_validation_test.parses_multi_view_peer_seeds_and_keeps_initial_membership_unchanged`
- `cluster_config_validation_test.rejects_invalid_view_peer_seed_endpoint_format_with_clear_diagnostics`
- `cluster_config_validation_test.rejects_view_peer_seed_self_reference_and_duplicates`

这些测试覆盖了：

- 单 ViewNode 缺少 `peer_seeds` 仍可解析
- 多 ViewNode peer seed 可 roundtrip 解析，并能通过 `ResolveClusterNodeConfig(...)` 暴露
- 非法 peer seed 能给出明确 field path 诊断
- peer seed 不会改变 Metadata/Raft 初始 membership 校验结果

## 验证命令

构建：

```bash
mkdir -p tmp/test-logs && (
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target cluster_config_test > tmp/test-logs/t038-build.log 2>&1
) 9>/tmp/cqupt_raft_build.lock
```

测试：

```bash
ctest --preset debug-tests -R "cluster_config_" --output-on-failure > tmp/test-logs/t038-ctest.log 2>&1
```

说明：

- 用户建议的 `-R "ClusterConfig"` 在当前仓库里不是实际测试名入口
- 已按真实 CTest 名称 `cluster_config_*` 运行

## 结果

- PASS
- `cluster_config_test` 定向构建通过
- `cluster_config_*` 相关 18/18 测试通过
- Linux 已验证
- Windows/macOS：未实测，pending
- 已在 `tasks.md` 只勾选 T038 完成

## 后续

可以进入后续任务，优先是 T039。`ViewNode` 的 peer seed 配置输入已经准备好，后续可以在此基础上接 peer sync client/server contract 和 app loop。
