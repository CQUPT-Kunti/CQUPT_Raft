# T085 - Linux / Windows failure validation notes

## 1. 修改了哪些文件

- `specs/008-integrated-object-storage-system/risk-register.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t085-linux-windows-failure-validation-notes.md`

本任务没有修改：

- 任何生产代码
- 任何测试代码
- `tests/CMakeLists.txt`
- `proto/`
- app 参数实现

## 2. risk-register.md 新增或更新了哪些 Linux-specific validation notes

- 更新了 `risk-register.md` 日期到 `2026-06-10`。
- 新增“平台验收状态标记”，明确：
  - `Linux 已验证`
  - `Linux-only 计划验证`
  - `Windows fallback/smoke`
  - `Windows 待实机验证`
- 新增/补充了以下 Linux-primary 风险与边界：
  - `R-008`：US6 Linux-primary 故障验证不能误写成全平台已通过，并明确记录：
    - T076 committed upload 后同 `data_dir` 重启读取与 checksum 验证属于 `Linux 已验证`
    - T077 未提交对象不可见与 cleanup scaffold 前置条件属于 `Linux 已验证`
    - T079 无健康/容量足够节点导致 upload 失败属于 `Linux 已验证`
    - T078 的 100-op 真实 round-trip 仍是 `Linux-only 计划验证`
  - `R-011`：bounded concurrency 设计边界与真正 100-op/大文件验收必须分开表述。
  - `R-012`：payload boundary 在故障恢复与平台 fallback 中仍是硬约束，不能因为 Linux/Windows 差异放松。

## 3. risk-register.md 新增或更新了哪些 Windows fallback / adaptation / follow-up notes

- 新增/补充了以下 Windows 相关风险与说明：
  - `R-009`：Windows durable publish / rename / flush 语义与 Linux 不完全等价，要求：
    - required durability operation 不允许 silent no-op success
    - 必须使用真实 Windows 等价路径或返回明确错误
    - 当前只能记为支持目标 / 待实机验证，不能记为已验收
  - `R-010`：Windows 路径、临时目录、文件锁、端口复用和启动差异会影响 US6 验收，当前仅能声明为 startup/config/path smoke/fallback 目标
  - `R-008` / `R-011` / `R-012` 中都显式写明：
    - Windows recovery / concurrency / 大文件 bounded transfer 还没有实机通过证据
    - `flock` 只是 Linux 并发构建约束，Windows 需要等价串行化策略
    - Windows fallback 不得通过 payload ingress 或 durability no-op 来伪造通过

## 4. 是否准确区分 Linux 已验证、Linux-only、Windows fallback、Windows 待测

- 是。
- `risk-register.md` 已明确区分：
  - 哪些场景已有 Linux 验证证据
  - 哪些场景仍只是 Linux-only 计划验证 / disabled stress skeleton
  - 哪些内容当前只能作为 Windows fallback/smoke expectation
  - 哪些内容仍是 Windows 待实机验证
- 文档没有把未执行的 Windows 测试写成 PASS，也没有把 Linux-only 验收写成跨平台已通过。

## 5. 是否发现不合理点 / 警告 / 风险

- 当前 `debug-tests` preset 仍绑定 `build/linux`，而 T084 的最小验证路径实际使用了 `build/linux/safe`。这不是 T085 要修改的范围，但说明后续 validation 收口时需要避免把 preset 差异误写成测试缺失。
- T078 的真实 100-op round-trip 仍是 disabled skeleton，因此不能把“bounded concurrency 已实现”直接表述成“高压并发全链路已验收”。
- Windows 的 durability / rename / flush / file-lock / startup 边界已经在风险登记中记录为 follow-up，但仍缺少 Windows 实机结果。

## 6. 是否没有修改代码、测试、CMake、proto

- 是。
- 本任务是纯文档风险登记任务，没有修改代码、测试、CMake、proto，也没有运行全量构建或测试。

## 7. 验证命令和结果

### diff 检查

```bash
git diff -- specs/008-integrated-object-storage-system/risk-register.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t085-linux-windows-failure-validation-notes.md
```

结果：已执行。当前 diff 中允许文件只有 `risk-register.md`、`tasks.md` 和本任务报告。

说明：

- `tasks.md` 工作树中已经存在 T084 的未提交勾选差异；本任务只额外把 `T085` 从 `[ ]` 改为 `[X]`，没有改动其他任务内容。

### 文档一致性核对

```bash
rg -n "Linux|Windows|durability|recovery|concurrency|restart|bounded|payload|flock|path|rename|flush|identity|smoke" specs/008-integrated-object-storage-system/plan.md specs/008-integrated-object-storage-system/spec.md specs/008-integrated-object-storage-system/quickstart.md specs/008-integrated-object-storage-system/tasks.md
```

结果：已执行。`risk-register.md` 新增的 Linux-primary / Windows fallback 表述与 `spec.md`、`plan.md`、`quickstart.md`、`tasks.md` 的平台边界方向保持一致。

### 构建 / smoke

- 本任务为文档风险登记任务，默认不需要 `cmake configure/build/ctest`。
- 因此本任务未运行构建或 smoke build。
