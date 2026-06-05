# Quickstart: Integrated Object Storage System

**Feature**: 008-integrated-object-storage-system  
**Purpose**: 描述本阶段完成后的手动验收流程。这里是目标流程，不表示当前仓库已经全部实现。

## 1. Configure And Build

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel
```

更保守：

```bash
cmake --preset debug-ninja-safe
cmake --build --preset debug-ninja-safe
```

## 2. Generate A Local Cluster Config

目标命令形态：

```bash
storage_client generate-config \
  --out tmp/008-cluster/cluster.json \
  --base_dir tmp/008-cluster/data \
  --view_nodes 1 \
  --metadata_nodes 3 \
  --storage_nodes 3 \
  --chunk_size 4194304 \
  --replicas 1
```

预期：

- 生成 ViewNode、MetadataNode、StorageNode 配置。
- 生成初始 Raft membership，3 个 voter 的 quorum 为 2。
- 为需要预分配身份的节点生成稳定 node_id/raft_id。
- 不要求用户手工编辑固定端口或节点名称。

## 3. Start Nodes

目标 Linux 形态：

```bash
view_node_app --config tmp/008-cluster/cluster.json --node_id view-1
metadata_node_app --config tmp/008-cluster/cluster.json --node_id meta-1
metadata_node_app --config tmp/008-cluster/cluster.json --node_id meta-2
metadata_node_app --config tmp/008-cluster/cluster.json --node_id meta-3
storage_node_app --config tmp/008-cluster/cluster.json --node_id store-1
storage_node_app --config tmp/008-cluster/cluster.json --node_id store-2
storage_node_app --config tmp/008-cluster/cluster.json --node_id store-3
```

目标 Windows 形态：

```powershell
view_node_app.exe --config tmp\008-cluster\cluster.json --node_id view-1
metadata_node_app.exe --config tmp\008-cluster\cluster.json --node_id meta-1
storage_node_app.exe --config tmp\008-cluster\cluster.json --node_id store-1
```

预期：

- 每个节点写入或读取本地 `node.identity`。
- MetadataNode 完成 leader election。
- StorageNode 向 ViewNode 注册并持续心跳。

## 4. Upload A Real File

```bash
python3 - <<'PY'
from pathlib import Path
import os
p = Path("tmp/008-cluster/input.bin")
p.parent.mkdir(parents=True, exist_ok=True)
p.write_bytes(os.urandom(64 * 1024 * 1024))
PY

sha256sum tmp/008-cluster/input.bin > tmp/008-cluster/input.sha256

storage_client upload \
  --config tmp/008-cluster/cluster.json \
  --bucket demo \
  --object input.bin \
  --file tmp/008-cluster/input.bin
```

预期：

- Client 通过 ViewNode 获取 MetadataNode 地址。
- MetadataNode 生成 WritePlan/Placement。
- Client 将 chunk 写入 StorageNode。
- CommitObject 通过 Raft quorum 后对象变为 COMMITTED。
- 输出 object_id、version、size、checksum、chunk_count、request_id。

## 5. Download And Verify

```bash
storage_client download \
  --config tmp/008-cluster/cluster.json \
  --bucket demo \
  --object input.bin \
  --out tmp/008-cluster/output.bin

sha256sum tmp/008-cluster/output.bin > tmp/008-cluster/output.sha256
diff tmp/008-cluster/input.sha256 tmp/008-cluster/output.sha256
```

预期：

- Client 通过 ViewNode 找 MetadataNode。
- Client 获取 COMMITTED manifest。
- Client 按 manifest 读取 StorageNode chunk。
- 每个 chunk checksum 正确。
- 输出文件 SHA-256 与输入完全一致。

## 6. Validate Quorum Safety

目标场景：

1. 使用 3 个 MetadataNode 启动。
2. 停止其中 2 个 voter。
3. 尝试上传新对象。

预期：

- 没有合法 leader 或无法 commit。
- 新对象不会变为 COMMITTED。
- ViewNode 可以显示节点 DOWN/SUSPECT，但不能降低 quorum。

## 7. Validate StorageNode Restart

目标场景：

1. 上传并 commit 对象。
2. 停止全部 StorageNode。
3. 使用原 data_dir 重启 StorageNode。
4. 下载对象并验证 SHA-256。

预期：

- 每个 StorageNode 复用原 node_id。
- 已发布 chunk 可读。
- staging/incomplete chunk 不会伪装成已提交数据。

## 8. Test Entry Points

```bash
./test.sh --group unit
CTEST_PARALLEL_LEVEL=1 ./test.sh --group all
```

计划新增 CTest 覆盖：

- `integrated_object_storage_e2e`
- `integrated_object_storage_quorum`
- `integrated_object_storage_recovery`
- `integrated_object_storage_concurrency`
- `view_node_discovery`
- `node_identity`

测试日志规则：

- PASS 只报告命令、PASS、耗时。
- FAIL 只报告失败测试名、关键断言、失败分类、最后 50 行日志和完整日志路径。
