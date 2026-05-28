# modules/store/chunk

## 模块职责

- 本模块定义 chunk data-plane 的抽象接口。
- 当前核心是 `ChunkStore` 以及相关 request / response 结构。

## 主要文件

- `chunk_store.h`：接口和请求/响应结构
- `chunk_store.cpp`：当前仅保留虚析构实现
- `module-notes.md`：接口、字段和边界说明

## 修改规则

- 这里只定义抽象接口边界，不做本地文件 IO、durable publish 或索引实现。
- 请求/响应字段变更要同步维护 `module-notes.md`。
- 如果你在 `.cpp` 里几乎看不到函数，这是正常的：当前模块以接口声明为主。
