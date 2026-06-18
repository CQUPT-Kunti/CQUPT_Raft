# 009 Local RPC Object Storage Stabilization Report

## 范围

本报告收口 008 阶段完成后，在 `examples/object-storage-local-3meta-6store` 真实 RPC 场景中继续暴露出来的本地稳定性问题，以及对应的最小修复与验证结果。

本次收口不改变以下边界：

- 不降低 Raft quorum
- 不让 ViewNode 参与 Raft membership / quorum / leader election
- 不让 follower 执行 Metadata authority 写路径
- 不用客户端 discovery fallback 掩盖 Metadata leader 不稳定
- 不修改测试断言来伪造通过

## 真实场景与症状

测试环境：

- 1 个 ViewNode
- 3 个 MetadataNode
- 6 个 StorageNode
- 客户端通过 `storage_client` 走真实 gRPC / RPC
- 测试文件目录：`tests/test_file`

初始症状按时间顺序分为四类：

1. Metadata leader 高频抖动
   - `CreateWritePlan` 经常返回 `METADATA_NOT_LEADER`
   - `leader_hint_id=-1`
   - metadata 日志持续出现 `start election / won election / become follower`
   - `term` 快速增长

2. bucket 缺失导致无效 metadata mutation 进入 committed 路径
   - upload 失败：`not found: bucket does not exist`
   - 之后 metadata 日志持续出现：
     - `state machine apply failed, index=2, reason=not found: bucket does not exist`

3. `CreateWritePlan` 返回 `version=0`
   - upload 失败：
     - `Metadata CreateWritePlan did not return a usable version`

4. `CommitObject` 与 `CreateWritePlan` 复用同一 metadata request_id
   - chunk 已真实写入 StorageNode
   - 最后 `CommitObject` 失败：
     - `idempotency conflict: request_id maps to different command`

## 本次修复

### 1. Metadata leader 抖动修复

修改文件：

- [apps/metadata_node_app.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/apps/metadata_node_app.cpp)

根因：

- `cluster.json` 中的 `heartbeat_interval_ms=1000` 被错误映射到 Raft 共识 heartbeat
- 但 `RaftNode` 默认 election timeout 仅 `300ms~600ms`
- follower 会在下一次 heartbeat 前先超时发起选举

修复：

- 取消把 cluster config 的观测层 heartbeat 直接覆盖到 Raft 共识 heartbeat
- 保留 `RaftNode` 默认的毫秒级 heartbeat / election timeout

结果：

- Metadata 3 节点稳定形成单 leader
- `term=1`
- 不再出现快速 term 增长
- `NOT_LEADER` 不再成为 upload 主故障

### 2. bucket 缺失前置拒绝

修改文件：

- [modules/raft/service/metadata_service_impl.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/service/metadata_service_impl.cpp)

根因：

- `MetadataServiceImpl::CreateObject` 在 bucket 不存在时没有先拒绝
- 无效 `CreateObject` 仍然进入 proposal / replicate / commit 路径
- 最后在 apply 阶段才失败，污染 committed metadata apply 边界

修复：

- 在 leader 本地、进入 `ProposeMetadata(...)` 之前先检查：
  - bucket 是否存在
  - bucket 是否已删除
  - object 是否已存在
- bucket 缺失时直接返回 `NOT_FOUND`
- object 已存在时直接返回 `STATE_CONFLICT`

结果：

- bucket 不存在不再产生新的 committed 无效日志项
- 无效请求被挡在 metadata authority service 入口

### 3. CreateWritePlan 版本分配修复

修改文件：

- [modules/raft/service/metadata_service_impl.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/service/metadata_service_impl.cpp)

根因：

- transfer 侧 `CreateWritePlan` 请求的 `version` 故意传 `0`
- 语义是“请 MetadataService 分配稳定版本”
- 但服务端原样把 `0` 写入 pending object record
- 导致客户端收到 `version=0` 后拒绝继续上传

修复：

- 当 `CreateObject` 请求携带 `version=0` 时：
  - 在 leader 本地基于 committed metadata 边界分配稳定版本
  - 新对象默认分配 `1`
  - 若同 key 之前存在已删除对象，则分配 `existing.version + 1`

结果：

- `CreateWritePlan` 返回的 `version` 变为可用正数
- 客户端可以继续进入真实 chunk 写路径

### 4. CommitObject request_id 分离

修改文件：

- [modules/store/transfer/object_transfer.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/store/transfer/object_transfer.cpp)

根因：

- `CreateWritePlan` 与 `CommitObject` 使用同一个 metadata `request_id`
- metadata 幂等表按 request_id 全局去重
- 因此把“同 request_id，不同命令”判定为幂等冲突

修复：

- 保持 `CreateWritePlan` 使用 upload session 主 request_id
- `CommitObject` 改用：
  - `<upload_request_id>/commit`

结果：

- chunk 写完后，`CommitObject` 不再触发 metadata idempotency conflict
- upload 可以真正完成到 COMMITTED

## 真实 RPC 测试步骤

### 1. 只编译运行相关 target

执行：

```bash
cmake --build --preset debug-ninja-low-parallel --target \
  view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client
```

说明：

- 只编译运行/验证所需 app target
- 不额外构建 tests target

### 2. 清理 example 运行目录

执行：

```bash
cd examples/object-storage-local-3meta-6store
./tingzhi.sh
rm -rf logs/* pids/* downloads
find nodes -mindepth 2 -maxdepth 2 \( -name data -o -name snapshots -o -name tmp -o -name raft_data -o -name raft_snapshots \) -exec rm -rf {} +
```

说明：

- 必须清掉 metadata / storage 的旧 data 与 snapshots
- 避免前一次失败留下的 pending object、坏日志项或 chunk 副本污染结果

### 3. 启动真实集群

执行：

```bash
./qidong.sh
```

观察：

- `view-1`
- `meta-1/meta-2/meta-3`
- `store-1` 到 `store-6`

都成功启动

### 4. 先做 metadata bucket bootstrap

执行：

```bash
../../build/linux/raft_metadata_client 127.0.0.1:7401 create-bucket \
  --request-id example-bucket-init-1 \
  --bucket example-bucket
```

说明：

- 当前 `storage_client` 没有 `create-bucket` CLI
- 因此 roundtrip 前显式 bootstrap bucket

### 5. 执行真实 status 与 roundtrip

执行：

```bash
./rpc_demo.sh status
./rpc_demo.sh roundtrip
```

其中 `rpc_demo.sh roundtrip` 会：

- 从 `tests/test_file` 收集全部真实文件
- 逐个 upload
- 逐个 download
- 最后本地文件做 `cmp` 比对

## 真实 RPC 测试结果

### status

通过：

- `metadata_nodes=3`
- `storage_nodes=6`
- metadata leader 稳定为 `meta-1`
- `term=1`
- `leader_hint.endpoint=127.0.0.1:7401`

### upload

通过的真实文件：

- `server.jar`
- `test_file.deb`
- `test_file.zip`
- `区域扩散.pdf`

关键事实：

- 每个对象都真实进入 Metadata `CreateWritePlan`
- 都真实进入 StorageNode `WriteChunk`
- chunk 实际写入了 3 个副本节点
- 最终 `CommitObject` 成功
- 对象获得 COMMITTED manifest

### download

通过的真实文件：

- `server.jar`
- `test_file.deb`
- `test_file.zip`
- `区域扩散.pdf`

关键事实：

- download 走 MetadataNode COMMITTED manifest
- 逐 chunk 读取 StorageNode 数据
- 最终对象级 checksum 校验 `integrity=PASS`

### 本地文件比对

最终结果：

- `[verify] OK server.jar`
- `[verify] OK test_file.deb`
- `[verify] OK test_file.zip`
- `[verify] OK 区域扩散.pdf`

## 当前结论

本次 local RPC stabilization 已经完成以下收口：

- Metadata leader 抖动已修复
- bucket 缺失前置拒绝已修复
- `CreateWritePlan` 版本分配已修复
- `CommitObject` request_id 分离已修复
- `examples/object-storage-local-3meta-6store` 真实 roundtrip 已跑通

即：

- `CreateWritePlan -> WriteChunk -> CommitObject -> Download -> 最终文件比对`

主链路在本地真实 RPC 场景下已经验证通过。

## 当前剩余观察

`status` 中 `view-1` 的 self-liveness 仍可能显示 `stale/dead`。

这说明：

- ViewNode 自身 registry 记录没有持续 self-heartbeat
- 这是观测层问题，不是当前数据面/一致性主故障
- 不影响本次 roundtrip 主链路通过

建议后续作为单独稳定性项处理，不与本次 metadata / object path 修复混合提交。
