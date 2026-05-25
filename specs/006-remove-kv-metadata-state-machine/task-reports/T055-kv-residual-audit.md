# T055 KV Residual Audit

## 审计范围
- 静态扫描：`modules/`、`apps/`、`proto/`、`tests/`、`CMakeLists.txt`、`tests/CMakeLists.txt`、`test.sh`、`test.ps1`、`CMakePresets.json`、`docs/`
- 参考：`T053`、`T054`、`T044`、`T045`、`T050`、`cross-task-risk-notes.md`
- 排除：`build/`、`third_party/`、`tmp/`
- 本任务未删除、重命名、构建或运行全量测试

## 使用的扫描命令
- `rg -n --hidden --glob '!build/**' --glob '!third_party/**' --glob '!tmp/**' --glob '!.git/**' --glob '!specs/006-remove-kv-metadata-state-machine/task-reports/**' "(CommandType::kSet|CommandType::kDelete|KvStateMachine|KVStateMachine|KvService|KVService|KvStatusCode|raft_kv_client|test_kv_service|test_state_machine|PutRequest|GetRequest|DeleteRequest|SET\\||DEL\\||DebugGetValue|kv-service|kv\\.proto)" .`
- `rg -n --hidden --glob '!build/**' --glob '!third_party/**' --glob '!tmp/**' --glob '!.git/**' --glob '!specs/006-remove-kv-metadata-state-machine/task-reports/**' "(kSet|kDelete|SetCommand|DeleteCommand|kv|KV|Kv)" modules apps proto tests CMakeLists.txt tests/CMakeLists.txt test.sh test.ps1 docs .specify`
- `rg -n "no-kv|group no-kv|kv-service|KvStateMachineTest|CommandTest|test_state_machine|raft_kv_client" test.sh test.ps1 CMakeLists.txt tests/CMakeLists.txt CMakePresets.json`

## 必须删除的生产代码残留
- `modules/raft/common/command.h`、`modules/raft/common/command.cpp`：仍保留 `CommandType::kSet/kDelete` 与 `SET|` / `DEL|` codec
- `modules/raft/state_machine/state_machine.h`、`modules/raft/state_machine/state_machine.cpp`：`KvStateMachine`、`KVS1` snapshot、KV map 持久化仍完整存在
- `modules/raft/node/raft_node.h`、`modules/raft/node/raft_node.cpp`：仍保留 `CompositeKvMetadataStateMachine`、`dynamic_cast<KvStateMachine>`、`DebugGetValue()`、`kv=` 诊断支路、`RpcKind::kKv*`、`CommandType::kSet/kDelete` 校验分支
- `modules/raft/common/config.h`：仍暴露 `KvRequestLimits` 与 `NodeConfig::kv_limits`
- `apps/main.cpp`：仍解析并打印 `kv_max_*` 配置
- `CMakeLists.txt`：`raft_core` 仍编译 `modules/raft/state_machine/state_machine.cpp`

## 必须删除或迁移的测试残留
- `tests/test_state_machine.cpp`：纯 `KvStateMachineTest`
- `tests/CMakeLists.txt`：仍注册 `test_state_machine`
- `tests/test_command.cpp`：仍覆盖 `CommandType::kSet/kDelete` 与 `SET|` / `DEL|`
- 高价值回归仍走 KV 断言：`tests/snapshot_test.cpp`、`tests/persistence_test.cpp`、`tests/test_raft_snapshot_recovery.cpp`、`tests/test_raft_snapshot_restart.cpp`、`tests/test_raft_snapshot_catchup.cpp`、`tests/test_raft_election.cpp`、`tests/test_raft_commit_apply.cpp`、`tests/raft_integration_test.cpp`
- `tests/support/raft_snapshot_restart_test_utils.h`：仍保留 `SetCommand()` / `DeleteCommand()` / `kvbridge:` helper
- 测试入口残留：`test.ps1` 仍宣传 `KvStateMachineTest` fallback；`CMakePresets.json` 仍保留 `KvStateMachineTest` fallback filter；`tests/README.md` 仍多处写 `test_state_machine` / `KvStateMachineTest`

## 允许暂存的历史说明残留
- `specs/006-remove-kv-metadata-state-machine/` 下的 `spec.md`、`plan.md`、`contracts/`、`tasks.md`
- `specs/003-*`、`specs/004-*`、`specs/005-*` 的历史基线、迁移说明、验证矩阵
- `specs/006-remove-kv-metadata-state-machine/task-reports/` 与 `cross-task-risk-notes.md`
- `docs/CONSISTENCY_LAYER_TRANSITION_PLAN.md` 这类过渡规划文档

## no-KV audit 自身允许出现的关键词
- `tests/no_kv_surface_audit.cmake` 中的检测字面量：`KvService`、`KvStatusCode`、`Put/Get/DeleteRequest`、`CommandType::kSet/kDelete`、`KvStateMachine`、`test_state_machine.cpp`、`SetCommand(`、`DeleteCommand(`
- `T055` 与其他 task report 中的审计引用关键词

## 需要人工判断的可疑项
- `apps/main.cpp` 的局部变量 `kv` 是配置 key/value 解析，不等于业务 KV；但 `kv_limits` 本身仍属生产残留
- `docs/CURRENT_INDUSTRIALIZATION_ANALYSIS.md` 大多是分析历史与技术债，少量 current-path 表述需后续人工复核
- `AGENTS.md` / 模块 `AGENTS.md` 同时包含边界说明与历史兼容语境，不宜机械清理
- `tests/metadata_state_machine_test.cpp` 中 `DebugGetValue("legacy-kv-key", ...)` 是否保留，应在 `T057` 结合测试价值判断

## T044 / T045 blocker 当前状态
- `T044` 仍 blocked：`CommandType::kSet/kDelete` 仍在生产代码和高价值恢复测试中活跃引用
- `T045` 仍 blocked：`KvStateMachine`、`state_machine.h/.cpp`、`test_state_machine.cpp` 仍在构建图与测试图中存在

## no-KV audit 覆盖现状
- 已 strict fail：`KvService`、`raft_kv_client`、`kv.proto`、KV proto message、`test_kv_service`
- 当前仅作为 tolerated blocker：`CommandType::kSet/kDelete`、`KvStateMachine`、`state_machine.h/.cpp`、`tests/test_state_machine.cpp`、`tests/support/raft_snapshot_restart_test_utils.h`
- 当前盲区：`CMakePresets.json` 的 `KvStateMachineTest` fallback filter、`test.ps1` 的 `KvStateMachineTest` fallback 说明

## `./test.sh --group no-kv` 当前风险
- 当前 `test.sh` 已是 `T051` Linux 全量验证脚本，不再提供 `--group` 分发
- 文件中不存在 `--group` / `no-kv` 参数逻辑
- 因此 `./test.sh --group no-kv` 不能视为轻量 no-KV audit 入口，应在 `T059` 单独修正

## Linux / Windows / CTest
- Linux 结果：PASS，已完成静态审计与分类
- Windows 结果：Windows 未执行，T055 是静态审计任务
- CTest 结果：未运行 CTest，T055 仅做静态审计

## KV residual status
- metadata-only 主路径：已完成
- 退役的 KV service/client/proto surface：已退出主路径
- KV 物理删除：未完成
- 当前残留主类：生产代码中的 KV command / KV state machine / KV limits / KV debug branch；snapshot/recovery/catch-up 测试中的 KV helper 与 KV 断言；Windows fallback / preset / README 中的旧测试入口残留

## 后续顺序建议
- 先 `T056`：清生产代码残留与构建图残留
- 再 `T057`：迁移或删除重复测试、旧 KV 语义测试与 helper
- 之后 `T058`：把 tolerated blocker 和 preset/script 盲区升级进 strict no-KV audit
- 最后 `T059`：重建 `test.sh --group no-kv` / `test.ps1` 的轻量审计入口并复验
