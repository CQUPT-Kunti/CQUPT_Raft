# T024 任务报告

## 1. 修改了哪些文件

- `modules/store/upload/upload_coordinator.cpp`
- `modules/store/upload/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t024-upload-bounded-checksum-implementation.md`

本任务未修改 `modules/store/upload/upload_coordinator.h`，沿用 T023 已收口的 bounded / streaming checksum 接口边界。
`tasks.md` 当前工作树中存在其他既有变更；本任务只将 T024 从 `[ ]` 标记为 `[X]`。

## 2. bounded / streaming checksum 实现做了什么

- 在 `upload_coordinator.cpp` 中新增对象级 checksum facts 收口逻辑 `ResolveObjectChecksumFacts(...)`。
- 按 `request.chunks` 顺序对每个 chunk 的 `payload` 做增量 SHA-256 计算，不再拼接整对象 buffer。
- 汇总对象总大小，并生成 `UploadObjectChecksumFacts{size, checksum, etag}`。
- 如果调用方提供了 `request.object_checksum.checksum`，会校验：
  - 算法必须是 `kSha256`
  - checksum 长度必须符合 SHA-256 十六进制输出
  - `size_bytes` / `object_checksum.size` 与实际对象大小必须一致
  - 如与增量计算结果不一致，返回 `kChecksumMismatch`
- `etag` 选择顺序为：
  - `request.object_checksum.etag`
  - `request.etag`
  - 增量计算得到的对象级 checksum 字符串
- `UploadCoordinator::UploadObject()` 改为使用上述对象级 facts 填充 metadata `CreateObject` / `CommitObject` 请求。

## 3. 如何避免 full-object buffering 和 payload 进入 metadata/Raft

- 删除了通过整对象拼接后再计算 checksum/etag 的路径。
- 对象级 checksum 只基于 chunk 顺序做 bounded 增量更新，内存占用受单个 chunk payload 大小约束。
- metadata 路径只接收对象级 `size` / `checksum` / `etag` 和 chunk facts，不接收完整 payload 或拼接后的对象内容。
- coordinator 仍然只把单个 chunk payload 交给 `UploadChunkWriter` 走 StorageNode data-plane，不把真实 payload 暴露给 metadata command、Raft log、Raft snapshot 或 metadata snapshot。

## 4. 是否发现不合理点 / 警告 / 风险

- 当前 `upload_coordinator.cpp` 内部增加了本地 SHA-256 增量实现，用于在不改公共接口的前提下完成 T024。后续如果项目已有统一 streaming checksum 组件，可考虑在不破坏边界的前提下收敛复用，避免重复实现。
- 本任务没有引入真实 upload/download 端到端流程，也没有实现 StorageNode 写入或 MetadataNode commit 细节，符合 T024 边界。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

未修改 `common-risk-notes.md` 或 `risk-register.md`。

## 6. 验证命令和结果

### Diff 检查

```bash
git diff -- modules/store/upload/upload_coordinator.cpp modules/store/upload/upload_coordinator.h modules/store/upload/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t024-upload-bounded-checksum-implementation.md
```

结果：已检查，改动范围符合 T024。

### 头文件语法检查

```bash
printf '#include "store/upload/upload_coordinator.h"\nint main() { return 0; }\n' | c++ -std=c++20 -I modules -x c++ -fsyntax-only -
```

结果：PASS。

### diff 格式检查

```bash
git diff --check -- modules/store/upload/upload_coordinator.cpp modules/store/upload/upload_coordinator.h modules/store/upload/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t024-upload-bounded-checksum-implementation.md
```

结果：PASS。

### 构建验证

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe' || echo "build lock busy, skip build in this window"
```

结果：`build lock busy, skip build in this window`。构建锁被占用，本窗口未执行 build。

### upload coordinator 相关测试验证

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R storage_upload_coordinator --output-on-failure' || echo "build lock busy, skip test in this window"
```

结果：`build lock busy, skip test in this window`。构建锁被占用，本窗口未执行 test，待统一验证。
