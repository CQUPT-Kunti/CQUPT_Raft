# Cross-Platform Coding Notes

- 先看验证入口：
  - Linux：`ctest --preset debug-tests`
  - Windows baseline：`ctest --preset windows-release-tests`
  - Windows full managed：`ctest --preset windows-release-managed-tests`
  - 新增重要功能后，Linux 和 Windows full managed 都要跑

- Storage / durability：
  - Linux 用 `fsync` / directory sync
  - Windows 用 `FlushFileBuffers` 和正确的文件 / 目录句柄
  - 不要把 durability 操作改成 no-op success
  - 不要吞掉 sync / flush / rename / replace 错误
  - 不要改持久化格式

- Failure injection：
  - 默认关闭，必须 opt-in
  - seam 要写清 `operation`、`path`、`failure category`、`recovery expectation`
  - 不要硬编码 `linux_specific=true` 这种平台预期
  - 新 seam 要确认 Windows 也能触发

- 测试路径：
  - Windows 测试路径要短
  - 测试必须使用独立 `data_dir` / `snapshot_dir`
  - 不要共用 `./raft_data` / `./raft_snapshots`
  - 注意 Windows file lock 和 cleanup

- Runtime / timing：
  - 不要只靠固定 sleep
  - 优先等待明确状态：`leader`、`redirect ready`、`commit_index`、`last_applied`
  - 注意 Windows 端口复用、线程调度、RPC deadline

- 平台差异放哪里：
  - `storage` / `runtime` helper 可以放平台差异
  - `node` / `replication` 不要散落大量平台 `#ifdef`
  - 测试可以有平台路径和端口差异，但不能放宽一致性断言

- 每次改代码前检查：
  - 是否影响持久化？
  - 是否涉及 `rename` / `replace` / directory sync？
  - 是否涉及 snapshot publish / prune？
  - 是否涉及端口、线程、timeout？
  - 是否需要同时更新 `validation-matrix.md` / `platform-support.md` / `tests/README.md`？
