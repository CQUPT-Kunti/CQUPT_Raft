# T005 任务报告

## T005 任务目标

根据 `T005 Implement metadata command codec` 的要求，在不修改现有 KV `Command` 语义和 `command.cpp` 行为的前提下，实现 metadata command 的序列化、反序列化、字段校验与 fingerprint，为后续 `request_id` 幂等冲突识别提供基础。

## 修改了哪些文件

- `modules/raft/common/metadata_command.cpp`
- `specs/005-strong-consistency-metadata-layer/task-reports/t005-report.md`

## 每个文件大概改了什么

### `modules/raft/common/metadata_command.cpp`

- 新增 metadata command codec 实现文件。
- 采用独立于 KV `SET|key|value` / `DEL|key|` 的 `META1` 文本包络格式。
- 实现了：
  - `SerializeMetadataCommand`
  - `ParseMetadataCommand`
  - `ValidateMetadataCommand`
  - `ComputeMetadataCommandFingerprint`
- 支持区分 `create` / `commit` / `delete`。
- 对 create 路径增加字段校验：
  - `request_id` 必填
  - `object_key` 必填
  - `chunk_size` / `chunk_count` 必须大于 0
  - `checksum` 必填
  - `mock_locations` 必填
  - `payload` 大小受限
- 为列表字段和字符串字段实现了转义/反转义，避免直接复用 KV 命令格式。
- fingerprint 使用稳定字段拼接，作为后续 idempotency conflict 判定基础。

### `specs/005-strong-consistency-metadata-layer/task-reports/t005-report.md`

- 新增本次 T005 的独立任务报告。

## 是否执行了验证

- 已执行最小验证：
  - `c++ -std=c++20 -I modules -fsyntax-only modules/raft/common/metadata_command.cpp`
  - 临时 smoke 程序编译并运行，覆盖 create command 的 validate / serialize / parse / fingerprint 和缺失 `request_id` 的拒绝
  - 结果：`PASS`
- 未执行测试目录下的测试。
  - 原因：本次允许读取范围不包含 `tests/**`，且 T007/T008 尚未开始。

## 当前风险或后续事项

- 当前 `metadata_command.h` 还没有为 codec 暴露函数声明；本次实现已落在 `.cpp`，但要让其他翻译单元直接调用，后续需要通过允许的任务补齐头文件声明。
- 当前校验重点覆盖 T005 明确要求的字段与 payload 限制，没有提前实现状态机、service、client 或真实数据面逻辑。
- 当前只为 create record 载荷做了详细校验；commit/delete 的更细粒度业务语义仍应由后续状态机和 service 任务处理。

## 建议 commit message

```text
feat(common): 实现 metadata command codec
```
