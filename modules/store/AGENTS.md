# modules/store

## 目录职责

- `modules/store/` 是 StorageNode / chunk data-plane 的主目录。
- 当前按职责拆成三个子模块：
  - `common/`：基础类型、chunk_id helper、checksum helper
  - `chunk/`：`ChunkStore` 抽象接口
  - `io/`：durable file 抽象接口和平台实现边界
  - `index/`：本地 chunk 索引接口和 sharded map 结构

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
- `chunk/` 只定义 chunk store 接口，不落本地磁盘实现。
- `io/` 只处理 durable file 语义，不承接 chunk 业务编排。
- `index/` 只维护本地索引，不负责磁盘扫描和上层对象可见性。
