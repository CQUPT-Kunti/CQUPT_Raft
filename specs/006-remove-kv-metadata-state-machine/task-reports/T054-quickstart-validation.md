# T054 Quickstart Validation

## 修改内容

- 更新 [quickstart.md](/home/yangjilei/Code/C++/CQUPT_Raft/specs/006-remove-kv-metadata-state-machine/quickstart.md)
- 校正为 metadata-only 主路径验证指南
- 同步为当前 Linux/Windows 最终验证均已通过的口径
- 未修改业务源码、协议或测试逻辑

## 使用的已有验证结果

- `t023-default-metadata-wiring.md`
- `t024-metadata-service-main-path.md`
- `t025-remove-kv-service-client.md`
- `T046-retire-kv-service.md`
- `T047-retire-raft-kv-client.md`
- `T048-remove-kv-proto-surface.md`
- `T049-update-metadata-only-docs.md`
- `T050-no-kv-surface-audit.md`
- `T043-us4-recovery-validation.md`
- `T051-linux-final-validation.md`
- `T051-linux-failure-fix.md`
- `t027-windows-validation.md`
- 当前任务状态确认：Linux 与 Windows 最终验证已通过

## quickstart 校正点

- 明确写出当前状态：
  - metadata-only 主路径已建立
  - KV 物理删除未完成
  - Linux final validation 已通过
  - Windows final validation 已通过
- Linux build 命令改成主路径相关 target：
  - `raft_demo`
  - `raft_metadata_client`
  - `no_kv_surface_audit`
- Linux metadata-focused CTest 改成 metadata-only 基础验证入口
- no-KV audit 改成直接使用：
  - `cmake --build ... --target no_kv_surface_audit`
  - `ctest -R NoKvSurfaceAudit`
- 保留 `raft_demo + raft_metadata_client` 的 metadata-only 手工 smoke
- 为 snapshot / restart / catch-up / recovery 补上 metadata-only 扩展验证入口
- Windows 部分保留复验入口，并改成最终通过后的维护口径

## 实际执行命令

- `cmake --build --preset debug-ninja-low-parallel --target raft_demo raft_metadata_client no_kv_surface_audit`
- `ctest --test-dir build/linux --output-on-failure -R 'NoKvSurfaceAudit'`
- `timeout 15 ./test.sh --skip-configure --skip-build --group no-kv`

## 实际结果

- `cmake --build --preset debug-ninja-low-parallel --target raft_demo raft_metadata_client no_kv_surface_audit`
  - PASS
  - 日志：`tmp/test-logs/t054-build.log`
- `ctest --test-dir build/linux --output-on-failure -R 'NoKvSurfaceAudit'`
  - PASS
  - `1/1 PASS`
  - 日志：`tmp/test-logs/t054-no-kv-ctest.log`
- `timeout 15 ./test.sh --skip-configure --skip-build --group no-kv`
  - 未作为 quickstart 推荐入口保留
  - 15 秒超时退出，退出码 `124`
  - 观察到该脚本实际展开为 configure/build/full single-worker CTest，而不只是 `NoKvSurfaceAudit`
  - 在截断日志中已出现与 no-KV 审计无关的失败：
    - `MetadataConcurrencyStressTest.AdmissionRejectsWhenInflightLimitIsReached`
  - 日志：`tmp/test-logs/t054-testsh-no-kv.log`

## Linux 状态

- 轻量 no-KV 审计命令：通过
- metadata-only quickstart 主路径命令：已校正
- Linux full final validation：当前任务状态中已通过
- `T051-linux-final-validation.md` 与 `T051-linux-failure-fix.md` 保留的是历史修复过程记录

## Windows 状态

- 本轮未执行 Windows 命令
- quickstart 中保留了 Windows configure/build/CTest 入口
- 当前任务状态中 Windows 最终验证已通过
- `t027-windows-validation.md` 保留的是历史阶段性补测记录

## 通过项

- quickstart 已不再引用 `KvService`
- quickstart 已不再引用 `raft_kv_client`
- quickstart 已不再把 `kv.proto` / KV SET/DEL / `KvStateMachineTest` 写成主验证路径
- quickstart 已明确推荐 direct no-KV audit 命令
- quickstart 已同步 Linux/Windows 最终验证通过口径

## 失败项

- `./test.sh --group no-kv` 当前不适合作为 quickstart 的轻量审计命令
- 无新增 quickstart 失败项

## blocked 项

- `T044/T045` 对应的 KV command / KvStateMachine 物理删除 blocker
- 历史 task report 中仍保留修复前失败快照，后续如需审计闭环可单独补最终收口报告

## 后续建议

- quickstart 日常使用优先走 direct build + `NoKvSurfaceAudit` + metadata-focused CTest
- `./test.sh --group no-kv` 如要继续作为轻量入口，需要后续单独校正脚本分组行为
- 继续把 `T044/T045` 作为 KV 物理删除的独立收尾项，不要与主路径最终验证混写
