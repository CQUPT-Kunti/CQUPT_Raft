# CQUPT_Raft 项目说明

禁止读.gitignore文件下面的文件夹里面的任何文件
禁止读 {
  vcpkg-configuration.json
  CQUPT_Raft_AI_Context.md
  README.md
  /deploy
}


这是一个基于 C++20、gRPC、Protobuf、GoogleTest 的 Raft KV 存储内核项目。

当前重点是 Raft 内核：选举、日志复制、提交应用、持久化、快照、落后节点追赶和重启恢复。暂时还不是完整的对外 KV 服务或分布式存储系统。



## 主要目录

```text
include/raft/   头文件和核心接口
src/            主要实现代码
proto/          Raft RPC 协议
tests/          GoogleTest 测试
config.txt      示例节点配置
test.sh         测试脚本
```

运行或测试会产生 `raft_data/`、`raft_snapshots/`、`build/tests/raft_test_data/` 等数据目录，不要当作源码修改。



## 构建

推荐低并发构建，避免占满 CPU/内存：

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel
```

更保守：

```bash
cmake --preset debug-ninja-safe
cmake --build --preset debug-ninja-safe
```

## 测试

推荐使用：

```bash
./test.sh
```

常用：

```bash
./test.sh --group unit
./test.sh --group persistence
./test.sh --group all
./test.sh --keep-data
```

测试并发建议保持低，例如 `CTEST_PARALLEL_LEVEL=1`。

## 启动示例

```bash
./build/raft_demo ./config.txt 1
./build/raft_demo ./config.txt 2
./build/raft_demo ./config.txt 3
```

如果 `config.txt` 只配置 `node.1`，就是单节点集群；配置多个 `node.<id>` 就是多节点集群。

## 项目流程

大致运行流程：

```text
读取配置
  -> 创建 RaftNode
  -> 加载持久化状态和 snapshot
  -> 启动 gRPC 服务、定时器、RPC 线程池
  -> 选举产生 leader
  -> client 调用 Propose 提交命令
  -> leader 写入本地日志
  -> leader 通过 AppendEntries 复制到 follower
  -> 多数派复制成功后推进 commit_index
  -> 已提交日志应用到 KvStateMachine
  -> 达到阈值后生成 snapshot 并压缩旧日志
```

重启恢复流程：

```text
读取 meta.bin 和 segment log
  -> 加载最新有效 snapshot
  -> replay snapshot 之后的 committed log
  -> 恢复 KV 状态机
  -> 重新参与选举和复制
```

落后 follower 追赶流程：

```text
leader 维护每个 follower 的 next_index / match_index
  -> 普通落后时批量发送 AppendEntries
  -> 日志已经被 leader 压缩时发送 InstallSnapshot
  -> snapshot 安装完成后继续复制后续日志
```

## 核心文件

- `src/main.cpp`：读取配置，启动一个 `RaftNode`。
- `src/raft_node.cpp`：Raft 核心逻辑，包括选举、RPC 处理、提交、应用、快照和恢复。
- `src/replicator.cpp`：每个 follower 的日志复制状态机。
- `src/raft_storage.cpp`：Raft term、vote、commit index 和 segment log 持久化。
- `src/snapshot_storage.cpp`：目录式 snapshot 保存、加载、校验和清理。
- `src/state_machine.cpp`：内存 KV 状态机，支持 SET、DEL 和 snapshot。
- `src/command.cpp`：命令序列化和反序列化。
- `proto/raft.proto`：RequestVote、AppendEntries、InstallSnapshot RPC 定义。

## 重要约定

- 不要轻易修改命令格式：`SET|key|value`、`DEL|key|`。
- 尽量保持已有函数名、变量名和整体结构。
- 优先使用 C++20 标准库，保持跨平台。
- 测试使用 GoogleTest。
- 测试数据放在 `build/tests/raft_test_data/`，不要放到 `/tmp`。
- 修改 Raft 行为时，优先补充或更新对应测试。
- 不要提交 build 产物、生成的 protobuf 文件、运行数据或 snapshot 数据。
