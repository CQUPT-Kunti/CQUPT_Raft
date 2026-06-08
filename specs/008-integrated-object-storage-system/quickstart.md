# Quickstart: Integrated Object Storage System

**Feature**: `008-integrated-object-storage-system`  
**Purpose**: 描述本阶段手动验收的推荐流程，并明确区分“当前仓库已经实现的命令形态”和“仍需完整联调/后续任务继续收敛的目标流程”。

## 1. Configure And Build

Linux 下，建议先只构建当前 quickstart 需要的 app targets：

```bash
cmake --preset debug-ninja-safe
cmake --build --preset debug-ninja-safe --target \
  storage_client \
  view_node_app \
  metadata_node_app \
  storage_node_app
```

如果需要做全量构建，仍可使用：

```bash
cmake --preset debug-ninja-safe
cmake --build --preset debug-ninja-safe
```

说明：

- 下面的 Linux 示例默认从仓库根目录执行，并使用 `build/linux/safe/` 下的可执行文件。
- Windows 下请在对应 preset 的构建输出目录执行同名 `.exe`，参数名与 Linux 示例一致，只需把路径分隔符切换为 Windows 风格。

## 2. Generate A Local Cluster Config

当前已实现命令：

```bash
./build/linux/safe/storage_client generate-config \
  --out tmp/008-cluster/cluster.json \
  --base-dir tmp/008-cluster/data \
  --cluster-id cluster-008-local \
  --view-count 1 \
  --metadata-count 3 \
  --metadata-voters 3 \
  --storage-count 3 \
  --chunk-size 4194304 \
  --replicas 3 \
  --min-writes 2
```

Windows 示例：

```powershell
.\storage_client.exe generate-config `
  --out tmp\008-cluster\cluster.json `
  --base-dir tmp\008-cluster\data `
  --cluster-id cluster-008-local `
  --view-count 1 `
  --metadata-count 3 `
  --metadata-voters 3 `
  --storage-count 3 `
  --chunk-size 4194304 `
  --replicas 3 `
  --min-writes 2
```

当前可验证：

- 统一 cluster config JSON 已可生成。
- 生成结果包含 ViewNode、MetadataNode、StorageNode 配置。
- 初始 Raft membership 已按配置生成；3 个 voter 的 quorum 为 2。
- 默认 `node_id` 形态与当前实现一致，例如 `view-1`、`meta-1`、`store-1`。

目标流程说明：

- 这里展示的是当前已经实现的命令形态，不代表后续所有集群联动能力都已完整收敛。

## 3. Start Nodes

当前已实现的启动参数形态如下：

```bash
./build/linux/safe/view_node_app \
  --config tmp/008-cluster/cluster.json \
  --node_id view-1

./build/linux/safe/metadata_node_app \
  --config tmp/008-cluster/cluster.json \
  --node_id meta-1

./build/linux/safe/metadata_node_app \
  --config tmp/008-cluster/cluster.json \
  --node_id meta-2

./build/linux/safe/metadata_node_app \
  --config tmp/008-cluster/cluster.json \
  --node_id meta-3

./build/linux/safe/storage_node_app \
  --config tmp/008-cluster/cluster.json \
  --node_id store-1

./build/linux/safe/storage_node_app \
  --config tmp/008-cluster/cluster.json \
  --node_id store-2

./build/linux/safe/storage_node_app \
  --config tmp/008-cluster/cluster.json \
  --node_id store-3
```

Windows 示例：

```powershell
.\view_node_app.exe --config tmp\008-cluster\cluster.json --node_id view-1
.\metadata_node_app.exe --config tmp\008-cluster\cluster.json --node_id meta-1
.\storage_node_app.exe --config tmp\008-cluster\cluster.json --node_id store-1
```

当前可验证：

- 三个 app target 名称已经接入构建：
  - `view_node_app`
  - `metadata_node_app`
  - `storage_node_app`
- 三个 app 都支持：
  - `--config`
  - `--node_id`
  - `--data_dir`
  - `--listen`
- 每个节点都会加载或创建本地 `node.identity`。
- `metadata_node_app` 会装配并启动 `RaftNode`，输出初始 voter/quorum 诊断。
- `storage_node_app` 会初始化本地 chunk store、装配 StorageNode gRPC service，并建立本地 registry seed。
- `view_node_app` 会建立本地 ViewNode registry seed 和 gRPC 生命周期边界。

当前限制 / 后续联调项：

- `view_node_app` 当前是 thin startup boundary，不应在 quickstart 中理解为 ViewNode 全部业务链路已完成。
- `storage_node_app` 当前不会在 app 内实现完整的 ViewNode registration / heartbeat loop；这属于后续任务继续收敛的范围。
- 因此，下面的 upload/download 与 discovery 联调应视为“目标验收流程”，需要结合后续集成状态一起验证。

## 4. Upload A Real File

先准备一个真实文件：

```bash
python3 - <<'PY'
from pathlib import Path
import os
p = Path("tmp/008-cluster/input.bin")
p.parent.mkdir(parents=True, exist_ok=True)
p.write_bytes(os.urandom(64 * 1024 * 1024))
PY

sha256sum tmp/008-cluster/input.bin > tmp/008-cluster/input.sha256
```

当前已实现命令：

```bash
./build/linux/safe/storage_client upload \
  --config tmp/008-cluster/cluster.json \
  --bucket demo \
  --object input.bin \
  --file tmp/008-cluster/input.bin
```

Windows 示例：

```powershell
.\storage_client.exe upload `
  --config tmp\008-cluster\cluster.json `
  --bucket demo `
  --object input.bin `
  --file tmp\008-cluster\input.bin
```

当前命令边界：

- `storage_client upload` 已实现，真实参数名为：
  - `--config`
  - `--bucket`
  - `--object`
  - `--file`
- 可选参数还包括：
  - `--object-id`
  - `--request-id`
  - `--chunk-size`
  - `--replicas`
  - `--min-writes`
  - `--concurrency`

目标验收流程：

- Client 通过 ViewNode 获取 MetadataNode 地址。
- MetadataNode 生成 `WritePlan/Placement`。
- Client 将 chunk 写入 StorageNode。
- `CommitObject` 通过 Raft quorum 后对象变为 `COMMITTED`。
- 成功输出 `object_id`、`version`、`size`、`checksum`、`chunk_count`、`request_id`。

说明：

- 上述是当前命令形态对应的目标端到端流程；是否已经在当前分支完成跨进程全链路联调，需要结合实际集群运行状态验证。

## 5. Download And Verify

当前已实现命令：

```bash
./build/linux/safe/storage_client download \
  --config tmp/008-cluster/cluster.json \
  --bucket demo \
  --object input.bin \
  --out tmp/008-cluster/output.bin

sha256sum tmp/008-cluster/output.bin > tmp/008-cluster/output.sha256
diff tmp/008-cluster/input.sha256 tmp/008-cluster/output.sha256
```

Windows 示例：

```powershell
.\storage_client.exe download `
  --config tmp\008-cluster\cluster.json `
  --bucket demo `
  --object input.bin `
  --out tmp\008-cluster\output.bin
```

当前命令边界：

- `storage_client download` 已实现，真实参数名为：
  - `--config`
  - `--bucket`
  - `--object`
  - `--out`
- 可选参数还包括：
  - `--object-id`
  - `--version`
  - `--request-id`
  - `--concurrency`

目标验收流程：

- Client 通过 ViewNode 找 MetadataNode。
- Client 获取 `COMMITTED` manifest。
- Client 按 manifest 读取 StorageNode chunk。
- 每个 chunk checksum 正确。
- CLI 最终只在对象 checksum 已验证通过时输出 `download OK ... integrity=PASS`。

## 6. Validate Quorum Safety

这是本阶段必须保留的目标验收流程：

1. 使用 3 个 `MetadataNode` 启动集群。
2. 停止其中 2 个 voter。
3. 尝试上传新对象。

预期：

- 没有合法 leader 或无法 commit。
- 新对象不会变为 `COMMITTED`。
- 当前 live 节点数减少不能动态降低 quorum。

说明：

- 这里属于 quorum safety 的目标场景。
- 当前仓库对此还应结合 `integrated_object_storage_quorum` 等测试覆盖一起验证，而不是只依赖 quickstart 人工操作。

## 7. Validate StorageNode Restart

这是本阶段必须保留的目标验收流程：

1. 上传并 commit 对象。
2. 停止全部 `StorageNode`。
3. 使用原 `data_dir` 重启 `StorageNode`。
4. 下载对象并验证 SHA-256。

预期：

- 每个 `StorageNode` 复用原 `node_id`。
- 已发布 chunk 可读。
- staging / incomplete chunk 不会伪装成已提交数据。

说明：

- 这部分是 restart / recovery 方向的目标验收流程。
- 若当前分支尚未完成全部恢复链路联调，不要把该流程解读为“已经稳定可用且无需额外验证”。

## 8. Test Entry Points

当前可用测试入口：

```bash
./test.sh --group unit
CTEST_PARALLEL_LEVEL=1 ./test.sh --group all
```

计划中的 CTest 覆盖包括：

- `integrated_object_storage_e2e`
- `integrated_object_storage_quorum`
- `integrated_object_storage_recovery`
- `integrated_object_storage_concurrency`
- `view_node_discovery`
- `node_identity`

测试日志规则：

- PASS 只报告命令、PASS、耗时。
- FAIL 只报告失败测试名、关键断言、失败分类、最后 50 行日志和完整日志路径。
