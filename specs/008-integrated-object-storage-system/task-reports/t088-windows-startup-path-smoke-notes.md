# T088 Windows startup and path smoke notes 报告

## 1. 修改了哪些文件

- `specs/008-integrated-object-storage-system/quickstart.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t088-windows-startup-path-smoke-notes.md`

未修改：

- 代码
- 测试
- `CMakeLists.txt` / `tests/CMakeLists.txt`
- `proto/`

## 2. quickstart.md 新增或更新了哪些 Windows startup notes

- 新增 `Windows Startup And Path Smoke Notes` 章节，明确这是 Windows smoke/fallback 边界，不等价于 Linux full validation。
- 明确 Windows 下四个可执行文件名称：
  - `view_node_app.exe`
  - `metadata_node_app.exe`
  - `storage_node_app.exe`
  - `storage_client.exe`
- 补充 PowerShell 命令使用方式：
  - 使用 `.\xxx.exe`
  - 多行命令使用反引号续行
  - 路径含空格时加双引号
- 明确 `--config`、`--node_id`、`--data_dir`、`--listen` 在 Windows 下与 Linux 参数名一致，没有单独的 Windows CLI 变体。
- 说明 `metadata_node_app.exe` 的 `--data_dir` / `--listen` 属于校验型 override，而 `view_node_app.exe` / `storage_node_app.exe` 是受控本地测试 override。
- 修正了 quickstart 里已经过期的 Linux 说明：
  - `metadata_node_app` 已接入 ViewNode registration / heartbeat loop
  - `storage_node_app` 已接入 ViewNode registration / heartbeat loop
  - 不再把这两项写成“后续任务”

## 3. quickstart.md 新增或更新了哪些 Windows path / durability / smoke notes

- 补充 Windows 路径注意事项：
  - 反斜杠路径写法
  - 相对路径与绝对路径示例
  - 含空格路径需要引号
  - 避免保留文件名和非法字符
  - 路径过深时建议开启长路径支持
- 补充 `data_dir` / `node.identity` 预期：
  - 首次启动创建 `node.identity`
  - 重启复用
  - mismatch 必须失败，不能静默覆盖
- 补充 Windows durability expectation：
  - required durability operation 不能 no-op success
  - 预期使用 `FlushFileBuffers`、`MoveFileExW` 或等价 publish 语义
  - 若无法提供等价保证，必须返回明确错误或记录较弱 contract
- 补充临时目录、端口和防火墙说明：
  - 不能假设 `/tmp`
  - 多进程重启前确认端口已释放
  - 本地 smoke 优先 `127.0.0.1`
  - 首次监听可能受 Windows Defender Firewall 影响
- 补充 Windows smoke 范围：
  - `generate-config`
  - `--help` / 参数检查
  - 最小启动
  - `node.identity` 创建与复用
  - 可选最小 upload/download smoke
- 补充 Windows build/test fallback：
  - Linux 的 `flock` 不能直接照搬到 Windows
  - Windows 需要单 build 目录单写者的串行策略

## 4. 是否准确区分 Linux full validation、Windows smoke/fallback、Windows 待测

- 是。
- quickstart.md 明确区分了：
  - `Linux 已验证`
  - `Windows smoke/fallback`
  - `Windows 待实机验证`
- 新增内容没有把 Windows 路径、durability、startup 或 upload/download smoke 写成已通过。
- 同时保留了 Linux 主流程和 Linux-first 的完整验收定位，没有把 Linux-only 验收误写成 Windows 已验证。

## 5. 是否发现不合理点 / 警告 / 风险

- 原 quickstart 中“`storage_node_app` 当前不会在 app 内实现完整的 ViewNode registration / heartbeat loop”已经过期；本次已按当前实现修正。
- 当前 quickstart 里虽然已有少量 Windows 命令示例，但此前缺少对：
  - PowerShell 路径与引号
  - `node.identity` 复用
  - Windows durability contract
  - 临时目录 / 防火墙 / 端口释放
  - `flock` 不适用于 Windows
  的系统说明；本次已补齐。
- 本次没有新增任何“Windows 已通过”的声明，仍需后续 Windows 实机验证支撑。

## 6. 是否没有修改代码、测试、CMake、proto

- 是。
- 本任务只更新了文档和任务状态：
  - `quickstart.md`
  - `tasks.md`
  - T088 任务报告

## 7. 验证命令和结果；如果未运行构建，说明因为本任务为文档更新任务

执行命令：

```bash
git diff -- specs/008-integrated-object-storage-system/quickstart.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t088-windows-startup-path-smoke-notes.md
git diff --check -- specs/008-integrated-object-storage-system/quickstart.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t088-windows-startup-path-smoke-notes.md
rg -n -e "--config" -e "--node_id" -e "--data_dir" -e "--listen" -e "generate-config" -e "upload" -e "download" -e "status" apps/storage_client.cpp apps/view_node_app.cpp apps/metadata_node_app.cpp apps/storage_node_app.cpp
```

结果：

- `git diff`：PASS，T088 相关文档改动清晰可见。
- `git diff --check`：PASS，未发现空白错误。
- 参数核对：PASS，quickstart.md 中引用的 app 参数与 target 名称与当前实现一致。

说明：

- 本任务是文档更新任务，默认不需要 `cmake configure/build/ctest`。
- 本次没有运行构建或 smoke test，也没有把未执行的 Windows smoke 写成 PASS。
