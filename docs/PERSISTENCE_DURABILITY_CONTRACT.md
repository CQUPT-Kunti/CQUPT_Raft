# PERSISTENCE_DURABILITY_CONTRACT

## 1. 标题与范围

本文定义当前 CQUPT_Raft 项目在 Phase 1 的持久化 durability contract。

- 本文只定义 contract、现状和后续改进边界，不修改代码。
- 本文不变更 Raft 协议、KV 逻辑、持久化格式或公共 API。
- 本文覆盖以下持久化对象：
  - `meta.bin`
  - `log/segment_*.log`
  - `snapshot_<index>/data.bin`
  - `snapshot_<index>/__raft_snapshot_meta`

本文关注的故障模型：

- 正常退出
- 进程 crash
- power loss
- restart recovery

## 2. 当前持久化对象与磁盘布局

当前持久化布局分成两条主线：

- Raft hard state 和 log 位于 `NodeConfig::data_dir`
- state machine snapshot 位于 `snapshotConfig::snapshot_dir`

### 2.1 Raft state 布局

```text
NodeConfig::data_dir/
  meta.bin
  meta.bin.tmp
  log/
    segment_00000000000000000001.log
    segment_00000000000000000513.log
  log.tmp/
  log.bak/
```

其中：

- `meta.bin` 保存 `current_term`、`voted_for`、`commit_index`、`last_applied`、`first_log_index`、`last_log_index`、`log_count`
- `log/segment_*.log` 保存实际 log records
- `meta.bin.tmp`、`log.tmp/`、`log.bak/` 是当前实现中的中间发布工件

### 2.2 Snapshot 布局

```text
snapshotConfig::snapshot_dir/
  snapshot_00000000000000000120/
    data.bin
    __raft_snapshot_meta
  snapshot_00000000000000000150/
    data.bin
    __raft_snapshot_meta
  install_snapshot_node_<id>.bin.tmp
  snapshot_work_node_<id>.bin
  <state_machine_tmp>.tmp
```

其中：

- `snapshot_<index>/data.bin` 是状态机 snapshot 数据文件
- `snapshot_<index>/__raft_snapshot_meta` 保存 snapshot header、`last_included_index`、`last_included_term`、`created_unix_ms`、checksum 和 data file name
- `install_snapshot_node_<id>.bin.tmp` 是 follower 接收远端 snapshot 时的临时文件
- `snapshot_work_node_<id>.bin` 是本地 snapshot worker 输出给 snapshot storage 的中间文件
- `*.tmp` 来自当前状态机 `SaveSnapshot()` 的临时输出文件

## 3. 每类对象的写入路径

### 3.1 Hard state / log

主路径：

- `RaftNode::PersistStateLocked()`
- `IRaftStorage::Save()`
- `FileRaftStorage::Save()`

当前 `PersistStateLocked()` 会把以下状态作为一个持久化单元交给 storage：

- `current_term`
- `voted_for`
- `commit_index`
- `last_applied`
- `log`

当前 `FileRaftStorage::Save()` 的发布顺序是：

1. `create_directories(data_dir)`
2. 删除旧 `log.tmp/`
3. 创建新 `log.tmp/`
4. `WriteSegments(log.tmp/, state.log)`
5. `WriteMeta(meta.bin.tmp, state)`
6. `ReplaceDirectory(log.tmp/, log/, log.bak/)`
7. `ReplaceFile(meta.bin.tmp, meta.bin)`

当前实现中，hard state 与 log 并不是两个独立 durability contract，而是通过 `meta.bin` 和 `log/` 的组合一起构成可信 Raft state。

### 3.2 本地 snapshot 生成

主路径：

- `RaftNode::SnapshotWorkerLoop()`
- `state_machine_->SaveSnapshot()`
- `FileSnapshotStorage::SaveSnapshotFile()`
- `CompactLogPrefixLocked()`
- `PersistStateLocked()`

当前顺序是：

1. snapshot worker 读取当前 `last_applied_` 和对应 term
2. `state_machine_->SaveSnapshot(temp_path)` 写状态机 snapshot 文件
3. `FileSnapshotStorage::SaveSnapshotFile(temp_path, snapshot_index, snapshot_term, ...)`
4. snapshot 保存成功后，`CompactLogPrefixLocked(snapshot_index, snapshot_term)`
5. compact 后再 `PersistStateLocked()`
6. 最后 `PruneSnapshots()`

这意味着当前本地 snapshot publish 与 log compaction publish 并不是单事务原子提交。

### 3.3 Follower 安装 snapshot

主路径：

- `RaftNode::OnInstallSnapshot()`
- `std::ofstream(temp_path)`
- `FileSnapshotStorage::SaveSnapshotFile()`
- `state_machine_->LoadSnapshot()`
- `CompactLogPrefixLocked()`
- `PersistStateLocked()`

当前顺序是：

1. 把 RPC 中的 snapshot bytes 写入 `install_snapshot_node_<id>.bin.tmp`
2. 调用 `SaveSnapshotFile()` 发布为 `snapshot_<index>/data.bin + __raft_snapshot_meta`
3. `LoadSnapshot()` 把已发布 snapshot 导入状态机
4. `CompactLogPrefixLocked(last_included_index, last_included_term)`
5. 更新 `commit_index_`、`last_applied_`
6. `PersistStateLocked()`
7. `PruneSnapshots()`

### 3.4 启动恢复

主路径：

- `storage_->Load()`
- `LoadLatestSnapshotOnStartup()`
- `ApplyCommittedEntries()`

当前启动顺序是：

1. `FileRaftStorage::Load()` 先读取 `meta.bin + log/`
2. 若启用 `load_on_startup`，`LoadLatestSnapshotOnStartup()` 枚举有效 snapshot，按最新有效优先尝试加载
3. snapshot 加载成功后，`CompactLogPrefixLocked()` 把内存 log 恢复到 snapshot boundary
4. 若 `commit_index_ > last_applied_`，通过 `ApplyCommittedEntries()` 重放 committed tail log

因此当前 restart recovery 的顺序是：

- 先恢复 persisted meta/log
- 再恢复 snapshot
- 再 replay committed tail log

## 4. 当前缺失 fsync 的位置

当前实现没有显式调用 `fsync`、`fdatasync` 或平台等价 durability barrier。

明确缺口包括：

- `raft_storage.cpp::WriteMeta()`
- `raft_storage.cpp::WriteSegments()`
- `snapshot_storage.cpp::CopyFilePortable()`
- `snapshot_storage.cpp::WriteMeta()`
- 状态机 `SaveSnapshot()` 数据文件写入路径
- `raft_node.cpp::OnInstallSnapshot()` 中 install-snapshot temp file 写入
- `raft_storage.cpp::ReadSegmentFile()` 检测坏尾部后调用 `resize_file` 截断，但没有额外 durability barrier

当前只有：

- `ofstream::flush()`
- `std::filesystem::rename()`
- `std::filesystem::remove()/remove_all()`

这些操作只能说明用户态缓冲区和目录项更新流程被请求执行，不能直接等价为 power-loss 级 durability。

## 5. 当前缺失 directory fsync 的位置

当前实现也没有对父目录做 durability publish。

明确缺口包括：

- `data_dir` 创建后没有 directory `fsync`
- `log.tmp/` 创建后没有 directory `fsync`
- `log/ -> log.bak/` rename 后没有 directory `fsync`
- `log.tmp/ -> log/` rename 后没有 directory `fsync`
- `meta.bin.tmp -> meta.bin` rename 后没有 `data_dir` directory `fsync`
- `snapshot_dir` 创建后没有 directory `fsync`
- `snapshot_<index>/` 创建后没有 directory `fsync`
- `data.bin` 和 `__raft_snapshot_meta` 发布后没有 `snapshot_<index>` 或其父目录 `fsync`
- `PruneSnapshots()` 删除 snapshot 目录后没有 directory `fsync`
- 状态机 `SaveSnapshot()` 的 `*.tmp -> final` rename 后没有父目录 `fsync`

这意味着即使 rename 成功返回，目录项在 power loss 后是否可见仍依赖底层文件系统语义。

## 6. 当前 publish ordering 风险

### 6.1 Log / meta 发布窗口

当前 `FileRaftStorage::Save()` 先替换 `log/`，再替换 `meta.bin`。

风险：

- 如果进程在 `log.tmp -> log/` 成功后、`meta.bin.tmp -> meta.bin` 之前 crash，磁盘上可能出现“新 log + 旧 meta”
- 当前恢复依赖 `meta.bin` 的 boundary 和 count 来约束 `log/` 的可见范围，因此这类状态通常倾向于回退到旧 meta 所描述的边界
- 但在缺少 directory durability 的前提下，最终可见状态仍可能受文件系统实现影响

### 6.2 Snapshot 直接写入最终目录

当前 `FileSnapshotStorage::SaveSnapshotFile()` 不创建 temp snapshot dir，而是直接：

1. 删除旧 `snapshot_<index>/`
2. 创建最终 `snapshot_<index>/`
3. 复制 `data.bin`
4. 写 `__raft_snapshot_meta`

风险：

- 没有单独的“snapshot publish point”
- crash/power loss 时，最终目录下可能出现只写了一半的数据文件，或者 `data.bin` 已存在而 meta 尚未完整发布
- 当前恢复通过 meta header、目录名、data 存在性、checksum 来过滤不可信 snapshot，但这不是原子发布

### 6.3 State machine snapshot rename 风险

当前状态机 `SaveSnapshot()` 使用：

- `*.tmp` 写入
- 删除旧 final file
- `rename(temp, final)`

风险：

- 文件级 rename 没有配套父目录 durability
- final file 在 crash/power loss 后是否一定可见，不由当前代码保证

### 6.4 Snapshot 与 log compaction 非原子

本地 snapshot worker 的顺序是：

1. 发布 snapshot
2. 再 compact log 并持久化新的 `meta.bin + log/`

风险：

- crash 可能发生在 snapshot 已发布但 log compaction 尚未 durable 之前
- 也可能发生在 log compacted 的持久化调用过程中
- 当前恢复路径可以容忍“新 snapshot + 旧 log boundary”并通过 replay 恢复，但不能把这表述成严格原子 snapshot publish

## 7. 目标保证矩阵

| 场景 | 当前阶段 contract 目标 | 当前实现可证明程度 |
| --- | --- | --- |
| 正常退出 | 最后一次成功返回的持久化状态在 restart recovery 中应可恢复 | 仅 best-effort，可依赖现有恢复逻辑和测试证明重启恢复主路径 |
| 进程 crash | 不接受未完成发布对象为可信状态；允许回退到最后一个完整状态 | 部分可证明，主要依赖 meta 边界、segment checksum/tail truncate、snapshot checksum 过滤 |
| power loss | 不把 acked write 解释为必然 durable | 明确未严格保证 |

当前阶段必须明确：

- “正常退出可恢复”不等于 “power-loss-safe”
- 当前实现的 durability 语义不能表述为严格的 fsync 级承诺
- 当前 restart recovery 目标是“识别并跳过不可信对象，尽量回退到最近完整状态”

## 8. Log append / truncate durability 语义

### 8.1 规范目标

当前阶段定义的规范目标是：

- append 成功时，对应 log 内容与 `meta.bin` 描述的边界应要么一起成为可信状态，要么一起不成为可信状态
- truncate/compaction 成功时，新的 `log/` 与新的 `meta.bin` 应共同定义新的可信边界

### 8.2 当前实现现状

当前实现通过两层机制降低 restart 时误信任坏数据的风险：

- `meta.bin` 保存 `first_log_index`、`last_log_index`、`log_count`，恢复只接受满足这些边界的 segment 集合
- 每条 segment entry 有 checksum，读取坏尾部时会对 segment 做 tail truncate

但当前实现仍然缺少：

- file data fsync
- directory fsync
- log publish 与 meta publish 的强原子性

因此当前现状只能定义为：

- append / truncate 是“可恢复型” durability 语义
- 不是 power-loss 级严格 durability 语义

### 8.3 当前 truncate/compaction 语义

当前 compaction 不是就地删除前缀，而是：

- 重新写 `log.tmp/`
- 用 `ReplaceDirectory()` 替换 `log/`
- 再写入新 `meta.bin`

这能减少部分部分写入问题，但仍缺少目录 durability barrier。

## 9. Hard state durability 语义

当前 hard state 包括：

- `current_term`
- `voted_for`
- `commit_index`
- `last_applied`

当前阶段的 contract 是：

- hard state 不能脱离当前 `log/` 单独被信任
- 可信 raft state 必须同时满足：
  - `meta.bin` 可完整解析
  - header magic/version 正确
  - `first_log_index`、`last_log_index`、`log_count` 与 segment 实际内容一致
  - segment indexes 连续

因此 restart recovery 时，可信状态判断不是“meta 文件存在即可”，而是“meta 和 segment 集合共同满足边界约束”。

## 10. Snapshot publish durability 语义

当前恢复只信任同时满足以下条件的 snapshot：

- snapshot meta header 正确
- snapshot meta version 正确
- snapshot 目录名中的 index 与 meta 中的 `last_included_index` 一致
- `data.bin` 存在
- `data.bin` checksum 与 meta 中记录一致

当前恢复策略是：

- `ListSnapshots()` 按 `last_included_index` 降序、`created_unix_ms` 降序排序
- `LoadLatestSnapshotOnStartup()` 逐个尝试加载
- 最新 snapshot 不可信时，允许回退到更旧 snapshot

当前阶段必须明确：

- snapshot publish 还不是 power-loss 下的原子发布
- 当前实现保证的是“恢复阶段尽量识别无效 snapshot 并回退”
- 不是“已返回成功的 snapshot 在 power loss 后必然完整存在”

## 11. Restart recovery 时如何判断可信状态

### 11.1 Meta / log 可信条件

恢复只信任满足以下条件的 persisted raft state：

- `meta.bin` 可打开并完整读取
- meta header magic/version 正确
- `log/` 目录存在并可遍历
- segment files 按 start index 排序后能构造出：
  - 与 `log_count` 一致的记录数量
  - 与 `first_log_index` / `last_log_index` 一致的边界
  - 连续无洞的 index 序列

### 11.2 Segment 坏尾部处理

当前 `ReadSegmentFile()` 对以下情况会截断坏尾部：

- header 不完整
- magic/version/type 非法
- `data_size` 异常
- entry payload 读失败
- checksum mismatch

但 contract 还要明确：

- 坏尾部可被截断，不代表整个 persisted state 一定可接受
- 截断后仍必须满足 `meta.bin` 声明的总条数和边界
- 若最终条数不匹配，则整体恢复失败

### 11.3 Snapshot 可信条件

snapshot 可信条件是：

- 最新有效优先
- 无效 snapshot 跳过
- 可回退到更旧 snapshot

### 11.4 恢复顺序

恢复顺序明确为：

1. 读取 persisted meta/log
2. 选取最新有效 snapshot 覆盖状态机状态
3. 把 log 恢复到 snapshot boundary
4. replay committed tail log

## 12. 当前阶段必须保证什么

Phase 1 必须保证以下事情被文档化并固定下来：

- 持久化对象和磁盘布局被明确列出
- 每类对象的写入路径和恢复路径被明确列出
- 当前缺失 `fsync` 和 directory `fsync` 的地方被明确标记
- 当前 publish ordering 风险被明确标记
- restart recovery 可信状态判定规则被明确标记
- “当前实现行为”和“后续阶段要收敛的 contract”被明确区分

Phase 1 不是实现 durability 加固，而是把 durability 边界说清楚。

## 13. 当前阶段暂时不保证什么

当前阶段明确不保证：

- power loss 后保留最后一次成功写入
- directory entry durable publish
- snapshot 发布原子性
- prune 删除后的严格 crash consistency
- fault injection / failure injection 覆盖完整
- 任意文件系统上的一致 durability 语义

这些内容应留给后续 Phase 2-6 逐步收敛。

## 14. 后续阶段需要补哪些测试

后续 durability 加固后，至少需要补以下测试场景：

- `meta.bin.tmp -> meta.bin` rename 前 crash
- `meta.bin.tmp -> meta.bin` rename 后、directory durable 前 crash
- `log.tmp/ -> log/` 切换窗口 crash
- `log/ -> log.bak/` 与 `log.tmp/ -> log/` 之间 crash
- segment 尾部 torn write
- segment tail truncate 后 restart recovery 行为
- snapshot `data.bin` 已写、`__raft_snapshot_meta` 未写
- snapshot meta 已写、目录项未 durable
- snapshot publish 完成但 log compaction 尚未 durable
- prune 旧 snapshot 中途 crash
- 启动恢复时 trusted-state 判定和诊断输出测试

## 15. 当前阶段结论

当前 CQUPT_Raft 已具备：

- `meta.bin + segment log + snapshot directory` 的基础持久化布局
- restart recovery 主路径
- snapshot checksum 过滤
- segment checksum 和坏尾部截断能力

但当前 durability 语义仍然是：

- restart-recoverable 优先
- best-effort durability
- 非 power-loss-safe

因此 Phase 1 的正式结论是：

- 当前代码可以支撑“定义并文档化 durability contract”
- 当前代码还不能声明“已经提供严格 fsync 级 durability 保证”
- 后续 Phase 2-6 应分别补齐 log、meta、snapshot、recovery diagnostics 和 failure injection 语义
