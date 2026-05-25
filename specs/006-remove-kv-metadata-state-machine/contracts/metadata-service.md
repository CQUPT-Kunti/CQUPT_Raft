# Contract: Metadata Service Surface After KV Removal

## Goal

定义 `006` 完成后的对外 gRPC 契约形态，重点是：

- `KvService` 从主 proto 删除
- `MetadataService` 成为唯一业务服务
- 状态/健康/指标不再挂在 KV 命名空间下
- bucket/object 生命周期替代 record-centric V1 RPC

## Preserved Surface

### Unchanged

- `RaftService`
  - `RequestVote`
  - `AppendEntries`
  - `InstallSnapshot`

这些 RPC 的语义、字段编号和调用方保持不变。

## Removed Surface

- `KvService`
- `PutRequest` / `PutResponse`
- `GetRequest` / `GetResponse`
- `DeleteRequest` / `DeleteResponse`
- `KvStatusCode`
- `raft_kv_client`

## MetadataService V2

### Service Methods

```text
service MetadataService {
  rpc CreateBucket(CreateBucketRequest) returns (CreateBucketResponse);
  rpc DeleteBucket(DeleteBucketRequest) returns (DeleteBucketResponse);
  rpc CreateObject(CreateObjectRequest) returns (CreateObjectResponse);
  rpc CommitObject(CommitObjectRequest) returns (CommitObjectResponse);
  rpc AbortObject(AbortObjectRequest) returns (AbortObjectResponse);
  rpc DeleteObject(DeleteObjectRequest) returns (DeleteObjectResponse);
  rpc HeadObject(HeadObjectRequest) returns (HeadObjectResponse);
  rpc ListObjects(ListObjectsRequest) returns (ListObjectsResponse);
}
```

### Common Write Fields

| Field | Applies To | Meaning |
|-------|------------|---------|
| `request_id` | all writes | 幂等键 |
| `bucket_name` | all bucket/object ops | bucket 名 |
| `object_key` | object ops | bucket 内对象键 |
| `client_timestamp` | optional | 仅诊断用途 |

### Status Code Set

| Code | Meaning |
|------|---------|
| `OK` | 首次成功 |
| `NOT_LEADER` | 当前节点不是 leader |
| `INVALID_ARGUMENT` | 字段或状态前置条件非法 |
| `NOT_FOUND` | 目标 bucket/object 不存在或不可见 |
| `IDEMPOTENT_REPLAY` | 相同 request_id 同内容重放 |
| `IDEMPOTENCY_CONFLICT` | 相同 request_id 不同内容 |
| `STATE_CONFLICT` | 生命周期转换非法 |
| `OVERLOADED` | admission queue / backpressure 拒绝 |
| `TIMEOUT` | 在 deadline 内未确认最终结果 |
| `NODE_STOPPING` | 节点停止中，不再接收新写入 |
| `INTERNAL_ERROR` | 非预期内部失败 |

## Object Operation Semantics

### CreateBucket

- 成功创建 bucket namespace
- 相同 request_id + 相同 bucket => replay
- bucket 已存在 => `STATE_CONFLICT`

### DeleteBucket

- 仅当 bucket 下无 active object facts 时成功
- bucket 非空 => `STATE_CONFLICT`
- bucket 不存在 => `NOT_FOUND`

### CreateObject

- 创建 `PENDING` lifecycle
- 成功后 `HeadObject` / `ListObjects` 不可见
- bucket 不存在 => `NOT_FOUND`
- 同 bucket/object 上已有 active lifecycle => `STATE_CONFLICT`

### CommitObject

- 仅允许 `PENDING -> COMMITTED`
- 成功后 `HeadObject` / `ListObjects` 可见
- stale commit / wrong lifecycle => `NOT_FOUND` 或 `STATE_CONFLICT`

### AbortObject

- 仅允许终止 `PENDING`
- 成功后对象不可见
- 不引入第四个对外状态

### DeleteObject

- 仅允许删除当前 `COMMITTED` lifecycle
- 成功后对象不可见，并写 tombstone facts

### HeadObject

- 只返回当前 `COMMITTED` 对象
- follower 调用 => `NOT_LEADER`

### ListObjects

- 只列出当前 bucket 下 `COMMITTED` 对象
- 返回确定性顺序
- follower 调用 => `NOT_LEADER`

## Non-KV Admin Surface

为保留现有诊断能力，本计划建议把以下 RPC 从 `KvService` 中剥离出来，迁移到单独的 non-KV 管理面服务，例如：

```text
service NodeAdminService {
  rpc Status(StatusRequest) returns (StatusResponse);
  rpc Health(HealthRequest) returns (HealthResponse);
  rpc Metrics(MetricsRequest) returns (MetricsResponse);
}
```

### Why Not Keep Them In KvService

- 继续挂在 `KvService` 名下会保留 KV service surface
- 与 metadata 业务模型无关，但对集群诊断仍有价值

## Compatibility Boundary

- 不保留 `KvService` compatibility mode
- 不保留 record-centric `CreateMetadataRecord/CommitMetadataRecord/DeleteMetadataRecord` 作为正式主路径
- 旧 client 命中新服务时应在编译期或 proto 生成期失败，而不是在运行时 silent fallback
