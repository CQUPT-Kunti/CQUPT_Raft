# T087 - acceptance validation matrix

## 1. 修改了哪些文件

- `specs/008-integrated-object-storage-system/validation-matrix.md`
- `specs/008-integrated-object-storage-system/task-reports/t087-acceptance-validation-matrix.md`
- `specs/008-integrated-object-storage-system/tasks.md`

说明：`tasks.md` 当前工作树中已存在其他任务状态变更；本任务只新增了 `T087` 的勾选，没有调整其他任务内容。

## 2. validation-matrix.md 覆盖了哪些验收场景

- US1：真实 upload/download、checksum mismatch fail-fast、payload boundary、`PENDING` hidden / `COMMITTED` visible
- US2：cluster config generation、per-node config resolution、`generate-config`、`view_node_app` / `metadata_node_app` / `storage_node_app` startup smoke
- US5：1/3/5/7 quorum 计算、3-voter 失去多数不能 commit、5-voter 可用性、ViewNode registered node 不计入 voter
- US3：registration / discovery、heartbeat / liveness、cluster view / leader hint observation-only、placement excludes dead/stale
- US4：`node.identity` first-start / restart reuse / mismatch、MetadataNode `raft_id` validation、durable identity conflict diagnostics
- US6：restart after committed upload、uncommitted cleanup scaffold、capacity failure、100-op concurrency、bounded memory / chunked transfer、retry/backoff、cleanup hook
- Cross-cutting：Linux-primary validation 与 Windows fallback / pending 验证边界

## 3. 是否准确区分 Linux / Windows 验证状态

是。

- Linux 列只在已有任务报告、定向测试或 CLI smoke 证据时标记为 PASS。
- Windows 列单独标记为 `fallback/smoke expectation only`、`pending verification` 或 `not yet run`，没有把命令示例、设计目标或共享代码路径写成已通过。
- US6 recovery / concurrency / capacity failure 明确沿用 `risk-register.md` 中的 `Linux 已验证`、`Linux-only 计划验证`、`Windows fallback/smoke`、`Windows 待实机验证` 分层。

## 4. 是否准确区分 implemented / scaffold / disabled / pending / passed / failed

是。

- 对已有明确证据的场景标记为 `passed`。
- 对只完成实现、但尚无完整 acceptance 证据的场景标记为 `implemented + pending verification`。
- 对只完成局部约束或前置条件的场景标记为 `scaffold`。
- 对明确保留的 disabled 用例，例如 T028、T078，显式标记为 `disabled`，没有误写成通过。
- 当前矩阵没有伪造 `failed` 项；没有证据的场景保持 `pending verification` 或 `not yet run`。

## 5. 是否发现不合理点 / 警告 / 风险

- “真实 upload/download 全链路验收”与“已有 scaffold / quickstart 命令形态”之间仍有明显边界，不能因为 CLI 已存在就写成最终 acceptance PASS。
- quorum safety 中 T050 / T051 已实现并完成定向 build，但当前任务报告没有留下最终 PASS 证据，因此矩阵必须保守写成 `implemented + pending verification`。
- US6 中 `T078` 的 100-op 真实 round-trip 仍是 disabled skeleton；bounded concurrency 已实现，不等于高压并发全链路已验收。
- `uncommitted cleanup` 当前可以确认“对象不可见 + cleanup 前置条件成立”，但“自动清理已完成”仍不能写成已最终验收。

## 6. 是否修改 risk-register.md

未修改。

本任务直接复用 `risk-register.md` 已有的 Linux / Windows 分层和风险编号，没有新增公共风险条目。

## 7. 验证命令和结果

### diff 检查

```bash
git diff -- specs/008-integrated-object-storage-system/validation-matrix.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t087-acceptance-validation-matrix.md
```

结果：已执行。

- `validation-matrix.md` 与 `t087-acceptance-validation-matrix.md` 已进入 diff。
- `tasks.md` 中可确认存在 `T087=[X]`。
- 同一份 `tasks.md` diff 还带有当前工作树中既有的 `T084`、`T085`、`T086`、`T088` 勾选差异。
补充说明：当前工作树中的 `tasks.md` 已存在其他任务状态差异；实际 diff 输出若同时出现 `T084`、`T085`、`T086`、`T088` 等勾选，应视为既有工作树状态，不是本任务额外修改。

### 轻量测试列表核对

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -N' || echo "build/test lock busy, skip T087 ctest listing in this window"
```

结果：已执行。

- 成功拿到锁并输出当前 `debug-tests` preset 下的测试列表。
- 本命令只用于核对当前可见测试名与 build 目录，不涉及构建或运行测试。
- 观察到 `debug-tests` preset 当前枚举的是 `build/linux` 下的测试列表；而若干任务报告的定向 build / smoke 使用的是 `debug-ninja-safe -> build/linux/safe`。因此矩阵里对“验证入口”的描述优先采用任务报告中已经留痕的 target / gtest / CLI smoke，不把 preset 差异误写成测试失败。

### 构建与测试

本任务未运行 build，也未运行全量测试。

原因：T087 是文档矩阵任务，约束明确要求“不实现代码、不修改测试、不运行全量验证”。本次只执行了轻量 `ctest -N` 列表核对。
