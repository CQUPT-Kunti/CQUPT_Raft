# T005 任务报告：CMake planned placeholders / guarded entries

## 1. 修改了哪些文件

- `CMakeLists.txt`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t005-cmake-planned-placeholders.md`

## 2. CMake 中为哪些模块或 app target 添加了 planned placeholder / guarded entry

### 规划模块占位

在 `CMakeLists.txt` 中新增了一个安全 helper：

- `raft_collect_existing_sources(...)`

并为以下 008 规划模块预留了源码接入位置：

- `modules/cluster/cluster_config.cpp`
- `modules/cluster/node_identity.cpp`
- `modules/view/view_registry.cpp`
- `modules/view/view_service_impl.cpp`
- `modules/view/view_client.cpp`
- `modules/store/transfer/object_transfer.cpp`
- `modules/store/transfer/metadata_transfer_client.cpp`
- `modules/store/transfer/storage_transfer_client.cpp`

接入方式是：

- 只有当这些 `.cpp` 文件真实存在时，才通过 `target_sources(raft_core ...)` 安全并入 `raft_core`
- 如果文件不存在，只输出清晰的 `STATUS` 提示，不会导致 configure / build 失败

### 规划 app target 占位

在 `CMakeLists.txt` 中新增了一个安全 helper：

- `raft_add_optional_app_target(target_name source_file)`

并为以下 planned app target 增加了 guarded entry：

- `view_node_app`
- `metadata_node_app`
- `storage_node_app`
- `storage_client`
- `storage_bench`

接入方式是：

- 只有当对应 `apps/*.cpp` 文件存在时，才创建 target
- 如果入口文件不存在，则跳过该 target 并输出 `STATUS` 提示
- 当前仓库里这些 planned app 源文件尚不存在，因此本次不会新增实际可构建 target，也不会破坏现有构建路径

## 3. 是否保持已有 target 名称不变

- 是。
- 未修改已有 target 名称：
  - `raft_proto`
  - `raft_core`
  - `raft_demo`
  - `raft_metadata_client`
  - `metadata_proto`
  - `storage_node_proto`
- 仅新增了 008 规划 target 的 guarded placeholder 入口。

## 4. 是否发现不合理点 / 警告 / 风险

- 当前 `tasks.md` 中的 T005 表述包含 “source/header placeholders”，但现有顶层 CMake 实际主要以 `.cpp` 为接入点，头文件不会单独进入构建图；本次按最小必要修改处理为“源码存在时安全接入”，没有额外制造 header-only 假接入。
- 规划中的 `view_node_app` 等 target 后续如果需要额外依赖（例如未来的 `view_proto`），仍需在后续任务里补充；本次只完成不破坏当前构建的 placeholder wiring。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`。
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`。
- 原因：本次仅补充 CMake 占位和 guard，没有引入新的设计风险，也没有改变已有行为边界。

## 6. 验证命令和结果

### 验证命令

```bash
git diff -- CMakeLists.txt specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t005-cmake-planned-placeholders.md
cmake --list-presets
cmake --preset debug-ninja-low-parallel
```

### 验证结果

- `git diff` 范围符合预期，只包含 `CMakeLists.txt`、`tasks.md` 的 T005 勾选状态和本任务报告。
- `cmake --list-presets` 正常输出 `debug-ninja-low-parallel` 和 `debug-ninja-safe`。
- `cmake --preset debug-ninja-low-parallel` 实际执行成功，configure / generate 完成，build files 已写入 `build/linux`。
- configure 过程中按预期输出了 008 placeholder 的 guard 提示：
  - planned `raft_core` 源文件尚不存在，因此未被强制接入
  - `view_node_app`、`metadata_node_app`、`storage_node_app`、`storage_client`、`storage_bench` 对应入口文件尚不存在，因此 target 被安全跳过
- configure 过程中还出现了一条与本次改动无关的既有 CMake dev warning：
  - `tests/CMakeLists.txt` 的 `FetchContent_Declare` 触发 `DOWNLOAD_EXTRACT_TIMESTAMP` / `CMP0135` 提示
  - 该 warning 未导致 configure 失败，也不是本任务引入的新问题

## 结论

- T005 已完成。
- 从构建占位和安全 guard 角度看，可以进入 T006。
