# T014 Report

## T014 任务目标

在 `proto/raft.proto` 中新增 `MetadataService` 及 metadata 专用 message / enum，只定义协议契约，不实现 service adapter、状态机或客户端。

## 修改了哪些文件

- `proto/raft.proto`

## 每个文件大概改了什么

- `proto/raft.proto`
  - 新增 `MetadataService`，包含 `CreateMetadataRecord`、`CommitMetadataRecord`、`DeleteMetadataRecord`、`HeadMetadataRecord`、`ListMetadataRecords`。
  - 新增 `MetadataStatusCode`，覆盖 `OK`、`NOT_LEADER`、`INVALID_ARGUMENT`、`NOT_FOUND`、`IDEMPOTENT_REPLAY`、`IDEMPOTENCY_CONFLICT`、`STATE_CONFLICT`、`INTERNAL_ERROR`、`TIMEOUT`。
  - 新增 `MetadataRecordState`，覆盖 `Pending`、`Committed`、`Deleted`。
  - 新增 `MetadataManifest`、`MetadataRecord`、`MetadataLeaderHint`、`MetadataResponseSummary`。
  - 新增 metadata 专用 request / response message，表达 `request_id`、`object_key`、manifest、payload、term、`log_index`、leader hint、committed-only 查询结果等字段。

## 是否执行了验证

- 已执行：
  - `cmake --preset debug-ninja-low-parallel`
  - `cmake --build --preset debug-ninja-low-parallel --target raft_proto`
- 结果：通过，`raft.pb.cc/.h` 与 `raft.grpc.pb.cc/.h` 生成并编译成功。

## 当前风险或后续事项

- 本次只完成协议层定义，未实现 `MetadataService` adapter。
- 后续 T015 需要根据这些 message / enum 接入 service 层，并同步处理 generated code 的实际调用路径。

## 建议 commit message

```text
feat(proto): 新增 metadata service 协议契约
```
