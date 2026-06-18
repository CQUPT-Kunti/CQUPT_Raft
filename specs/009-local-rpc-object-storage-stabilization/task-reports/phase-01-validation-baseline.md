# T002 Phase 1 Validation Baseline Confirmation

## Scope

本任务只确认 009 阶段继续以当前 local RPC example 和脚本作为本地 RPC baseline，不写业务代码，不改脚本逻辑，不改测试断言。

## Sources

- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/quickstart.md`
- `specs/009-local-rpc-object-storage-stabilization/contracts/local-rpc-validation.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/local-rpc-object-storage-stabilization-report.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/phase-01-survey.md`

## Confirmed Baseline

- 当前 local RPC example 路径仍为：`examples/object-storage-local-3meta-6store`
- 当前 startup baseline 仍为：`examples/object-storage-local-3meta-6store/qidong.sh`
- 当前 shutdown baseline 仍为：`examples/object-storage-local-3meta-6store/tingzhi.sh`
- 当前 status baseline 仍为：`examples/object-storage-local-3meta-6store/rpc_demo.sh status`
- 当前 roundtrip baseline 仍为：`examples/object-storage-local-3meta-6store/rpc_demo.sh roundtrip`
- 当前静态 topology baseline 仍为：
  - `ViewNode=1`
  - `MetadataNode=3`
  - `StorageNode=6`
- 当前 client baseline 仍为：`storage_client`
- 当前测试文件目录 baseline 仍为：`tests/test_file`
- 当前真实验证调用链仍为：`CreateWritePlan -> WriteChunk -> CommitObject -> Download -> cmp`

## Compatibility Requirement

后续 009 场景可以在此 baseline 上扩展：

- ViewNode self refresh
- 多 ViewNode peer sync
- StorageNode dynamic join
- Metadata learner join

但这些扩展场景都不能把当前静态 local RPC roundtrip baseline 写成“已废弃”或“已被新入口替代”。

## File Existence Checks

本任务执行了轻量文件存在性检查：

- `test -d examples/object-storage-local-3meta-6store`
- `test -f examples/object-storage-local-3meta-6store/qidong.sh`
- `test -f examples/object-storage-local-3meta-6store/tingzhi.sh`
- `test -f examples/object-storage-local-3meta-6store/rpc_demo.sh`
- `test -d tests/test_file`
- `test -f specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `test -f specs/009-local-rpc-object-storage-stabilization/contracts/local-rpc-validation.md`

结果：以上路径均存在。

## Build / Script Execution

- 未执行 targeted build
- 未执行 `qidong.sh` / `rpc_demo.sh status` / `rpc_demo.sh roundtrip` / `tingzhi.sh`
- 原因：本任务为 Phase 1 文档基线确认，只要求确认入口和兼容基线，不要求运行脚本

## Next Step

T002 所需的文档基线已收口，可以进入 `T003`，继续对照 `tests/CMakeLists.txt` 确认 CTest target 与 label 覆盖。

