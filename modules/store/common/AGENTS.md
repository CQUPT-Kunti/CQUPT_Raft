# modules/store/common

## 模块职责

- 本模块提供 `storedemo` 数据面的基础类型和轻量 helper。
- 这里是 `chunk/`、`io/` 以及后续 LocalDiskChunkStore 共用的底座。

## 主要文件

- `store_types.h`：公开类型、枚举、结构体、helper 声明
- `store_types.cpp`：helper 实现
- `module-notes.md`：结构体、字段、函数语义说明

## 修改规则

- 新增类型、枚举、函数签名时改 `store_types.h`。
- 具体逻辑、校验、checksum、chunk_id 处理放 `store_types.cpp`。
- 不在这里引入 proto、RaftNode、MetadataStateMachine、KV fallback。
- 如果新增字段或 helper，记得同步维护 `module-notes.md`，让说明能直接对上头文件和 `.cpp` 函数。
