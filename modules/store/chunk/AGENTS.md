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
- `local_disk_chunk_store.cpp`：目录初始化和当前未实现接口的明确返回
- `module-notes.md`：接口、字段和边界说明

## 修改规则

- `chunk_store.*` 继续保持抽象接口边界。
- `local_disk_chunk_store.*` 可以编排目录初始化、依赖注入和后续 chunk 生命周期入口，但不要把平台 durability 细节复制到这里。
- 目录路径、publish 和路径安全 helper 优先复用 `modules/store/io/`，不要在这里重复发明一套路径规则。
- 请求/响应字段变更要同步维护 `module-notes.md`。
- `WriteChunk` / `ReadChunk` / `DeleteChunk` / `StatChunk` / `ListChunks` 的真实数据路径按任务顺序逐步实现，不要提前把 T023 之后的行为塞进 T021。
