# T040 Cluster Config Generation Tests 报告

## 1. 修改了哪些文件

- `tests/cluster_config_test.cpp`
- `tests/CMakeLists.txt`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t040-cluster-config-generation-tests.md`

未修改：

- 生产代码
- `proto/*`
- app 入口
- `common-risk-notes.md`
- `risk-register.md`

## 2. T040 的 cluster config generation tests 做了什么

新增 `tests/cluster_config_test.cpp`，并在 `tests/CMakeLists.txt` 中最小接入 `cluster_config_test` target。

本次测试只覆盖配置生成与校验边界，没有实现或修改任何生产配置逻辑。

新增测试包括：

- `cluster_config_generation_test.supports_1_3_5_7_voter_layouts_with_valid_generated_membership`
  - 验证 1/3/5/7 个 MetadataNode voter 场景下，配置生成成功且校验通过。
- `cluster_config_generation_test.same_request_generates_reproducible_config_without_hardcoded_demo_topology`
  - 验证相同请求生成结果可重复，支持固定 node_id / raft_id / capacity override，不退化成硬编码 demo 拓扑。
- `cluster_config_validation_test.rejects_zero_storage_capacity_in_generated_config`
  - 验证 StorageNode capacity 基础约束被校验，0 capacity 会导致生成结果无效并返回诊断。

## 3. 覆盖了哪些配置生成和校验边界

已覆盖：

- 1/3/5/7 个 Raft voter 配置生成
- 生成结果可重复
- `cluster_id`、`base_dir`、`membership_epoch` 与请求一致
- `node_id` 唯一且非空
- endpoint 唯一且与请求端口基线一致
- `data_dir` / `snapshot_dir` 唯一且非空
- MetadataNode `raft_id` 唯一且为正数
- 初始 `voter_raft_ids` 与生成出的 MetadataNode voter 集合一致
- StorageNode `capacity_bytes` 默认值与 override 值生效，并拒绝 0 capacity

有意未覆盖：

- per-node config resolution
- 独立 quorum helper 的直接断言

原因：

- 这两部分分别属于 T042 / T043 的后续边界。
- 当前 T040 只锁定“配置生成出的 voter 集合与初始 membership 一致”，不提前替 T042 / T043 做实现或扩展覆盖。

## 4. 是否有 disabled/scaffold 测试

没有新增 disabled/scaffold 测试。

## 5. 是否发现不合理点 / 警告 / 风险

发现两点需要说明：

- `ctest --preset debug-tests -R cluster_config` 在当前仓库里命中了 `build/linux`，而这次按构建锁执行的是 `debug-ninja-safe`，实际测试产物位于 `build/linux/safe`，因此 preset 方式第一次返回了 “No tests were found”。这不是用例失败，后续已改用 `ctest --test-dir build/linux/safe -R cluster_config` 完成验证。
- 当前 T040 只验证了“生成出的 voter 数与初始 membership 一致”，没有把 `ComputeInitialRaftQuorumSize(...)` 作为独立行为断言；该直接 quorum helper 覆盖建议在 T043 继续补齐。
- `tasks.md` 在本窗口开始前已存在其他未提交改动；本次只将 `T040` 从 `[ ]` 改为 `[X]`，若 `git diff` 同时出现 `T041` 等其他变更，应视为工作区既有脏改动而非本任务新增。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

未修改 `common-risk-notes.md` 或 `risk-register.md`。

## 7. 验证命令和结果

执行的验证命令：

```bash
git diff -- tests/cluster_config_test.cpp tests/CMakeLists.txt specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t040-cluster-config-generation-tests.md
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target cluster_config_test'
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R cluster_config --output-on-failure'
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --test-dir build/linux/safe -R cluster_config --output-on-failure'
```

结果：

- `cluster_config_test` 构建通过
- `ctest --preset debug-tests -R cluster_config --output-on-failure`
  - 未命中测试，输出 `No tests were found!!!`
  - 原因是 preset 目录与本次 `debug-ninja-safe` 构建目录不一致
- `ctest --test-dir build/linux/safe -R cluster_config --output-on-failure`
  - 3/3 PASS
  - 通过用例：
    - `cluster_config_generation_test.supports_1_3_5_7_voter_layouts_with_valid_generated_membership`
    - `cluster_config_generation_test.same_request_generates_reproducible_config_without_hardcoded_demo_topology`
    - `cluster_config_validation_test.rejects_zero_storage_capacity_in_generated_config`

本地日志文件：

- `tmp/test-logs/t040-build.log`
- `tmp/test-logs/t040-ctest.log`
- `tmp/test-logs/t040-ctest-safe.log`
