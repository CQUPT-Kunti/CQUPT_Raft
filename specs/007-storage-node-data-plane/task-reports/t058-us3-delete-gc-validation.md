# T058 US3 Delete GC Validation

## 修改文件

- `specs/007-storage-node-data-plane/task-reports/t058-us3-delete-gc-validation.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 运行了哪些 US3 验证

- 删除闭环与 delete contract：
  - `storage_delete_gc`
  - `storage_delete_chunk_contract`
- service / client 删除路径：
  - `storage_node_service`
  - `storage_node_client`
- GarbageCollector：
  - `storage_garbage_collector`

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - 日志路径：未单独保存 build 日志
- `ctest --test-dir build/linux -R "storage_delete_gc|storage_delete_chunk_contract|delete_chunk_contract" --output-on-failure 2>&1 | tee tmp/007/t058-us3-delete-gc.log`
  - PASS
  - 日志路径：`tmp/007/t058-us3-delete-gc.log`
  - 说明：实际匹配到的测试名为 `storage_delete_gc`、`storage_delete_chunk_contract`
- `ctest --test-dir build/linux -R "storage_node_service|storage_node_client" --output-on-failure 2>&1 | tee tmp/007/t058-us3-storage-node-delete.log`
  - PASS
  - 日志路径：`tmp/007/t058-us3-storage-node-delete.log`
  - 说明：实际匹配到的测试名为 `storage_node_service`、`storage_node_client`
- `ctest --test-dir build/linux -R "storage_garbage_collector|garbage_collector" --output-on-failure 2>&1 | tee tmp/007/t058-us3-garbage-collector.log`
  - PASS
  - 日志路径：`tmp/007/t058-us3-garbage-collector.log`
  - 说明：实际匹配到的测试名为 `storage_garbage_collector`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志摘要

- 本次验证未失败

## Windows 验证判断

- T058 只在当前 Linux 环境下执行 US3 删除 / GC 验证
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 既有 Windows 待验证项继续保留，本任务未新增 `T058-WIN`

## 是否通过 T058

- 是

## 是否可以进入 T059

- 可以

## 当前任务发现的不合理点 / 警告 / 风险

- 本次 US3 验证范围内未暴露新的删除 / GC 风险
- 既有风险仍保留：
  - Windows 删除语义与 directory durability 待验证
  - timeout/cancellation 运行中传播未实现
  - corruption 自动回写未实现
  - registry / heartbeat / failure cache 未接入
  - delayed retry scheduler 未实现
  - GC schema migration 与多进程 `persistence_root` 协议未定义

## 是否修改高频文档及原因

- 修改了 `tasks.md`
  - 原因：标记 T058 完成
- 未修改 `common-risk-notes.md`
  - 原因：本次仅做验证，未发现需要新增、删除或收缩的 US3 风险

## common-risk-notes.md 读取结果

- 已读取并核对当前风险项
- 与 T058 直接相关且仍需保留的风险包括：
  - `T014/T023/T025/T026` Windows 删除 / durability 待验证
  - `T019` timeout/cancellation 运行中传播边界
  - `T024` corruption 自动回写未实现
  - `T045` registry / failure cache 未接入
  - `T049` 删除 / GC 仍非完整后台生命周期闭环
  - `T055` metadata fact freshness、延迟重试调度器未实现
  - `T056` candidate 生成依赖 metadata snapshot 新鲜度
  - `T057` whole-snapshot rewrite、schema migration、多进程 persistence root 协议、Windows directory durability 待验证

## common-risk-notes.md 新增/删除/保留情况

- 新增：无
- 删除：无
- 保留：全部保留
