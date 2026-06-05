# T001 任务报告：StorageNode 模块边界说明补充

## 1. 修改了哪些文件

- `modules/store/upload/module-notes.md`
- `modules/store/placement/module-notes.md`
- `modules/store/node/module-notes.md`

## 2. 每个文件补充了什么边界

### `modules/store/upload/module-notes.md`

- 补充 008 阶段职责：upload 协调、WritePlan 执行衔接、chunk 写入结果收集、checksum 边界和 commit 前后 data-plane 协作事实。
- 明确 upload 不是 object manifest / metadata 权威。
- 明确 upload 不参与 Raft quorum、leader election、membership change。
- 明确禁止把完整 payload、chunk bytes 或整对象内容写入 metadata / Raft log / snapshot。
- 明确后续应向 bounded / streaming checksum 演进，避免整文件入内存。

### `modules/store/placement/module-notes.md`

- 补充 008 阶段职责：基于健康、容量、负载、failure domain、replica policy 做 placement。
- 明确可以消费 ViewNode / `StorageNodeRegistry` snapshot，但这些只是输入事实，不是权威。
- 明确 placement 结果最终服务于 MetadataNode 的 WritePlan / manifest，不负责对象提交或对象可见性。
- 明确禁止把 ViewNode 注册结果解释为 Raft voter membership。
- 明确保留 `excluded_nodes`、`reasons`、决策纪元等 decision reason 用于测试和诊断。

### `modules/store/node/module-notes.md`

- 补充 008 阶段职责：StorageNode 数据面写/读/删、checksum、durable publish、restart recovery、本地状态上报。
- 明确可以向 ViewNode 注册并发送 heartbeat。
- 明确对象可见性来自 Raft MetadataNode 的 COMMITTED manifest，不由 node 模块决定。
- 明确 node 模块不保存 object manifest 的一致性权威副本。
- 明确 node 模块不参与 Raft quorum、leader election、membership change。
- 明确 data-plane 返回 chunk / durability 事实，control-plane 决定 manifest、版本和可见性。

## 3. 是否发现不合理点 / 警告 / 风险

- 发现一个已知张力点：`modules/store/upload/module-notes.md` 当前实现说明里仍保留“未提供 `etag` 时把所有 chunk payload 拼接后计算对象级摘要”的现状，这与 008 的 bounded / streaming checksum 目标不完全一致。
- 本次没有改实现，只把边界写清楚。该风险已属于后续实现任务范围，不应在 T001 中通过文档伪装为已解决。

## 4. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`。
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`。
- 原因：本次只做模块职责边界补充，且上述 upload 内存边界风险已经在 008 的 `risk-register.md` 中有对应记录，不需要重复登记。

## 5. 验证命令和结果

### 验证命令

```bash
git diff -- modules/store/upload/module-notes.md modules/store/placement/module-notes.md modules/store/node/module-notes.md specs/008-integrated-object-storage-system/task-reports/t001-store-module-boundary-notes.md
```

### 验证结果

- diff 范围符合预期，只包含 3 个 `module-notes.md` 和本任务报告。
- 未修改业务代码、proto、CMake、测试。
- 本任务是纯文档边界收敛，不需要编译验证。

## 结论

- T001 已完成，结果满足“只补充 StorageNode 相关模块职责边界说明、不改业务逻辑”的要求。
- 从文档边界角度看，可以进入 T002。
