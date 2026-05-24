## T048 结果

- 本任务已完成。
- `proto/raft.proto` 当前已无 `KvService`、`KvStatusCode`、`Put/Get/Delete` 相关 message/RPC。
- 本次未修改 `proto/raft.proto` 或 `CMakeLists.txt`，因为 proto 面已经是 metadata-only / Raft-only 分层状态。

## 清理了什么

- 本次主要做精确扫描、确认和验证。
- `raft.proto` 保持仅承载 `RaftService`：
  - `RequestVote`
  - `AppendEntries`
  - `InstallSnapshot`
- metadata 业务 RPC 主面位于 `metadata.proto`，不在 `raft.proto` 中。

## proto 是否仍有 KV service/message 残留

- `proto/raft.proto`：未发现 `KvService` 残留。
- 未发现：
  - `KvStatusCode`
  - `PutRequest` / `GetRequest` / `DeleteRequest`
  - `PutResponse` / `GetResponse` / `DeleteResponse`
  - `kv.proto`
- `CMakeLists.txt` 中也未发现 KV proto target 或 `raft_kv_client` target 残留。

## Linux 验证

- 扫描：
  - `rg -n "KvService|KvStatusCode|PutRequest|GetRequest|DeleteRequest|PutResponse|GetResponse|DeleteResponse|raft_kv_client|kv_service_impl|kv.proto|KV 主路径|KV service|KV client" proto docs README.md tests/README.md CMakeLists.txt`
  - 结果：`raft.proto` 无 KV RPC 面；命中只剩文档中的 legacy / 历史说明。
- 配置：
  - `cmake --preset debug-ninja-low-parallel`
  - 结果：PASS
- 构建：
  - `cmake --build --preset debug-ninja-low-parallel --target raft_proto raft_demo raft_metadata_client`
  - 结果：PASS
- CTest：
  - `ctest --test-dir build/linux --output-on-failure -R "(MetadataClientScenarioTest|MetadataFailoverTest|MetadataMainPathTest)"`
  - 结果：PASS（17/17）
  - 说明：`MetadataMainPathTest` 未匹配到实际测试项；实际执行并通过的是 `MetadataClientScenarioTest` 与 `MetadataFailoverTest`。

## 是否可以进入 T050

- 可以进入 `T050`。
- 当前未发现 proto / CMake 层面的 KV 业务 RPC 残留 blocker。
