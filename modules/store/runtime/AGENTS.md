# modules/store/runtime

## 模块职责

- 本模块只维护 store data-plane 的有界执行器和有界任务队列。
- 当前核心是 `BoundedStorageExecutor`。

## 主要文件

- `storage_executor.h`：配置、请求/响应、统计结构、执行器声明
- `storage_executor.cpp`：worker loop、submit、shutdown、异常统计
- `module-notes.md`：执行器语义、停机边界和扩展点说明

## 修改规则

- 这里只做运行时调度基础设施，不引入 chunk/durable file 业务编排。
- 不在这里引入平台线程 API、RaftNode、proto 或 KV 路径。
- 队列必须保持有界；不要改成无界积压模型。
- 如果修改停机语义、异常处理、统计字段或任务承载类型，记得同步维护 `module-notes.md`。
- `Shutdown()` 和对象析构默认按 owner 线程模型使用，不要在 worker 回调里自停执行器。
