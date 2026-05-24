# T059 Final No-KV Audit Validation

## 本次修改文件
- `test.sh`
- `test.ps1`
- `CMakePresets.json`

## test.sh --group no-kv 新行为
- 新增 `--group no-kv` 轻量入口
- `./test.sh --group no-kv` 现在只执行：
  - 可选 `cmake --preset debug-ninja-low-parallel`
  - 可选 `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - `ctest --test-dir build/linux --output-on-failure -R '^NoKvSurfaceAudit$'`
- 不再展开为 full configure/build/full single-worker CTest
- `--skip-configure` / `--skip-build` 语义保持有效

## recovery / snapshot restart 低并发策略
- 在 `test.sh` 中新增 `--group recovery`
- recovery / snapshot / catch-up 入口固定：
  - `CTEST_PARALLEL_LEVEL=1`
  - `ctest -j 1`
- `no-kv` 组与 recovery 重测试显式分离，避免把轻量审计混成重流程
- 本轮未全量执行 recovery 组；仅完成入口收口

## test.ps1 / CMakePresets.json fallback 清理
- `test.ps1` 帮助文本与启动输出不再保留 `KvStateMachineTest` fallback 文案
- Windows conservative fallback 子集改为：
  - `CommandTest`
  - `MetadataStateMachineTest`
  - `TimerSchedulerTest`
  - `ThreadPoolTest`
- `CMakePresets.json` 中：
  - `windows-debug-tests`
  - `windows-release-tests`
  的 filter 已同步从 `KvStateMachineTest` 改为 `MetadataStateMachineTest`
- `test.ps1` 同步补充：
  - no-KV audit target 说明
  - recovery / snapshot restart 建议低并发复验说明

## Linux 执行命令和结果
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS
  - 日志：`tmp/test-logs/t059-no-kv-build.log`
- `ctest --test-dir build/linux --output-on-failure -R '^NoKvSurfaceAudit$'`
  - PASS
  - `1/1 PASS`
  - 日志：`tmp/test-logs/t059-direct-no-kv-ctest.log`
- `./test.sh --skip-configure --skip-build --group no-kv`
  - PASS
  - 仅执行 `NoKvSurfaceAudit`
  - 未触发 configure / build / full CTest
  - 日志：`tmp/test-logs/t059-testsh-no-kv.log`

## Windows 状态
- Windows 未执行，原因是当前环境为 Linux；`test.ps1` / `CMakePresets.json` 已清理旧 `KvStateMachineTest` fallback 文案与 filter，需后续在 Windows 上复验

## CTest 结果
- 本轮只运行 direct `NoKvSurfaceAudit` 与脚本 `no-kv` 组
- 未运行全量 CTest
- 未运行 Windows CTest
- recovery 低并发策略已接入脚本入口，但本轮未追加执行 recovery 组

## NoKvSurfaceAudit 结果
- direct `ctest -R '^NoKvSurfaceAudit$'`：PASS
- `./test.sh --group no-kv`：PASS
- direct CTest 与 script group 行为已一致

## KV residual status
- no-KV 审计主入口已收口为轻量 strict audit
- Linux Bash 入口不再把 `no-kv` 组展开成全量验证
- Windows fallback 文案 / preset filter 不再宣传 `KvStateMachineTest`
- 当前剩余风险不在 KV residual 本身，而在：
  - Windows 入口尚未本地复验
  - recovery 重测试只完成低并发策略接线，未在本轮全量复验

## 是否修改生产代码
- 否

## 是否修改业务测试逻辑
- 否

## 是否仍有遗留风险
- Windows wrapper 与 preset 仅完成静态清理，需后续 Windows 环境实测
- recovery / snapshot restart 低并发策略已接入，但本轮未执行 `./test.sh --group recovery`

## 结论
- 可以认为 no-KV audit 入口已收口
- `./test.sh --group no-kv` 已成为真正的轻量 no-KV audit 入口
- `test.ps1` / `CMakePresets.json` 的旧 KV fallback 残留已清理
