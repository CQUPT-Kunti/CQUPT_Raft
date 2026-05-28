# modules/store/index

## 模块职责

- 本模块定义本地 chunk 索引接口和 sharded map 结构。
- 当前核心是 `ChunkIndex` 抽象接口和 `ShardedChunkIndex` 基础实现。

## 主要文件

- `chunk_index.h`：接口和返回结构
- `chunk_index.cpp`：基础内存实现
- `module-notes.md`：索引语义、分页边界和后续扩展说明

## 修改规则

- 这里只维护本地索引，不做文件 IO 或 durable publish。
- 不在这里引入平台文件 API、RaftNode、proto 或 KV 路径。
- 如果修改返回语义、分页参数或 shard 结构，记得同步维护 `module-notes.md`。
- per-chunk lock 和真正的并发控制留给后续任务，不要在这里提前塞复杂锁逻辑。
