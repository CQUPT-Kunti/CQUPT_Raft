# T067 US4 Registry Placement Validation

- 修改文件
  - `specs/007-storage-node-data-plane/task-reports/t067-us4-registry-placement-validation.md`
  - `specs/007-storage-node-data-plane/tasks.md`

- 运行了哪些 US4 验证
  - 构建当前 `debug-ninja-low-parallel` 预设
  - `storage_heartbeat_registry` / `storage_node_service` / `storage_node_client`
  - `store_placement_policy` / `store_placement_manager`
  - `storage_read_integration` / `storage_read_chunk_contract`

- 验证命令、PASS/FAIL、日志路径
  - `mkdir -p tmp/007`
    - PASS
  - `cmake --build --preset debug-ninja-low-parallel`
    - PASS
  - `ctest --test-dir build/linux -R "storage_heartbeat_registry|storage_node_service|storage_node_client" --output-on-failure 2>&1 | tee tmp/007/t067-us4-registry-service-client.log`
    - PASS
    - 实际匹配到的测试名为 `storage_node_service`、`storage_node_client`、`storage_heartbeat_registry`
    - 日志路径：`tmp/007/t067-us4-registry-service-client.log`
  - `ctest --test-dir build/linux -R "store_placement_policy|store_placement_manager" --output-on-failure 2>&1 | tee tmp/007/t067-us4-placement.log`
    - PASS
    - 实际匹配到的测试名为 `store_placement_policy`、`store_placement_manager`
    - 日志路径：`tmp/007/t067-us4-placement.log`
  - `ctest --test-dir build/linux -R "storage_read|storage_read_chunk_contract" --output-on-failure 2>&1 | tee tmp/007/t067-us4-read-replica.log`
    - PASS
    - 实际匹配到的测试名为 `storage_read_integration`、`storage_read_chunk_contract`
    - 日志路径：`tmp/007/t067-us4-read-replica.log`

- 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志
  - 本次验证未失败

- Windows 验证判断
  - T067 仅代表当前 Linux 环境下的 US4 阶段验证
  - 当前无 Windows 编译/测试环境，不伪造 Windows PASS
  - 本任务不新增 `T067-WIN`

- 是否通过 T067
  - 是

- 是否可以进入 T068
  - 可以
  - 前提：T068 进入 US5 restart/recovery 测试，不把 T067 扩展成额外功能实现

- 当前任务发现的不合理点 / 警告 / 风险
  - `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`，执行 `007` 任务时仍需要人工纠偏
  - 当前验证未暴露新的 US4 风险，也没有关闭现有 Windows durability、timeout/cancellation 运行中传播、corruption 自动回写、registry freshness、delayed retry scheduler、GC schema migration / 多进程 persistence_root、Repair / Rebalance / Scrub 等既有风险

- 是否修改高频文档及原因
  - 修改了 `specs/007-storage-node-data-plane/tasks.md`
    - 原因：标记 T067 完成，并记录当前真实验证范围与测试名

- common-risk-notes.md 读取结果
  - 已读取并核对
  - 仍存在且未误删的风险包括：
    - Windows 删除 / directory durability 待验证
    - timeout / cancellation 运行中传播未实现
    - corruption 自动回写未实现
    - registry fact 新鲜度风险
    - delayed retry scheduler 未实现
    - GC schema migration / 多进程 persistence_root 协议未定义
    - Repair / Rebalance / Scrub 未实现

- common-risk-notes.md 新增/删除/保留情况
  - 新增：无
  - 删除：无
  - 保留：现有风险全部保留，因 T067 仅做验证未实现新功能
