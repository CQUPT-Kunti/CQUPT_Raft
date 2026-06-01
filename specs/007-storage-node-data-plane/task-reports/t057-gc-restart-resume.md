# T057 GC Restart Resume

## 修改文件

- `modules/store/maintenance/garbage_collector.h`
- `modules/store/maintenance/garbage_collector.cpp`
- `modules/store/maintenance/gc_task_store.h`
- `modules/store/maintenance/gc_task_store.cpp`
- `modules/store/maintenance/module-notes.md`
- `modules/store/maintenance/AGENTS.md`
- `CMakeLists.txt`
- `tests/storage_garbage_collector_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/task-reports/t057-gc-restart-resume.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 为 `GarbageCollectorConfig` 增加可选 `persistence_root`
- 新增 `GarbageCollectorTaskStore`
  - 保存整份 GC task snapshot
  - 加载 GC task snapshot
  - 使用 schema header + 文本行格式持久化任务字段
  - 使用 `DurableFile` staging + publish + directory sync 做原子更新
- 在 `GarbageCollector` 中接入：
  - submit 时先持久化再真正接受 task
  - worker 状态迁移时持续更新 snapshot
  - 进程重启时 load persisted snapshot
  - 对 `running/queued/retry_pending` 做 state normalization + resume
  - 保留 `completed/failed/cancelled` 终态但不自动重跑
- 扩展 `storage_garbage_collector` 测试，覆盖：
  - submit 后 snapshot 文件存在
  - restart 后 queued / running / retry_pending task 恢复
  - completed / failed / cancelled 不重复执行
  - metadata boundary / reason / chunk identity / attempts / last_error 恢复不丢失
  - corrupted snapshot 不崩溃
  - 恢复后的 task 仍经过 safety checker

## GC task persistence 输入、输出和恢复语义

- 输入：
  - `GarbageCollectorTask` 列表
  - 字段至少包括：
    - `task_id`
    - `chunk_id`
    - `object_id`
    - `version`
    - `chunk_index`
    - `reason`
    - `metadata_boundary`
    - `state`
    - `attempts`
    - `max_attempts`
    - `last_error`
    - `last_error_detail`
    - `retryable`
    - `next_retry_after_ms`
- 输出：
  - `gc/tasks.snapshot`
  - 带 `GC_TASK_STORE_V1` header 的整份 snapshot
  - 字符串字段用 hex 编码；空串显式编码为占位符，避免解析歧义
- 恢复语义：
  - load 成功后先做字段校验
  - 再做 task state normalization
  - 再重新调度可恢复 task
  - load 失败或文件损坏时不崩溃，只记录明确错误并跳过恢复

## restart resume 对 queued / running / retry_pending / completed / failed 的处理

- `Queued`
  - 恢复后继续执行
- `Running`
  - 恢复后规范化为可重新调度状态
  - 不会因为崩溃前处于 `Running` 而永久卡死
- `RetryPending`
  - 恢复后保留上次错误事实
  - 继续作为可执行 task 重新尝试
- `Completed`
  - 恢复后只保留终态
  - 不自动重跑
- `Failed`
  - 恢复后只保留终态
  - 不自动重跑
- `Cancelled`
  - 恢复后保留终态
  - 不自动重跑

## safety checker / delete handler / persistence 当前边界

- 持久化和恢复只覆盖 task snapshot，不替代 safety checker
- 恢复后的 task 仍必须先经过 T055 metadata-driven safety checker
- safety checker 返回非 `kOk` 时，delete handler 不会被调用
- delete handler 仍必须由调用方注入
- 当前 persistence 是 whole-snapshot rewrite
- 当前没有 delayed retry scheduler
- 当前没有多进程共享同一 persistence root 的并发协议

## 是否调用 metadata / Raft；是否真实删除 chunk

- `GarbageCollector` persistence / resume 生产代码不调用 metadata / Raft
- `GarbageCollector` persistence / resume 生产代码不把 payload 写入 metadata / Raft
- 本任务生产逻辑只恢复 task，不直接删除 chunk
- 是否真实删除仍由恢复后运行到的 injected delete handler 决定
- 本次测试大多使用 fake handler；safety gate 测试验证了恢复后的 task 仍受 checker 约束

## 是否使用 tests/test_file/test_file.zip

- 否
- T057 测试不需要真实二进制 payload fixture

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - 日志路径：未单独保存 build 日志
- `ctest --test-dir build/linux -R "garbage_collector|storage_garbage_collector|gc_restart|restart_resume" --output-on-failure 2>&1 | tee tmp/007/t057-gc-restart-resume.log`
  - PASS
  - 日志路径：`tmp/007/t057-gc-restart-resume.log`
  - 说明：实际匹配到的测试名为 `storage_garbage_collector`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志摘要

- 本次验证未失败

## Windows 验证判断

- T057 涉及持久化文件、staging/publish、路径和 directory sync 边界
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本次没有新增 `T057-WIN`
- 由于本次引入了真实 GC task snapshot 持久化逻辑，Windows 下的 `SyncDirectory()` explicit unsupported 和真实 publish/durability 语义仍需后续单独验证

## 是否通过 T057

- 是

## 是否可以进入 T058

- 可以
- T058 应只做 US3 当前阶段低并发验证，不回头扩展 persistence schema、candidate generation、repair / rebalance / scrub

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 persistence 采用 whole-snapshot rewrite，没有 schema migration 机制
- 当前没有多进程共享同一 persistence root 的并发协议
- `next_retry_after_ms` 仍只是 task model 扩展点，没有 delayed retry scheduler
- safety checker 正确性仍依赖调用方提供的 metadata facts 是否新鲜完整
- Windows directory durability / delete 语义仍未实机验证
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/maintenance/module-notes.md`
- 更新了 `modules/store/maintenance/AGENTS.md`

## module-notes.md 是否补充了 .cpp 关键函数 / helper

- 是
- 已补：
  - task serialization / deserialization helper
  - persistence load / save helper
  - restart resume helper
  - task state normalization helper
  - completed / failed / retry_pending 恢复策略 helper
  - atomic write / publish helper
  - corrupted persistence file 处理 helper

## 是否修改高频文档及原因

- 修改了 `tasks.md`
  - 原因：标记 T057 完成，并把实际修改路径更新到本次真实范围
- 修改了 `common-risk-notes.md`
  - 原因：收缩已解决的 restart resume 表述，并补充 GC snapshot persistence 仍存在的 schema / Windows durability 风险

## common-risk-notes.md 读取结果

- 已重新读取并核对现有风险项
- 保留：
  - `T001` prerequisites 脚本错误指向 006
  - `T014/T023/T025/T026` Windows 待验证
  - `T019` timeout/cancellation / owner-thread shutdown 边界
  - `T024` corruption 自动回写未实现
  - `T027` pending/orphan 失败路径仍需更完整恢复收口
  - `T045` heartbeat/registry/failure cache/read-side 真实接线未完成
  - `T049/T055/T056` 剩余的全生命周期、metadata fact source、快照新鲜度风险

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T057` whole-snapshot rewrite、schema migration 缺失、Windows directory durability 待验证
- 删除：
  - 无整项删除
- 收缩：
  - `T049` 去掉“缺 restart cleanup / persistence”旧表述
  - `T055` 去掉“缺 restart cleanup/resume”旧表述
  - `T056` 去掉“缺 cleanup persistence / restart resume”旧表述
