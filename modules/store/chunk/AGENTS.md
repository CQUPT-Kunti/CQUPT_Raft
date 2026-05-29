# modules/store/chunk

## 模块职责

- 本模块定义 chunk data-plane 的抽象接口。
- 当前核心包括：
  - `ChunkStore` 以及相关 request / response 结构
  - `LocalDiskChunkStore` 的配置、初始化和后续本地实现骨架

## 主要文件

- `chunk_store.h`：接口和请求/响应结构
- `chunk_store.cpp`：当前仅保留虚析构实现
- `local_disk_chunk_store.h`：本地磁盘 store 的配置、初始化结果和类声明
- `local_disk_chunk_store.cpp`：目录初始化、`WriteChunk` / `ReadChunk` / `DeleteChunk` / `StatChunk` / `ListChunks` 本地路径
- `module-notes.md`：接口、字段和边界说明

## 修改规则

- `chunk_store.*` 继续保持抽象接口边界。
- `local_disk_chunk_store.*` 可以编排目录初始化、依赖注入和后续 chunk 生命周期入口，但不要把平台 durability 细节复制到这里。
- 目录路径、publish 和路径安全 helper 优先复用 `modules/store/io/`，不要在这里重复发明一套路径规则。
- `WriteChunk` 必须先走 `ChunkIndex` 的 per-chunk guard，再进入 staging/flush/publish/index update；不要绕开这条主路径。
- `ReadChunk` 必须先查 `ChunkIndex`，只读取 `LIVE` final chunk，不要按路径直读未登记文件，也不要回退读取 staging。
- `DeleteChunk` 必须先走 `ChunkIndex` 的 per-chunk guard，只删除目标 final chunk，不要顺手碰 staging 或其它 chunk。
- `StatChunk` / `ListChunks` 必须以 `ChunkIndex` 为准，不要直接扫描文件系统绕过 index。
- 并发测试或后续接入如果覆盖 read/delete/list 交错，断言必须遵守当前 contract：同 chunk write/delete 依赖 guard 串行；read/delete 交错允许返回明确失败，list 分页仍不承诺并发快照一致性。
- required durability operation 必须检查真实 durable boundary，不能把 `kOk` 但未到达 boundary 的结果当成成功。
- 请求/响应字段变更要同步维护 `module-notes.md`。
- 后续 restart rebuild、corruption 状态回写、后台 scrub/repair 仍按任务顺序逐步实现，不要在 T025 提前把恢复流程硬塞进前台路径。
