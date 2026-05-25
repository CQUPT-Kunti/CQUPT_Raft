## T050 结果

- 本任务已完成。
- 已接入最终 `no-KV surface` 审计，覆盖 CMake target、CTest 和 `test.sh` 主入口；`test.ps1` 也已补上对应入口。
- 审计严格 fail 已退役 surface，同时把当前已知未完成项作为 blocker 输出，不伪装成已清理。

## audit 接入内容

- 新增 [tests/no_kv_surface_audit.cmake](/home/yangjilei/Code/C++/CQUPT_Raft/tests/no_kv_surface_audit.cmake)
  - 负责 strict retired-surface 扫描与 blocker 汇总
  - 失败信息会指明具体残留类型和文件
- 更新 [tests/CMakeLists.txt](/home/yangjilei/Code/C++/CQUPT_Raft/tests/CMakeLists.txt)
  - 新增 build target：`no_kv_surface_audit`
  - 新增 CTest：`NoKvSurfaceAudit`
  - 增加 label：`no-kv-audit`
- 更新 [test.sh](/home/yangjilei/Code/C++/CQUPT_Raft/test.sh)
  - 新增 `--group no-kv`
  - `all` 主入口已纳入 `no-kv` 审计
- 更新 [test.ps1](/home/yangjilei/Code/C++/CQUPT_Raft/test.ps1)
  - 在 Windows 常规入口和 managed 入口后调用 `cmake --build --target no_kv_surface_audit`

## 当前哪些 KV surface 已作为 fail 条件

- `kv_service_impl.h/.cpp`
- `test_kv_service.cpp`
- `apps/raft_kv_client.cpp`
- `proto/kv.proto`
- `CMakeLists.txt` 中的 `raft_kv_client` / `kv_service_impl`
- `tests/CMakeLists.txt` 中的 `test_kv_service`
- `proto/raft.proto` 中的：
  - `KvService`
  - `KvStatusCode`
  - `PutRequest` / `GetRequest` / `DeleteRequest`
  - `PutResponse` / `GetResponse` / `DeleteResponse`
- `test.sh` / `test.ps1` / `README.md` / `tests/README.md` / `docs/PERSISTENCE_DURABILITY_CONTRACT.md` 中的已退役 KV 主路径残留
- `docs/CURRENT_INDUSTRIALIZATION_ANALYSIS.md` 中几条已知的“KV 仍是当前主路径”旧表述

## 当前仍未清理的 blocker

- `CommandType::kSet` / `CommandType::kDelete`
- `KvStateMachine`
- `modules/raft/state_machine/state_machine.h`
- `modules/raft/state_machine/state_machine.cpp`
- `tests/test_state_machine.cpp`
- `tests/support/raft_snapshot_restart_test_utils.h` 中 `SetCommand` / `DeleteCommand` helper
- 这些 blocker 当前只报告，不作为 T050 fail 条件

## Linux 验证

- 配置：
  - `cmake --preset debug-ninja-low-parallel`
  - 结果：PASS
- 构建 audit target：
  - `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - 结果：PASS
- CTest 审计入口：
  - `ctest --test-dir build/linux --output-on-failure -R "NoKv|no_kv|KvSurface|Audit"`
  - 结果：PASS（1/1）
- Bash 主入口审计分组：
  - `./test.sh --skip-configure --skip-build --group no-kv`
  - 结果：PASS

## 是否可以进入 T051

- 可以进入 `T051`。
- 当前 `no-KV surface` 审计已就位，并且明确区分了 strict fail surface 与 legacy blocker。
