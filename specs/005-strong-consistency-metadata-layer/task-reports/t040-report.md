# T040 执行报告

## 任务范围

- 任务编号：`T040`
- 任务目标：在 Linux 平台验证 `raft_metadata_client` 的基本运行流程：`create -> head -> list -> commit -> head -> list -> delete -> head -> list`
- 本次仅执行：
  - 启动单节点本地 `raft_demo`
  - 使用固定 `request_id` 和 mock manifest 执行 metadata basic flow
- 本次未执行：
  - `T041` 及后续 Windows 验证
  - 任意源码、测试、CMake、文档修复

## 读取与边界确认

- 已先读取根目录 `NOTREAD.md`，并遵守其中禁止路径。
- 按任务允许范围读取了：
  - `specs/005-strong-consistency-metadata-layer/tasks.md`
  - `specs/005-strong-consistency-metadata-layer/client-design.md`
  - `specs/005-strong-consistency-metadata-layer/validation-matrix.md`
- 为确定本地 demo 启动方式，最小补充读取了：
  - `apps/main.cpp`
  - `config.txt`
- 未读取 `specs/004-raft-industrialization/**`，未全量扫描 `tests/**`，未读取 `build/**` 产物内容。

## 验证平台

- 平台：Linux
- 服务端：`raft_demo`
- 客户端：`raft_metadata_client`
- 运行模式：单节点本地 Raft cluster

## 运行参数

- address：`127.0.0.1:50051`
- object_key：`demo/t040-object`
- create request_id：`req-create-t040`
- commit request_id：`req-commit-t040`
- delete request_id：`req-delete-t040`
- manifest：
  - `object_size=1024`
  - `chunk_size=512`
  - `chunk_count=2`
  - `checksum=sha256:t040-mock`
  - `mock_locations=node-1/chunk-0,node-1/chunk-1`
- payload：metadata-only，`payload_bytes=18`

## 执行命令与结果

### 1. Create

命令：

```bash
./build/linux/raft_metadata_client 127.0.0.1:50051 create \
  --request-id req-create-t040 \
  --object-key demo/t040-object \
  --object-size 1024 \
  --chunk-size 512 \
  --chunk-count 2 \
  --checksum sha256:t040-mock \
  --mock-location node-1/chunk-0 \
  --mock-location node-1/chunk-1 \
  --payload '{"kind":"metadata-only","case":"t040"}' \
  --timeout-ms 3000
```

结果：

- `PASS`
- `code=OK`
- `state=PENDING`
- `leader_id=1`
- `leader_address=127.0.0.1:50051`
- `term=2`
- `log_index=3`

### 2. Create 后 Head / List 不可见

命令：

```bash
./build/linux/raft_metadata_client 127.0.0.1:50051 head --object-key demo/t040-object --timeout-ms 3000
./build/linux/raft_metadata_client 127.0.0.1:50051 list --timeout-ms 3000
```

结果：

- `head`：`PASS`
  - `code=NOT_FOUND`
  - `message="committed record not found"`
  - `found=false`
- `list`：`PASS`
  - `code=OK`
  - `records_count=0`

### 3. Commit

命令：

```bash
./build/linux/raft_metadata_client 127.0.0.1:50051 commit \
  --request-id req-commit-t040 \
  --object-key demo/t040-object \
  --expected-create-request-id req-create-t040 \
  --commit-info "t040 commit" \
  --timeout-ms 3000
```

结果：

- `PASS`
- `code=OK`
- `state=COMMITTED`
- `leader_id=1`
- `leader_address=127.0.0.1:50051`
- `term=2`
- `log_index=4`

### 4. Commit 后 Head / List 可见

命令：

```bash
./build/linux/raft_metadata_client 127.0.0.1:50051 head --object-key demo/t040-object --timeout-ms 3000
./build/linux/raft_metadata_client 127.0.0.1:50051 list --timeout-ms 3000
```

结果：

- `head`：`PASS`
  - `code=OK`
  - `state=COMMITTED`
  - `found=true`
  - 返回 record，字段与 manifest 一致
- `list`：`PASS`
  - `code=OK`
  - `records_count=1`
  - 包含 `demo/t040-object`

### 5. Delete

命令：

```bash
./build/linux/raft_metadata_client 127.0.0.1:50051 delete \
  --request-id req-delete-t040 \
  --object-key demo/t040-object \
  --delete-info "t040 delete" \
  --timeout-ms 3000
```

结果：

- `PASS`
- `code=OK`
- `state=DELETED`
- `leader_id=1`
- `leader_address=127.0.0.1:50051`
- `term=2`
- `log_index=5`

### 6. Delete 后 Head / List 不可见

命令：

```bash
./build/linux/raft_metadata_client 127.0.0.1:50051 head --object-key demo/t040-object --timeout-ms 3000
./build/linux/raft_metadata_client 127.0.0.1:50051 list --timeout-ms 3000
```

结果：

- `head`：`PASS`
  - `code=NOT_FOUND`
  - `message="committed record not found"`
  - `found=false`
- `list`：`PASS`
  - `code=OK`
  - `records_count=0`

## 验收结论

- `raft_metadata_client` Linux basic flow：通过
- create 后不可见：通过
- commit 后可见：通过
- delete 后不可见：通过
- `T040`：通过本次 Linux Metadata Client basic flow 验证

## 边界说明

- 本次使用 metadata-only payload；未读取真实文件，未生成真实 chunk，未访问 `StorageNode` / `ChunkStore`。
- 本次未进入 `T041` 或任何 Windows 验证任务。
- 本次未修改源码、测试、CMake、高频文档。
