# T011 Extend Identity Data Model For Node Type, Optional `raft_id`, Membership State, And Persistent Generation

## 做了什么

本任务实现了 `modules/cluster/node_identity.*` 的数据模型扩展，使 `node.identity` 能真实表达 009 阶段需要的三类长期身份边界：

- StorageNode identity
- ViewNode identity
- MetadataNode identity
  - bootstrap voter
  - dynamic join candidate

本次只扩展 durable identity 数据模型、序列化/反序列化和校验规则，不实现：

- Metadata learner join
- app 启动 wiring
- ViewNode self refresh
- StorageNode heartbeat / dynamic join
- Raft membership change / quorum 变更

## 修改了哪些文件

- `modules/cluster/node_identity.h`
- `modules/cluster/node_identity.cpp`
- `tests/node_identity_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t011-extend-identity-data-model-for-node-type-optional-raft-id-membership-state-and-persistent-generation.md`

## NodeIdentity 新增或确认了哪些字段

### 已确认 / 保持

- `cluster_id`
- `node_type`
- `node_id`
- optional `raft_id`
- `identity_version`
- `created_at_unix_ms`
- `source`

### 本任务新增 / 收口

- `membership_state`
  - 新增 `NodeIdentityMembershipState`
  - 支持：
    - `non_raft`
    - `joining`
    - `candidate`
    - `learner`
    - `voter`
- `persistent_generation`
  - 新增本地持久代际字段
  - 新写入 identity 默认写成 `1`

### 版本兼容

- `kNodeIdentityCurrentVersion` 从 `1` 升到 `2`
- 新实现同时支持读取：
  - `identity_version=1`
  - `identity_version=2`

## StorageNode / ViewNode / Metadata bootstrap voter / Metadata dynamic join candidate 的身份语义

### StorageNode

- `node_type=storage`
- `raft_id` 不允许存在
- `membership_state=non_raft`
- `node_id` 是长期本地 durable identity
- 首次缺失 identity file 时允许创建
- 重启时必须复用既有 durable identity

### ViewNode

- `node_type=view`
- `raft_id` 不允许存在
- `membership_state=non_raft`
- ViewNode 仍不是 identity authority，也不是 membership authority
- durable identity 只表达本地长期身份，不表达 voter/learner authority

### Metadata bootstrap voter

- `node_type=metadata`
- `raft_id` 必须是正值
- `membership_state=voter`
- 当前默认通过 `source=config_generator` 推断 bootstrap voter 语义
- 本地文件可以持久化 bootstrap voter 身份，但这仍然只是“初始配置生成的 durable identity”，不是把 ViewNode 变成 authority

### Metadata dynamic join candidate

- `node_type=metadata`
- `membership_state` 可以是 `joining` 或 `candidate`
- `raft_id` 现在允许为空，也允许携带 provisional 正值
- `source=explicit_override` 的 metadata identity 不能持久化成 `voter`
- 这保证了 dynamic join candidate 不能靠本地文件直接把自己变成 voter

说明：

- 本任务里使用的等价状态映射是：
  - non-Raft node -> `non_raft`
  - dynamic join initial state -> `candidate` / `joining`
  - committed learner -> `learner`
  - committed voter -> `voter`

## 是否兼容旧 identity 文件

兼容。

兼容策略：

- 旧 `v1` identity 文件不要求包含：
  - `membership_state`
  - `persistent_generation`
- 加载旧文件时：
  - non-metadata 节点默认推断为 `membership_state=non_raft`
  - metadata + `source=config_generator` + 正 `raft_id` 默认推断为 `membership_state=voter`
  - `persistent_generation` 默认补成 `1`
- 新写入 identity 文件使用 `v2`，会显式写出：
  - `membership_state=...`
  - `persistent_generation=1`

本任务新增了 `T011LegacyIdentityFileWithoutMembershipStateAndPersistentGenerationLoadsCompatibly`，验证旧 `v1` metadata bootstrap identity 可被兼容加载。

## T006-T010 中仍暴露的后续 T012 / T013 / T016 缺口

仍然存在的后续缺口：

- `T012`
  - atomic publish / restart validation 还需要更进一步的专门覆盖与说明
  - 当前 durable publish 逻辑仍在，但 T011 主要做的是数据模型，不是 publish 语义扩展
- `T013`
  - process incarnation / boot epoch 仍未进入 `node.identity`
  - 这符合设计，因为 incarnation 不是长期 durable identity 字段
  - 但相关生成与测试仍需要后续任务补齐
- `T016`
  - Metadata app 还没有真正分出 bootstrap voter 与 dynamic join candidate 启动路径
  - 当前只是 durable identity 数据模型已经能表达两者
  - 还没有实际 app wiring 去消费 `membership_state`

另外：

- `learner` / `voter` 的 committed membership authority 还没有接入 Raft runtime
- 当前只是 durable identity model 可以表达这些状态，不代表运行时已经支持 learner promote 或 membership change

## 构建和测试命令

构建：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_node_identity
) 9>/tmp/cqupt_raft_build.lock
```

测试：

```bash
ctest --preset debug-tests -R "NodeIdentityTest\\." --output-on-failure
```

## 结果

- build: PASS
- test: PASS
- `NodeIdentityTest`: 25/25 passed

日志：

- `tmp/test-logs/t011-build.log`
- `tmp/test-logs/t011-ctest-debug-tests.log`

补充说明：

- 当前仓库不存在 `ctest --preset debug-ninja-low-parallel` 这个 test preset
- 因此测试阶段使用仓库实际存在的 `debug-tests` preset
- 本任务没有额外跑 `cluster_config_test`
  - 原因：T011 代码修改集中在 `node_identity.*`
  - 本轮验证优先遵守“只跑 identity 相关 targeted build/test，不默认扩大全量构建”

## 是否可以进入 T012

可以。

T011 已经把 durable identity 数据模型扩展到能够表达：

- node type
- optional `raft_id`
- membership state
- persistent generation
- v1/v2 identity file compatibility

并且没有破坏 T006-T010 的 identity 测试边界。下一步可以进入 `T012`，继续补 atomic first-start identity creation 和 restart validation 的实现/验证。 
