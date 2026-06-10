# modules/cluster

## 目录职责

- `modules/cluster/` 是 008 阶段为统一 cluster/config/identity 边界预留的公共基础目录。
- 这里负责：
  - `ClusterConfig` 结构说明
  - 配置生成、加载、校验边界
  - `NodeIdentity` / `node.identity` 的持久身份边界
  - 初始 Raft membership 的配置生成边界
  - Linux / Windows 路径与 durability contract 说明

## 不负责

- 不实现 Raft 共识、leader election、log replication、commit quorum。
- 不直接修改已经提交的 Raft membership。
- 不决定对象是否 COMMITTED 可见。
- 不保存 object manifest 的一致性权威副本。
- 不处理真实 chunk payload。
- 不替代 ViewNode 的服务发现与节点观测。
- 不替代 StorageNode 的 chunk 落盘、publish、scrub、repair 或 restart recovery 逻辑。

## 修改入口

- 修改本目录前，先读根 `AGENTS.md`。
- 再读本文件。
- 然后读 `module-notes.md` 和直接相关的 spec / plan / data-model / contract 文档。

## 修改规则

- cluster/config/identity 边界必须保持清晰，不把 Raft、ViewNode、StorageNode 的业务逻辑混进来。
- 新增关键结构、字段、特殊参数、平台差异说明时，必须同步更新 `module-notes.md`。
- 配置和身份逻辑必须强调可测试、可诊断、跨平台，不允许依赖隐式默认值掩盖错误。
- identity 与 durability contract 的说明必须明确 Linux / Windows 差异，禁止静默降级。
- 不在该目录写任务执行日志、调试流水账或临时结论。

## 重点关注

- endpoint 唯一性
- `data_dir` 冲突和路径合法性
- `node.identity` 首次创建、重启复用、mismatch 诊断
- 初始 1/3/5/7 MetadataNode voter 配置
- atomic publish、flush、directory durability 的平台差异

## 相关文档

- `specs/008-integrated-object-storage-system/spec.md`
- `specs/008-integrated-object-storage-system/plan.md`
- `specs/008-integrated-object-storage-system/data-model.md`
- `specs/008-integrated-object-storage-system/contracts/cluster-config.md`
