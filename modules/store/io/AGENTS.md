# modules/store/io

## 模块职责

- 本模块定义 durable file 抽象接口，并承接平台实现。
- 当前已经有 Linux 路径；Windows 路径仍留待后续任务实现。

## 主要文件

- `durable_file.h`：durable file 类型、请求/响应、接口声明
- `durable_file.cpp`：错误映射、Linux durable file 实现
- `module-notes.md`：结构体、函数和平台边界说明

## 修改规则

- 公共契约和接口放 `durable_file.h`。
- 平台实现、errno 映射、路径处理放 `durable_file.cpp`。
- required durability operation 不能 silent no-op success。
- 修改或新增公开函数后，要同步维护 `module-notes.md`，写清楚头文件声明对应哪个 `.cpp` 实现。
