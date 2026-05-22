# tests

## 目录职责

- `tests/` 只放测试入口、测试辅助头、测试构建脚本和测试组织文档。
- 当前测试主线按主题分布：
  - `metadata_*`：metadata 编解码、状态机、快照、failover、client 场景
  - `test_raft_*` / `raft_integration_test.cpp`：Raft 一致性、复制、切主、快照追赶、重启恢复
  - `persistence*.cpp` / `snapshot_test.cpp` / `test_snapshot_storage_reliability.cpp`：持久化、快照存储、恢复边界
  - `test_command.cpp` / `test_state_machine.cpp` / `test_thread_pool.cpp` / `test_min_heap_timer.cpp`：基础单测
- `support/` 存放跨文件复用的测试 helper；helper 只负责构造、等待、诊断、断言辅助，不承载测试结论。

## 修改入口

- 修改 `tests/` 下文件前，先读本文件。
- 如果改动 `tests/support/`，继续读 `tests/support/AGENTS.md`。
- 不为单次改动扫描整个仓库；优先读取：
  - `tests/CMakeLists.txt`
  - 目标测试文件
  - 直接依赖的 helper 头

## 文件组织规则

- 同一测试文件应保持单一主题；新增不同职责的大块 case 时，优先拆到新的同主题文件。
- 允许一个 target 由多个 `.cpp` 共同组成；拆分以“主题清晰”优先，不追求机械行数上限。
- 共享命令构造、cluster 生命周期、等待逻辑、恢复断言优先放进 `support/`。
- 少量强上下文重复可保留；跨文件扩散的 helper 必须收敛。

## CMake / CTest 规则

- 优先保持已有 target 名称和 label 语义不变。
- 可以为同一个 gtest target 增加多个 source 文件，但不要改变业务链接关系。
- 修改 `tests/CMakeLists.txt` 后，至少重新 `cmake --preset ...` 一次，再构建受影响 target。
- 不随意新增“临时 target”；诊断类或拆分后的文件优先并入现有主题 target。

## 修改注意事项

- 不改业务源码语义，不通过测试重构顺手改 Raft 行为。
- 不删除仍有独立覆盖价值的恢复、快照、追赶、切主、持久化 case。
- 测试 helper 可以抽取、移动、改名，但调用语义必须保持等价。
