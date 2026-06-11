# T020 任务报告

## 做了什么

本任务只做 local RPC status regression 入口，不修改 ViewNode registry 生产实现，不实现 self refresh loop，不改 peer sync，不改 Raft membership。

本次完成内容：

- 在 `examples/object-storage-local-3meta-6store/rpc_demo.sh` 内新增 `status-self-liveness` 动作。
- 保留原有 `status` / `upload` / `download` / `roundtrip` 行为不变。
- `status-self-liveness` 会：
  1. 先执行一次 `storage_client status`；
  2. 断言输出中存在本地 `view-1` 记录；
  3. 断言该记录的 `liveness=live`；
  4. 从 `cluster.json` 读取 `liveness_dead_timeout_ms`；
  5. 等待 `dead_timeout + grace`；
  6. 再执行一次 `storage_client status`；
  7. 如果 `view-1` 变成 `stale` / `suspect` / `dead`，直接失败并打印该行。

这样可以在 example/status 层稳定暴露“ViewNode 运行中把自己判为 stale/dead”的回归，而不是把问题藏在单元测试或手工观察里。

## 修改了哪些文件

- `examples/object-storage-local-3meta-6store/rpc_demo.sh`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t020-add-viewnode-self-liveness-regression-to-local-rpc-status-expectations.md`

## status 现在如何检查 ViewNode self liveness

新增入口：

```bash
examples/object-storage-local-3meta-6store/rpc_demo.sh status-self-liveness
```

检查规则：

- 必须能从 `storage_client status` 输出中看到 `view_node node_id=view-1 ... liveness=...`
- 初始检查要求 `liveness=live`
- 二次检查要求跨过 `liveness_dead_timeout_ms` 后仍保持 `liveness=live`
- 如果输出里出现：
  - `liveness=stale`
  - `liveness=suspect`
  - `liveness=dead`
  脚本立即失败，不吞掉问题

兼容性：

- 原有 `rpc_demo.sh status` 未改成强制等待 TTL
- 原有 `rpc_demo.sh roundtrip` 未改
- 008 baseline 脚本行为保持兼容

## 当前 status 输出是否缺少 liveness 字段

不缺。

当前 `storage_client status` 已经输出：

```text
view_node node_id=view-1 ... liveness=live|stale|suspect|dead ...
```

因此本任务不需要新增 009 sibling script，也不需要改 `storage_client` 输出格式。

当前仍存在的缺口不是“看不到 liveness”，而是“ViewNode 没有持续 self refresh，导致跨过 TTL 后 liveness 真的掉成 dead”。

## 是否同步 validation-matrix

已同步。

在 `validation-matrix.md` 的 `Scenario Matrix` 中新增：

- `Local RPC ViewNode self-liveness regression`

入口为：

```bash
examples/object-storage-local-3meta-6store/qidong.sh
examples/object-storage-local-3meta-6store/rpc_demo.sh status-self-liveness
examples/object-storage-local-3meta-6store/tingzhi.sh
```

并明确：

- 跨过 dead TTL 后健康运行中的 ViewNode 仍应保持 `LIVE`
- 在 T021 之前若出现 `STALE` / `SUSPECT` / `DEAD`，应直接暴露失败

## 验证命令

语法检查：

```bash
bash -n examples/object-storage-local-3meta-6store/rpc_demo.sh
```

targeted build：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client
) 9>/tmp/cqupt_raft_build.lock
```

local RPC regression：

```bash
examples/object-storage-local-3meta-6store/tingzhi.sh
examples/object-storage-local-3meta-6store/qidong.sh
examples/object-storage-local-3meta-6store/rpc_demo.sh status-self-liveness
examples/object-storage-local-3meta-6store/tingzhi.sh
```

## 验证结果

- `bash -n`: PASS
- targeted build: PASS
- `qidong.sh`: PASS
- `rpc_demo.sh status-self-liveness`: FAIL，且该 FAIL 正是本任务要暴露的已知回归
- `tingzhi.sh`: PASS

关键失败摘要：

- 初始 status：`view-1 liveness=live`
- 等待 `dead_timeout_ms=15000` 加 `grace_ms=2000` 后再次 status：
  - `view_node node_id=view-1 ... liveness=dead ...`
  - 同时出现诊断：`status=liveness_excluded ... message=node is not live`

这说明：

- 当前 local RPC status 输出已经具备观察 ViewNode 自身 liveness 的能力；
- 当前生产实现仍缺少 self refresh；
- T020 已成功把该问题固定为 example/status 层可复现回归入口。

日志文件：

- `tmp/test-logs/t020-build.log`
- `tmp/test-logs/t020-start.log`
- `tmp/test-logs/t020-status-self-liveness.log`
- `tmp/test-logs/t020-stop-before.log`
- `tmp/test-logs/t020-stop-after.log`
- `tmp/test-logs/t020-run.rc`

## PASS / FAIL / SKIPPED

任务结果：PASS

说明：

- 本任务目标是“新增并验证 regression 入口”，不是“修复 self refresh”
- local RPC regression 命令本身返回 FAIL，属于预期诊断结果，不是任务失败

## Linux / Windows / macOS

- Linux：已验证，regression 入口成功暴露问题
- Windows：未实机验证，pending
- macOS：未实机验证，pending

## 是否可以进入 T021

可以进入 T021。

T020 已完成的前置条件：

- example/status 层已经有明确的 ViewNode self-liveness regression 入口
- 该入口已经在 Linux 本地复现当前问题
- 后续 T021 可以直接针对这个入口推进 self refresh 修复，而不是先补诊断脚手架
