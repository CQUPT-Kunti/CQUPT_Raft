# T026 执行报告

## 任务范围

- 任务编号：`T026`
- 任务目标：更新 `client-design.md`，记录 Metadata Client 在 leader failover 后使用同一个 `request_id` 进行 `commit retry` / `delete retry` 的流程，并说明 `Head/List` 的读后写验证方式。
- 本次仅处理：
  - `specs/005-strong-consistency-metadata-layer/client-design.md`
- 本次未执行：
  - `T027` 及后续任务
  - 源码修改
  - CMake 接入
  - Metadata Client 新功能实现

## 读取与边界确认

- 已先读取根目录 `NOTREAD.md`，并遵守禁止路径约束。
- 按任务允许范围读取了：
  - `specs/005-strong-consistency-metadata-layer/client-design.md`
  - `specs/005-strong-consistency-metadata-layer/validation-matrix.md`
- 为对齐 T025 当前实现，最小读取了：
  - `apps/raft_metadata_client.cpp`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t025-report.md`
- 未修改源码、未修改 CMake、未读取 `specs/004-raft-industrialization/**`。

## checklist 状态

| Checklist | Total | Completed | Incomplete | Status |
|-----------|-------|-----------|------------|--------|
| requirements.md | 16 | 16 | 0 | PASS |

## 文档更新内容

本次在 `client-design.md` 中补充了以下稳定设计信息：

1. 当前最小 CLI 形态
   - 新增 `raft_metadata_client <addr> commit-retry ...`
   - 新增 `raft_metadata_client <addr> delete-retry ...`
   - 明确当前只是最小 failover retry 场景，不是完整 dispatcher

2. failover retry 规则
   - `commit retry` 必须复用原始 `request_id`
   - `delete retry` 必须复用原始 `request_id`
   - `NOT_LEADER` / `TIMEOUT` 后不得生成新的 request_id
   - retry 只能改变目标地址，不能改变 request identity
   - retry 必须有 `max-retries` 限制，不能无限循环

3. leader hint 使用方式
   - 若响应包含 `leader_hint.leader_address`，应优先作为下一次 retry 的目标地址
   - 文档中明确输出和提示 `leader_id`、`leader_address`

4. 输出字段
   - 明确客户端输出应包含：
     - `request_id`
     - `leader_id`
     - `leader_address`
     - `term`
     - `log_index`
   - 并补充建议输出当前投递地址、retry 次数和下一次 retry 目标地址

5. Head/List 读后写验证流程
   - `commit-retry` 成功后执行 `HeadMetadataRecord`
   - 验证 `found=true` 且 `state=COMMITTED`
   - 再执行 `ListMetadataRecords` 验证 `object_key` 出现在 committed-only 列表
   - `delete-retry` 成功后再次执行 `HeadMetadataRecord` / `ListMetadataRecords`
   - 验证目标对象不可见
   - 若 `Head/List` 返回 `NOT_LEADER`，根据 leader hint 切换到 leader 后继续验证

6. 当前阶段说明
   - 当前 `raft_metadata_client` 只覆盖 `commit-retry` / `delete-retry`
   - 当前尚未接入 CMake target
   - 正式构建接入留到 `T033`

## 修改文件

- 已修改：`specs/005-strong-consistency-metadata-layer/client-design.md`

## 验证

- 已检查 `client-design.md` 包含：
  - `commit retry`
  - `delete retry`
  - `same request_id`
  - `leader hint`
  - `Head/List` 读后写验证流程
- 本次为文档任务，未执行构建验证。

## 验收结论

- `T026`：通过

说明：

- 本次只更新稳定设计与使用流程，没有写入执行日志到 `client-design.md`
- 当前不进入 `T027`
- 按用户约束，未修改 `tasks.md`
