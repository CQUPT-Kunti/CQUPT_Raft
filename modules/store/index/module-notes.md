# store/index 说明

## 模块职责

`modules/store/index` 定义本地 chunk 索引接口和 sharded map 结构。

它负责：

- 本地 `ChunkIndexEntry` 的内存索引
- insert / update / find / list / remove 接口边界
- 按 `ChunkState` 过滤和分页扩展点
- 为后续并发扩展预留 shard 结构

它不负责：

- 文件系统扫描
- durable publish
- LocalDiskChunkStore 业务编排
- per-chunk lock 逻辑
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
- `default_page_size`
- `max_page_size`

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

当前实现重点是按 `chunk_id` 计算 shard，并把 `lock_shard` 回填到 `ChunkIndexEntry`，方便后续 T018 接 per-chunk lock / lock striping。

## 当前 `.cpp` 里实现了哪些函数

- `ChunkIndex::~ChunkIndex()`
- `ShardedChunkIndex::ShardedChunkIndex(...)`
- `ShardedChunkIndex::~ShardedChunkIndex()`
- `ShardedChunkIndex::Insert(...)`
- `ShardedChunkIndex::Update(...)`
- `ShardedChunkIndex::Find(...)`
- `ShardedChunkIndex::Remove(...)`
- `ShardedChunkIndex::List(...)`
- `ShardedChunkIndex::config()`

## 当前 list 语义

- 结果按 `chunk_id` 字典序输出
- `page_token` 当前表示“上一页最后一个 `chunk_id`”
- `snapshot_epoch` 只是返回当前 mutation 代次，占位给后续更强的一致性分页语义

T017 已用单线程测试钉住以上 list 语义，但当前实现还没有做并发修改下的稳定分页保证；后续需要在 T018 及其后续任务里继续收紧。
