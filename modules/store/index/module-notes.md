# store/index 说明

## 模块职责

`modules/store/index` 定义本地 chunk 索引接口和 sharded map 结构。

它负责：

- 本地 `ChunkIndexEntry` 的内存索引
- insert / update / find / list / remove 接口边界
- per-chunk lock 和 lock striping
- 按 `ChunkState` 过滤和分页扩展点
- 为后续并发扩展预留 shard 结构

它不负责：

- 文件系统扫描
- durable publish
- LocalDiskChunkStore 业务编排
- object committed/deleted 可见性判断

## 文件对照

- `chunk_index.h`：接口、返回结构、分页参数、`ShardedChunkIndex` 声明
- `chunk_index.cpp`：基础内存实现

## 主要结构体和类

### `ChunkIndexResult`

索引操作的通用返回基类。

### `ChunkIndexListOptions`

分页和过滤参数。

关键字段：

- `state_filter`
- `prefix_filter`
- `page_token`
- `page_size`
- `include_quarantine`

### `ChunkIndexConfig`

sharded map 配置。

关键字段：

- `shard_count`
- `lock_stripe_count`
- `default_page_size`
- `max_page_size`

### `ChunkLockGuard / ChunkIndexLockResponse`

`ChunkLockGuard` 是面向上层 chunk 操作的 RAII guard。

- `AcquireChunkLock(chunk_id)` 成功后返回已持有的 guard
- guard 析构或被 move 覆盖后自动释放
- guard 不可拷贝，只能 move
- 同一 `chunk_id` 会稳定映射到同一条 stripe
- guard 只负责 chunk 级冲突串行，不负责 list 快照一致性

### `ChunkIndexInsertResponse / UpdateResponse / FindResponse / RemoveResponse / ListResponse`

分别对应 5 个基础索引操作的返回语义。

其中：

- `Insert` 重复插入返回 `kAlreadyExists`
- `Update` 缺失项返回 `kNotFound`
- `Find` / `Remove` 缺失项返回 `kNotFound`
- `List` 返回受 `page_size` 约束的有界结果

### `ChunkIndex`

本地 chunk 索引抽象接口。

### `ShardedChunkIndex`

基于 shard 划分的内存实现。

当前实现有两层并发边界：

- shard 级读写锁：保护 `unordered_map` 本身，避免并发访问容器时出现数据竞争
- chunk 级 striped mutex：按 `chunk_id` hash 串行化同一个 chunk 的冲突操作

`lock_shard` 仍会回填到 `ChunkIndexEntry`，供后续更细粒度并发扩展复用。

## 当前 `.cpp` 里实现了哪些函数

- `ChunkIndex::~ChunkIndex()`
- `ShardedChunkIndex::ShardedChunkIndex(...)`
- `ShardedChunkIndex::~ShardedChunkIndex()`
- `ShardedChunkIndex::Insert(...)`
- `ShardedChunkIndex::Update(...)`
- `ShardedChunkIndex::Find(...)`
- `ShardedChunkIndex::Remove(...)`
- `ShardedChunkIndex::List(...)`
- `ShardedChunkIndex::AcquireChunkLock(...)`
- `ShardedChunkIndex::config()`

## 当前 list 语义

- 结果按 `chunk_id` 字典序输出
- `page_token` 当前表示“上一页最后一个 `chunk_id`”
- `snapshot_epoch` 只是返回当前 mutation 代次，占位给后续更强的一致性分页语义

T017 已用单线程测试钉住以上 list 语义。T018 新增了 chunk 级串行化和 shard 级容器保护，但当前实现仍然没有做并发修改下的稳定分页保证；后续需要继续收紧 page token / snapshot 语义。

## 当前锁语义

- `AcquireChunkLock()` 失败时返回明确错误，不返回空转成功
- 同一 chunk 的 write / delete / repair / rebalance 等冲突路径应复用同一个 guard 入口
- 不同 chunk 即使共享同一个 shard，也可以通过不同 stripe 获得更高并行度
- 这些 guard 不是递归锁；同一线程重复获取同一 chunk 锁会阻塞自己
- `Insert / Update / Find / Remove / List` 现在具备基础容器级线程安全，但跨步骤原子性仍需要上层显式持有 chunk guard
