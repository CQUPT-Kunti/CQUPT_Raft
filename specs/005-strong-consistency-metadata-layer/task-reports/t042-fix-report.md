# T042-fix Report

## 问题根因

Windows 下 `MetadataClientScenarioTest` 原先通过外部 shell 命令启动 `raft_metadata_client`，依赖命令拼接、路径引号、重定向和工作目录处理。该链路在 Windows 上不稳定，导致测试进程启动/输出捕获异常，最终表现为：

- `The filename, directory name, or volume label syntax is incorrect.`
- `result.exit_code == 1`

这不是 metadata 业务逻辑错误，而是 Windows 测试 harness 的命令执行兼容性问题。

## 修改文件

1. `tests/metadata_client_scenario_test.cpp`
2. `tests/CMakeLists.txt`

## 修复方式

### 1. Windows 测试改为进程内调用真实 CLI 入口

在 `tests/metadata_client_scenario_test.cpp` 中：

- 保留非 Windows 平台原有外部进程执行方式；
- 对 Windows 平台不再使用 shell 命令拼接；
- 直接调用真实客户端入口 `raft_metadata_client_entry(...)`；
- 使用 `testing::internal::CaptureStdout()` / `CaptureStderr()` 捕获 CLI 输出；
- 仍将输出写入测试生成的场景日志文件，便于失败诊断。

这样避免了：

- 可执行路径包含空格时的引号问题；
- Windows shell 重定向语法差异；
- 工作目录/命令行转义导致的启动失败；
- `*_NOT_BUILT` 已解决后残留的外部调用不兼容问题。

### 2. 仅为测试目标注入真实客户端入口源码

在 `tests/CMakeLists.txt` 中：

- 为 `test_metadata_client_scenario` 增加 `../apps/raft_metadata_client.cpp`；
- 仅对该测试目标内的这份源文件施加 `main=raft_metadata_client_entry` 编译定义；
- 不修改 `apps/raft_metadata_client.cpp` 本身，不改变正式可执行程序的行为。

## Windows 验证结果

### 构建

命令：

```powershell
cmake --build --preset windows-debug --target raft_metadata_client test_metadata_client_scenario
```

结果：`PASS`

### 单独验证客户端场景测试

命令：

```powershell
ctest --test-dir build/windows --output-on-failure -C Debug -R "^MetadataClientScenarioTest\."
```

结果：`PASS`

- `5/5` 通过
- 之前失败的 5 个用例全部通过

### 再验证 Windows metadata 全量相关测试

命令：

```powershell
ctest --test-dir build/windows --output-on-failure -C Debug -R "Metadata(Command|Manifest|StateMachine|Snapshot|Failover|ClientScenario)Test"
```

结果：`PASS`

- `34/34` 通过

## 是否影响 Linux

- 未修改 metadata 业务逻辑
- 未修改 `apps/raft_metadata_client.cpp`
- 非 Windows 平台仍走原有测试执行路径

结论：

- 预期不影响 Linux 语义与覆盖目标
- 本次任务未重新执行 Linux 验证

## 是否可以重新执行 T042

可以。

从当前验证结果看，`T042` 所要求的 Windows metadata 相关 CTest 已经通过；如果按流程需要正式重记一次 `T042`，现在可以直接标记为可重跑且预期通过。就本次实际验证而言，`T042` 的技术阻塞已经解除。

## 是否可以进入 T043

可以。

`T042` 的 Windows metadata CTest 阻塞已经消除；如流程允许，下一步可进入 `T043` 的 Windows Metadata Client basic flow 验证。
