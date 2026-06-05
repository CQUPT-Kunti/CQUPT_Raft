# T023 任务报告：upload bounded checksum 接口边界

## 1. 修改了哪些文件

- `modules/store/upload/upload_coordinator.h`
- `modules/store/upload/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t023-upload-bounded-checksum-interface.md`

## 2. upload_coordinator.h 如何收口 streaming / bounded checksum 接口

- 新增 `UploadObjectChecksumFacts`，表达对象级 metadata facts：
  - `size`
  - `checksum`
  - `etag`
- 在 `UploadCoordinatorRequest` 中新增 `object_checksum`，作为后续 streaming / bounded checksum 路径向 `CreateObject` / `CommitObject` 提供对象级事实的入口。
- 保留现有 `etag` 字段以兼容旧 metadata 字段和现有调用方，但注释明确新调用方应优先使用 `object_checksum`。
- 为 `UploadChunkInput::payload` 添加边界注释：它只能是单个 bounded chunk 的 data-plane buffer，不能作为整对象常驻内存或 metadata / Raft payload 通道。
- 为 `UploadChunkInput::expected_checksum` 添加边界注释：调用方可通过 chunk streaming / bounded 路径预先填充；后续实现若需要现算，也只能对当前 chunk 计算，不能拼接整对象。

## 3. 如何避免 full-object buffering 和 payload 进入 metadata/Raft

- 头文件接口把对象级 checksum / size / etag 提升为 metadata facts，不要求 coordinator 为生成 etag 拼接完整对象。
- `module-notes.md` 明确当前 `.cpp` 中拼接所有 chunk payload 计算 etag 的 fallback 是 T024 需要替换的 legacy full-object buffering 实现债，不再是 008 后续接口契约。
- 真实 payload 边界保持为 StorageNode data-plane；metadata/control-plane 只接收 `size`、`checksum`、`etag`、chunk identity、offset、replica nodes 等 facts。
- 本任务没有把完整 payload 暴露给 metadata command，也没有新增整文件常驻内存接口。

## 4. 是否发现不合理点 / 警告 / 风险

- 当前 `modules/store/upload/upload_coordinator.cpp` 仍存在 legacy `ComputeObjectEtag` fallback：当请求未带 `etag` 时会拼接所有 chunk payload 再计算对象摘要。T023 已在接口和 module notes 收口边界，T024 必须移除或替换该实现路径。
- `tasks.md` 当前工作区相对 HEAD 还包含 T011 / T016 等任务状态变化；本轮补丁只针对 T023，未主动修改这些其他任务状态，也未回滚可能来自其他工作的改动。
- 全量 `cmake --build --preset debug-ninja-low-parallel` 两次被用户中断，日志均显示 `ninja: build stopped: interrupted by user.`；本次改用受影响目标验证并通过。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`。
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`。
- 原因：本次只调整 upload coordinator 头文件接口边界和模块说明，没有实现新的运行时行为、协议语义或持久化格式。

## 6. 验证命令和结果

### 验证命令

```bash
git diff -- modules/store/upload/upload_coordinator.h modules/store/upload/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t023-upload-bounded-checksum-interface.md
git diff --check -- modules/store/upload/upload_coordinator.h modules/store/upload/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t023-upload-bounded-checksum-interface.md
printf '#include "store/upload/upload_coordinator.h"\nint main() { return 0; }\n' | c++ -std=c++20 -I modules -x c++ -fsyntax-only -
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel --target test_storage_upload_coordinator
```

### 验证结果

- `git diff --check` 无输出。
- `upload_coordinator.h` C++20 include 语法检查通过。
- `cmake --preset debug-ninja-low-parallel`：PASS，用时 5 秒。
- `cmake --build --preset debug-ninja-low-parallel`：未完成，两次均被用户中断；日志路径：
  - `tmp/test-logs/t023-cmake-build.log`
  - `tmp/test-logs/t023-cmake-build-rerun.log`
- `cmake --build --preset debug-ninja-low-parallel --target test_storage_upload_coordinator`：PASS，用时 1 秒。
- 未运行测试用例：本任务只调整接口边界，不改测试；T025 负责补充对应测试。

## 结论

- T023 已完成。
- 从接口和受影响目标编译角度看，可以进入 T024。
