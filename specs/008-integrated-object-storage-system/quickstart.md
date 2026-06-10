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
- `metadata_node_app` 会向 ViewNode 执行 registration / heartbeat loop，提供 metadata 节点观测事实。
- `storage_node_app` 会初始化本地 chunk store、装配 StorageNode gRPC service，并建立本地 registry seed。
- `storage_node_app` 会向 ViewNode 执行 registration / heartbeat loop，上报健康、容量、负载和 liveness 观测事实。
- `view_node_app` 会建立本地 ViewNode registry seed 和 gRPC 生命周期边界。

当前限制 / 后续联调项：

- `view_node_app` 当前是 thin startup boundary，不应在 quickstart 中理解为 ViewNode 全部业务链路已完成。
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

## 9. Windows Startup And Path Smoke Notes

本节只描述 Windows 下的 startup/path/durability smoke 边界，不等价于 Linux 的完整故障矩阵验收。

状态标记：

- `Linux 已验证`：当前仓库的主要 build/test 证据仍来自 Linux。
- `Windows smoke/fallback`：当前只承诺配置、路径、启动、identity 和最小命令层面的 smoke 目标。
- `Windows 待实机验证`：如果本节没有明确给出 Windows 实机结果，就只能视为待验证，不能写成 PASS。

### 9.1 Windows Executables And PowerShell Invocation

Windows 下可执行文件名称与 Linux target 一一对应，只是带 `.exe` 后缀：

- `view_node_app.exe`
- `metadata_node_app.exe`
- `storage_node_app.exe`
- `storage_client.exe`

PowerShell 建议：

- 当前目录运行使用 `.\view_node_app.exe`、`.\storage_client.exe` 这类前缀。
- 多行命令使用反引号 `` ` `` 续行，而不是 Bash 的反斜杠 `\`。
- 路径含空格时要加双引号，例如：

```powershell
.\storage_client.exe generate-config `
  --out ".\tmp\008 cluster\cluster.json" `
  --base-dir ".\tmp\008 cluster\data"
```

- `--listen` 仍使用 `host:port` 文字形式，例如 `127.0.0.1:33001`，不要写成 Windows 路径风格。

### 9.2 Startup Arguments And Path Expectations

四个可执行文件在 Windows 下的参数名与 Linux 一致，不存在单独的 Windows CLI 变体：

- `view_node_app.exe`：
  - `--config`
  - `--node_id`
  - `--data_dir`
  - `--listen`
- `metadata_node_app.exe`：
  - `--config`
  - `--node_id`
  - `--data_dir`
  - `--listen`
- `storage_node_app.exe`：
  - `--config`
  - `--node_id`
  - `--data_dir`
  - `--listen`
- `storage_client.exe`：
  - `generate-config`
  - `upload`
  - `download`
  - `status`

路径边界：

- 配置文件、`data_dir`、下载输出路径都应优先使用 `std::filesystem` 可正常解析的本地路径。
- 推荐使用相对路径如 `.\tmp\008-cluster\...`，或显式绝对路径如 `C:\work\CQUPT_Raft\tmp\...`。
- 避免把 `--listen`、`--node_id`、`--object` 等非路径参数写成带反斜杠的 Windows 文件名。
- 避免保留文件名和非法字符，例如 `CON`、`PRN`、`NUL`、`<`、`>`、`:`、`"`、`|`、`?`、`*`。
- 如果仓库所在目录或 `base-dir` 很深，建议在 Windows 上开启长路径支持，避免 `data_dir`、snapshot、chunk staging 路径过长。

### 9.3 data_dir And node.identity On Windows

Windows 下 `node.identity` 的预期和 Linux 相同：

- 首次启动时在当前节点 `data_dir` 下创建 `node.identity`
- 后续重启时复用同一 `node.identity`
- identity/config mismatch 必须失败并给出诊断，不能静默覆盖

app 级别差异：

- `view_node_app.exe` 和 `storage_node_app.exe` 的 `--data_dir` 仍是受控本地测试 override。
- `metadata_node_app.exe` 的 `--data_dir` / `--listen` 是“校验型 override”：
  - 它会校验是否与配置生成的 durable identity / raft identity 一致
  - 不一致时拒绝启动，而不是接受漂移

Windows smoke 目标：

- `storage_client.exe generate-config` 生成配置
- `view_node_app.exe --help`、`metadata_node_app.exe --help`、`storage_node_app.exe --help`
- 至少一个节点的 `node.identity` 创建与重启复用

当前状态：

- `Windows 待实机验证`：本 quickstart 只给出行为预期，没有在当前任务内声明 Windows 实机 PASS。

### 9.4 Windows Durability Expectation

Windows 不允许把 required durability operation 视为 no-op success。

当前 durability contract 预期：

- `node.identity` publish 必须使用真实的 Windows 等价 durability 路径，例如 `FlushFileBuffers`、`MoveFileExW` 或等价安全替换序列。
- StorageNode chunk publish / download 临时文件 publish 也必须遵守同类 durability / replace 语义。
- 如果某个 Windows 路径做不到等价保证，必须返回明确错误或记录较弱 contract；不能“假装成功”。

这部分属于：

- `Linux 已验证`：主要 runtime 证据仍在 Linux。
- `Windows smoke/fallback`：这里只声明 contract，不宣称已经通过所有 Windows durability 验收。
- `Windows 待实机验证`：真实 `FlushFileBuffers` / rename / replace / recovery 行为仍需单独实机确认。

### 9.5 Temporary Directory, Firewall And Port Notes

Windows 上建议注意：

- 不要假设 `/tmp` 存在；临时目录应使用平台默认 temp 路径。
- 多次重跑集群前，确认旧进程已退出、旧端口已释放。
- 首次监听端口时，Windows Defender Firewall 可能弹出提示；本地 loopback 多进程 smoke 建议优先使用 `127.0.0.1`。
- 如果 `view_node_app.exe`、`metadata_node_app.exe`、`storage_node_app.exe` 分多个 PowerShell 窗口启动，建议先启 ViewNode，再启 MetadataNode，再启 StorageNode，最后再跑 `storage_client.exe`。
- Windows 上文件替换、删除和端口释放的时序可能比 Linux 更敏感；不要把 Linux 下“立即重启成功”的经验直接视为 Windows 已验证。

### 9.6 Recommended Windows Smoke Scope

推荐的 Windows smoke 范围：

1. `storage_client.exe generate-config` 成功生成配置文件。
2. 四个可执行文件至少完成 `--help` 或参数缺失检查。
3. `view_node_app.exe` / `metadata_node_app.exe` / `storage_node_app.exe` 至少做一次最小启动。
4. 验证 `data_dir` 下 `node.identity` 创建后，再用同一路径重启并复用。
5. 如环境允许，再做最小 upload/download smoke，但这只能视为命令层 smoke，不等价于 Linux 的 recovery/concurrency/full-matrix PASS。

不应在 Windows quickstart 中直接宣称：

- recovery matrix 已通过
- 100-op concurrency 已通过
- durability failure injection 已通过
- 所有 Linux-only 验收步骤都已在 Windows 验证

### 9.7 Windows Build And Test Fallback Notes

Linux 文档里的 `flock` 只适用于 Linux 开发窗口并发控制，不应直接照搬到 Windows。

Windows fallback 建议：

- 同一个 build 目录同一时刻只保留一个 configure/build/test 写入者。
- 如果没有等价锁机制，至少人工串行执行：
  - `cmake --preset ...`
  - `cmake --build --preset ... --target storage_client view_node_app metadata_node_app storage_node_app`
- 在没有 Windows 实机验证结果前，本 quickstart 只能把这些步骤记为“建议 smoke”，不能记为 PASS。
