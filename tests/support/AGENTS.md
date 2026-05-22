# tests/support

## 目录职责

- 本目录只放跨多个测试文件复用的 helper 头。
- helper 允许做：
  - command / payload 构造
  - cluster 启停与等待
  - 通用恢复断言与诊断拼接
  - 测试专用路径、快照文件、环境变量辅助
- helper 不允许做：
  - 固化具体测试结论
  - 隐藏业务行为差异
  - 引入与测试主题无关的高层封装

## 文件归属

- `metadata_test_utils.h`：metadata 状态机与 metadata Raft 回归共享的命令构造/单机快照辅助。
- `raft_snapshot_restart_test_utils.h`：snapshot restart / recovery 主题共享的 cluster、等待、文件与 failpoint 辅助。

## 修改规则

- 只在出现明确跨文件重复时扩展 helper。
- 优先保持 header-only，避免为测试 support 再引入新的链接目标。
- 新增 helper 时，命名应表达主题边界，不做“万能工具箱”。
