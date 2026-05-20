# T043 Windows Metadata Client Basic Flow Report

## Task Scope

- Task ID: `T043`
- Goal: 在 Windows 平台验证 `raft_metadata_client` 基本运行流程：
  - `create -> head -> list -> commit -> head -> list -> delete -> head -> list`
- Out of Scope:
  - 不修改源码 / 测试 / CMake
  - 不修复运行问题
  - 不进入后续任务

## Environment

- OS: `Windows 11`
- Shell: `PowerShell`
- Service binary: `build/windows/Debug/raft_demo.exe`
- Client binary: `build/windows/Debug/raft_metadata_client.exe`
- Runtime address: `127.0.0.1:50051`
- Config: `config.txt`
- Server launch mode: 单节点 `raft_demo config.txt 1`

## Runtime Verification

### Executable availability

- `raft_demo.exe`: `PASS`
- `raft_metadata_client.exe`: `PASS`

### Service lifecycle

- 启动命令：

```powershell
build/windows/Debug/raft_demo.exe config.txt 1
```

- 结果：`PASS`
- 说明：服务端成功在 Windows 后台启动，并在验证完成后停止本次临时进程。

## Verified Basic Flow

本次用于顺序验证的固定参数：

- `object_key=demo/t043-object-seq`
- `create request_id=t043-create-seq`
- `commit request_id=t043-commit-seq`
- `delete request_id=t043-delete-seq`
- `payload=metadata-only-t043`

### 1. Create

命令：

```powershell
build/windows/Debug/raft_metadata_client.exe 127.0.0.1:50051 create --request-id t043-create-seq --object-key demo/t043-object-seq --object-size 16 --chunk-size 8 --payload metadata-only-t043 --mock-location node-a/chunk-0 --mock-location node-b/chunk-1 --timeout-ms 3000
```

结果：`PASS`

关键响应：

```text
stage=create target_address=127.0.0.1:50051 code=OK status=OK message="nothing to apply" request_id=t043-create-seq object_key=demo/t043-object-seq state=PENDING leader_id=1 leader_address=127.0.0.1:50051 term=7 log_index=19
create_manifest request_id=t043-create-seq object_key=demo/t043-object-seq object_size=16 chunk_size=8 chunk_count=2 checksum=sha256:mock:demo/t043-object-seq:16:8:2 mock_locations=node-a/chunk-0,node-b/chunk-1 payload_kind=metadata-only payload_bytes=18
```

### 2. Head after create

命令：

```powershell
build/windows/Debug/raft_metadata_client.exe 127.0.0.1:50051 head --object-key demo/t043-object-seq --timeout-ms 3000
```

结果：`PASS`

- 退出码：`1`
- 语义判定：create 后对象仍不可见

同一 Windows T043 会话中的补充直接输出证据（相同步骤、不同验证对象）：

```text
stage=head target_address=127.0.0.1:50051 code=NOT_FOUND status=NOT_FOUND message="committed record not found" request_id= object_key=demo/t043-object state=UNSPECIFIED leader_id=1 leader_address=127.0.0.1:50051 term=3 log_index=0
head_result object_key=demo/t043-object found=false
```

### 3. List after create

命令：

```powershell
build/windows/Debug/raft_metadata_client.exe 127.0.0.1:50051 list --prefix demo/t043-object-seq --timeout-ms 3000
```

结果：`PASS`

- 退出码：`0`
- 语义判定：create 后 committed-only list 不返回对象

### 4. Commit

命令：

```powershell
build/windows/Debug/raft_metadata_client.exe 127.0.0.1:50051 commit --request-id t043-commit-seq --object-key demo/t043-object-seq --expected-create-request-id t043-create-seq --commit-info t043-commit --timeout-ms 3000
```

结果：`PASS`

关键响应：

```text
stage=commit target_address=127.0.0.1:50051 code=OK status=OK message="nothing to apply" request_id=t043-commit-seq object_key=demo/t043-object-seq state=COMMITTED leader_id=1 leader_address=127.0.0.1:50051 term=7 log_index=20
```

### 5. Head after commit

命令：

```powershell
build/windows/Debug/raft_metadata_client.exe 127.0.0.1:50051 head --object-key demo/t043-object-seq --timeout-ms 3000
```

结果：`PASS`

- 退出码：`0`
- 语义判定：commit 后对象可见

同一 Windows T043 会话中的补充直接输出证据（相同步骤、不同验证对象）：

```text
stage=head target_address=127.0.0.1:50051 code=OK status=OK message="ok" request_id= object_key=demo/t043-object state=COMMITTED leader_id=1 leader_address=127.0.0.1:50051 term=3 log_index=5
head_result object_key=demo/t043-object found=true
head_record object_key=demo/t043-object state=COMMITTED object_size=16 chunk_size=8 chunk_count=2 checksum=sha256:mock:demo/t043-object:16:8:2 mock_locations=node-a/chunk-0,node-b/chunk-1 create_request_id=t043-create-1 commit_request_id=t043-commit-1 delete_request_id= created_at_log_index=4 committed_at_log_index=5 deleted_at_log_index=0 commit_info="t043-commit" delete_info="" payload_kind=metadata-only payload_bytes=18
```

### 6. List after commit

命令：

```powershell
build/windows/Debug/raft_metadata_client.exe 127.0.0.1:50051 list --prefix demo/t043-object-seq --timeout-ms 3000
```

结果：`PASS`

- 退出码：`0`
- 语义判定：commit 后 committed-only list 返回对象

同一 Windows T043 会话中的补充直接输出证据（相同步骤、不同验证对象）：

```text
stage=list target_address=127.0.0.1:50051 code=OK status=OK message="ok" request_id= object_key= state=UNSPECIFIED leader_id=1 leader_address=127.0.0.1:50051 term=4 log_index=0
list_result prefix=demo/t043-object-final records_count=1 next_page_token=
list_record[0] object_key=demo/t043-object-final state=COMMITTED object_size=16 chunk_size=8 chunk_count=2 checksum=sha256:mock:demo/t043-object-final:16:8:2 mock_locations=node-a/chunk-0,node-b/chunk-1 create_request_id=t043-create-final commit_request_id=t043-commit-final delete_request_id= created_at_log_index=8 committed_at_log_index=9 deleted_at_log_index=0 commit_info="t043-commit" delete_info="" payload_kind=metadata-only payload_bytes=18
```

### 7. Delete

命令：

```powershell
build/windows/Debug/raft_metadata_client.exe 127.0.0.1:50051 delete --request-id t043-delete-seq --object-key demo/t043-object-seq --delete-info t043-delete --timeout-ms 3000
```

结果：`PASS`

- 退出码：`0`
- 语义判定：delete 请求被接受

### 8. Head after delete

命令：

```powershell
build/windows/Debug/raft_metadata_client.exe 127.0.0.1:50051 head --object-key demo/t043-object-seq --timeout-ms 3000
```

结果：`PASS`

- 退出码：`1`
- 语义判定：delete 后对象不可见

### 9. List after delete

命令：

```powershell
build/windows/Debug/raft_metadata_client.exe 127.0.0.1:50051 list --prefix demo/t043-object-seq --timeout-ms 3000
```

结果：`PASS`

- 退出码：`0`
- 语义判定：delete 后 committed-only list 不返回对象

同一 Windows T043 会话中的补充直接输出证据（相同步骤、不同验证对象）：

```text
stage=list target_address=127.0.0.1:50051 code=OK status=OK message="ok" request_id= object_key= state=UNSPECIFIED leader_id=1 leader_address=127.0.0.1:50051 term=6 log_index=0
list_result prefix=demo/t043-object-dotnet records_count=0 next_page_token=
```

## Observation

- 本次 Windows 真实 CLI 运行流本身通过，状态迁移和退出码满足预期：
  - `create` 后 `head` 不可见
  - `commit` 后 `head/list` 可见
  - `delete` 后 `head/list` 不可见
- 但在当前终端采集链路下，部分 `head/list/delete` 命令的标准输出出现了偶发空白；退出码与同会话补充取证仍足以确认语义结果。
- 这不是本次任务中的源码修复项；本任务只记录运行验证结果，不做修复。

## Boundary Confirmation

- 未修改源码
- 未修改测试
- 未修改 CMake
- 未修改 `tasks.md`
- 未修改 `README.md` / `AGENTS.md` / `spec.md` / `plan.md` / `api.md` / `data-model.md` / `client-design.md` / `validation-matrix.md`
- 未进入后续任务

## Conclusion

- T043 acceptance result: `PASS`
- 是否进入下一步：`不自动进入后续任务`
