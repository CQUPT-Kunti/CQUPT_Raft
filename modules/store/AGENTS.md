# modules/store

## 目录职责

- `modules/store/` 是 StorageNode / chunk data-plane 的主目录。
- 当前按职责拆成多个子模块：
  - `common/`：基础类型、chunk_id helper、checksum helper
  - `chunk/`：`ChunkStore` 抽象接口和 `LocalDiskChunkStore` 本地实现骨架
  - `io/`：durable file 抽象接口和平台实现边界
  - `index/`：本地 chunk 索引接口和 sharded map 结构
  - `runtime/`：有界执行器和有界任务队列
  - `node/`：StorageNode data-plane 的 gRPC service / client 适配层
  - `placement/`：副本策略和副本候选节点选择
  - `upload/`：上传协调层和 upload helper
  - `maintenance/`：GC / scrub / repair / rebalance 等后台维护任务基础设施

## 阅读顺序

- 先读本文件。
- 再进入目标子模块读取对应的 `AGENTS.md`。
- 然后再读该模块的 `module-notes.md`、`.h`、`.cpp` 和直接相关测试。

## 修改规则

- 保持命名空间统一为 `storedemo`。
- 公共数据结构和接口放 `.h`，实现放 `.cpp`。
- 结构体、类、函数的说明优先维护在各模块的 `module-notes.md`，不要把解释散落成大量代码注释。
- 不要在这里恢复 KV 语义、Raft control-plane 依赖或旧 `modules/raft/storage_node/` 路径。

## 子模块边界

- `common/` 不做文件 IO、不做 RPC。
- `chunk/` 负责 chunk store 接口和本地磁盘 store 编排骨架，但具体 durable file 细节仍下沉到 `io/`。
- `io/` 只处理 durable file 语义，不承接 chunk 业务编排。
- `index/` 只维护本地索引，不负责磁盘扫描和上层对象可见性。
- `runtime/` 只负责本地任务调度，不绑定具体 chunk / IO 业务。
- `node/` 只负责 RPC 适配，不负责 metadata commit、upload coordinator 或 Raft control-plane。
- `placement/` 只负责候选节点选择和副本策略计算，不负责真正发起写入或提交 metadata。
- `upload/` 只负责 upload 顺序协调，不负责 metadata 底层实现、StorageNode RPC server 或后台 GC。
- `maintenance/` 只负责后台任务模型、队列、重试和调度边界，不直接决定 metadata safety 或 object 可见性。
