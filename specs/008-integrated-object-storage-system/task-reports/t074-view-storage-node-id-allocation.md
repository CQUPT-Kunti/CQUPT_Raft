# T074 View StorageNode ID Allocation

## 1. 修改了哪些文件

- `modules/view/view_service_impl.cpp`
- `modules/view/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t074-view-storage-node-id-allocation.md`

## 2. ViewNode StorageNode first registration / node_id allocation path 做了什么

- 在 `ViewNodeServiceImpl::RegisterNode()` 进入 registry 之前，新增了
  StorageNode first registration 的 node_id 解析路径。
- 当 `node_type == Storage` 且 `node_id` 为空时：
  - 先按 `cluster_id + data_dir_fingerprint` 查询当前 ViewNode 已观测到的 StorageNode
    快照。
  - 如果同一 fingerprint 已有唯一匹配记录，则复用并确认该稳定 `node_id`。
  - 如果没有匹配记录，则基于 `cluster_id + data_dir_fingerprint` 生成确定性的
    `store-<hex>` node_id，并把它作为首次注册的 service-side allocation 结果。
- 当 `node_id` 非空但 fingerprint 已经映射到另一个 StorageNode 时，返回可诊断冲突。
- 该路径只补 service adapter 的注册边界，不改 proto，不改 ViewNode registry 的基础
  注册/心跳/发现逻辑。

## 3. 如何保持 ViewNode non-authority 边界

- 分配/确认的只有 StorageNode discovery identity，不是 Raft membership identity。
- 实现只修改 `RegisterNode` 适配路径，不触碰 quorum、election、membership、commit。
- ViewNode 仍只输出 summary / snapshot / warning 这类 observation 结果。
- 没有引入 object manifest、对象可见性或 chunk payload 的任何 authority 逻辑。

## 4. 是否发现不合理点 / 警告 / 风险

- 当前 proto 没有显式的“allocate node_id”请求开关，因此 service adapter 只能通过
  `StorageNode + empty node_id` 这一输入形态推断 first registration。
- 当前实现把确定性 node_id 生成规则收敛在 `view_service_impl.cpp`。如果后续需要让 app
  或 cluster identity 侧也共享同一分配算法，最好再抽到公共 helper，但这不属于本任务。
- 当前实现依赖 `data_dir_fingerprint` 作为首次分配和后续确认的稳定依据；如果调用方不提供
  fingerprint，service 会明确拒绝空 `node_id` 的首次注册。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `risk-register.md`。
- `common-risk-notes.md` 在当前 feature 目录下不存在，因此未修改。

## 6. 验证命令和结果

### diff 检查

```bash
git diff -- modules/view/view_service_impl.cpp \
  modules/view/view_service_impl.h \
  modules/view/view_registry.h \
  modules/view/view_registry.cpp \
  modules/view/module-notes.md \
  specs/008-integrated-object-storage-system/tasks.md \
  specs/008-integrated-object-storage-system/task-reports/t074-view-storage-node-id-allocation.md
```

- 结果：已执行。
- 说明：本次 diff 只包含 `view_service_impl.cpp` 的 service adapter 实现、最小
  `module-notes.md` 同步、`T074` 勾选和任务报告。
- 说明：`tasks.md` 邻近区域中的 `T075` 状态变更是当前工作区既有差异，不是本任务新增修改。

### 最小构建验证

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_view_node_discovery' \
  || echo "build lock busy, skip test_view_node_discovery build in this window"
```

- 结果：PASS。
- 命令成功完成 `debug-ninja-safe` configure，并编译通过 `test_view_node_discovery`。

### 相关测试

为保持最小验证，直接运行已编译测试二进制：

```bash
flock -n /tmp/cqupt_raft_build.lock -c './build/linux/safe/tests/test_view_node_discovery --gtest_brief=1' \
  || echo "build/test lock busy, skip test_view_node_discovery binary in this window"
```

- 结果：PASS。
- 测试命令：`./build/linux/safe/tests/test_view_node_discovery --gtest_brief=1`
- 通过情况：`11 tests from 1 test suite ran. (39 ms total)`，`11 tests passed`。
- 说明：现有测试主要覆盖 discovery/service 基础路径和 heartbeat/liveness 行为，没有单独新增 “empty node_id first registration” 专项 case；本任务按约束未修改测试文件，因此这里以最小相关 target 编译和已有服务测试通过作为回归验证。
