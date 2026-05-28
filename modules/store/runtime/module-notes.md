# store/runtime 说明

## 模块职责

`modules/store/runtime` 定义 store data-plane 的有界执行器和有界任务队列。

它负责：

- 固定 worker 数量
- 有界任务排队
- 队列满时返回 overloaded / rejected
- drain / cancel-pending 停机语义
- 任务异常统计和可观测性
- 为 timeout / cancellation 预留任务上下文

它不负责：

- ChunkIndex 业务逻辑
- durable file 读写
- LocalDiskChunkStore 编排
- StorageNodeService / Repair / Rebalance

## 文件对照

- `storage_executor.h`：配置、请求/响应、统计结构、执行器声明
- `storage_executor.cpp`：执行器实现、worker loop、提交/停机逻辑

## 主要结构体和类

### `StorageExecutorSubmitCode`

提交结果分类。

- `kAccepted`
- `kOverloaded`
- `kStopped`
- `kInvalidArgument`

### `StorageExecutorStopMode`

停机模式。

- `kDrain`：不再接收新任务，已入队任务继续执行
- `kCancelPending`：不再接收新任务，队列里尚未执行的任务直接丢弃

### `StorageTaskContext`

给后续 timeout / cancellation 扩展预留的任务上下文。

当前字段：

- `timeout_ms`
- `best_effort_cancel`

当前实现只承载这些字段，不做复杂取消传播。

### `StorageExecutorConfig`

执行器配置。

关键字段：

- `worker_count`
- `queue_capacity`

当前实现会把 `0` 修正为 `1`，避免非法配置产生未定义行为。

### `StorageExecutorSubmitRequest / SubmitResult`

描述一次任务提交。

特殊字段：

- `task_name`
- `context`
- `task`

`task` 当前使用 `std::function<void()>`，这意味着任务对象需要能放进可拷贝的函数包装；如果后续需要严格 move-only 任务，再单独演进接口。

### `StorageExecutorShutdownRequest / ShutdownResult`

描述一次执行器停机。

返回结果里会表达：

- 是否 stopped
- 是否 drained
- 丢弃了多少 pending task
- 是否因为错误的 shutdown 调用位置而被拒绝

### `StorageExecutorStats`

执行器统计快照。

当前可以看到：

- 是否还接收新任务
- 队列里还有多少任务
- 当前活跃 worker 数
- submitted / completed / rejected / failed / dropped 计数
- 最近一次 worker 异常摘要

### `BoundedStorageExecutor`

当前模块的具体执行器实现。

核心语义：

- `Submit(...)` 非阻塞
- 队列已满时立即返回 overloaded
- `Shutdown(kDrain)` 等待已入队任务完成
- `Shutdown(kCancelPending)` 丢弃未执行任务，但不会中断已经开始运行的任务
- worker 捕获任务异常并继续存活，不会因为单个任务抛异常而静默退出

## 当前停机边界

- 析构时默认按 `kDrain` 收口
- `Shutdown()` 之后不再接受新任务
- `Shutdown()` 需要由执行器 owner 线程触发，不应该从 worker 回调里自停
- 当前没有做“运行中任务的强制取消”，这部分要等后续业务层或 T020 之后的任务继续收紧

## 当前未实现内容

- 完整单元测试，留给 T020
- deadline 到点自动取消
- 任务级 future / 返回值聚合
- 更细粒度的优先级、速率限制和多队列调度
