# T009 Checksum Helper

- 修改文件
  - `modules/store/common/store_types.h`
  - `modules/store/common/store_types.cpp`
  - `tests/store_types_test.cpp`
  - `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - `specs/007-storage-node-data-plane/tasks.md`
  - `specs/007-storage-node-data-plane/task-reports/t009-checksum-helper.md`

- 做了什么
  - 在 `storedemo` 命名空间下实现 `ComputeChunkChecksum()` 和 `VerifyChunkChecksum()`，当前统一使用 SHA-256，并输出小写十六进制字符串，结果可直接写入 `ChunkChecksum`。
  - 保持 checksum 语义仅用于数据完整性校验，不作为内容寻址、全局去重或引用计数身份。
  - 调整 `ChunkChecksum::IsSet()` 语义，使空 payload 的合法 checksum 也能被识别为已设置。
  - 更新 `tests/store_types_test.cpp`，覆盖相同 payload 稳定、空 payload、二进制 payload、重复计算一致性和 mismatch 识别。
  - 在 `common-risk-notes.md` 中新增一条测试辅助 checksum 语义与生产 helper 不一致的跨任务风险。
  - 在 `tasks.md` 中将 T009 标记为已完成。

- 验证命令和结果
  - `cmake --build --preset debug-ninja-low-parallel`：PASS
  - `ctest --test-dir build/linux -R "store_types" --output-on-failure`：PASS
  - `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`：PASS

- 是否通过 T009
  - 通过。

- 是否可以进入 T010
  - 可以。

- 当前任务发现的不合理点 / 警告 / 风险
  - `.specify/scripts/bash/check-prerequisites.sh` 仍会把当前 feature 解析到 `specs/006-remove-kv-metadata-state-machine`，本任务继续按用户指定的 `specs/007-storage-node-data-plane` 执行。
  - `tests/support/store_test_utils.h` 中的 `MakeChecksumFixture()` 仍是 FNV1a 夹具摘要，不等同于当前 SHA-256 生产 helper。

- 是否修正了高频文档，为什么
  - 是。修改了 `specs/007-storage-node-data-plane/tasks.md`，仅用于将 T009 标记为完成。

- common-risk-notes.md 新增/删除/解决了哪些项
  - 新增 1 项：`tests/support/store_test_utils.h` 的 `MakeChecksumFixture()` 与当前 `storedemo::ComputeChunkChecksum()` 的生产 SHA-256 语义不一致，需在后续 storage 测试工具或 LocalDiskChunkStore 测试任务中统一。
