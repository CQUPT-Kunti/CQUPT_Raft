# T057 - integrated quorum CMake 接入

## 1. 修改了哪些文件

- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t057-integrated-quorum-cmake.md`

说明：

- `tests/CMakeLists.txt` 本任务未再修改
- `tests/integrated_object_storage_quorum_test.cpp` 本任务未修改

## 2. integrated_object_storage_quorum target 接入做了什么

本任务对现有工作区状态做了核对和验证，确认 `tests/CMakeLists.txt` 已具备 T057 需要的接入：

- 已通过 `add_raft_gtest(...)` 注册 `test_integrated_object_storage_quorum`
- source 为 `tests/integrated_object_storage_quorum_test.cpp`
- 依赖沿用项目现有测试风格：
  - `raft_core`
  - `GTest::gtest_main`
- 已存在自定义 target：
  - `integrated_object_storage_quorum`
  - 通过 `DEPENDS test_integrated_object_storage_quorum` 支持单独构建
- `gtest_discover_tests(...)` 保持 `DISCOVERY_MODE PRE_TEST`，不会改变已有测试入口语义

因为当前工作区中的 `tests/CMakeLists.txt` 已经满足 T057 目标，所以本任务没有为“制造改动”而重复改写该文件，只完成了验收、验证、任务状态更新和报告记录。

## 3. 添加或补齐了哪些 CTest label

当前已确认 `integrated_object_storage_quorum` 使用：

- `integrated-object-storage`
- `integrated-object-storage-quorum`
- `platform-neutral`

标签来源是现有 `RAFT_008_LABELS_QUORUM`，语义满足：

- 可按 quorum 安全边界过滤
- 可按 008 integrated object storage 过滤
- 保持 platform-neutral 解释口径

本任务未新增额外 label，也未修改已有 label 语义。

## 4. 是否保持已有测试 target、label、group、preset 不变

是。

- 未重命名已有测试 target
- 未修改已有 label 集合定义
- 未改变 `gtest_discover_tests` 注册方式
- 未修改已有 CTest preset
- 未把该测试强行并入新的必跑集合

## 5. 是否发现不合理点 / 警告 / 风险

- 当前 `tests/CMakeLists.txt` 中 T057 需要的接入实际上已经存在，但 `tasks.md` 仍未勾选；本任务已按实际工作区状态完成验收并补齐任务状态
- 本次 `ctest --preset debug-tests -R integrated_object_storage_quorum --output-on-failure` 因构建锁被占用未执行，因此本窗口只验证了“可单独 configure/build”，未完成“本窗口内单独运行”
- 从当前 CMake 接入看，`integrated_object_storage_quorum` 的单独构建路径已成立；若后续要把它纳入更细的测试分组，需要在不破坏现有 label 语义的前提下另行安排

## 6. 是否修改 `common-risk-notes.md` 或 `risk-register.md`

未修改。

- `common-risk-notes.md`：未修改
- `risk-register.md`：未修改

## 7. 验证命令和结果

### diff 检查

命令：

```bash
git diff -- tests/CMakeLists.txt \
  tests/integrated_object_storage_quorum_test.cpp \
  specs/008-integrated-object-storage-system/tasks.md \
  specs/008-integrated-object-storage-system/task-reports/t057-integrated-quorum-cmake.md
```

结果：

- `tests/CMakeLists.txt`：本任务未新增改动
- `tests/integrated_object_storage_quorum_test.cpp`：本任务未新增改动
- `tasks.md`：仅将 T057 从 `[ ]` 改为 `[X]`
- 任务报告文件为新增文件

### 最小构建验证

命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target integrated_object_storage_quorum' \
|| echo "build lock busy, skip integrated_object_storage_quorum build in this window"
```

结果：

- PASS
- `cmake --preset debug-ninja-safe` 配置成功
- `cmake --build --preset debug-ninja-safe --target integrated_object_storage_quorum` 执行成功
- 本次输出为 `ninja: no work to do.`，说明该 target 已可单独构建

### 单独测试运行验证

命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R integrated_object_storage_quorum --output-on-failure' \
|| echo "build/test lock busy, skip integrated_object_storage_quorum test in this window"
```

结果：

- 构建锁被占用，本窗口未执行 `integrated_object_storage_quorum` 单独测试，待统一验证
