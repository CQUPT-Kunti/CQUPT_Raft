## T049 结果

- 本任务已完成。
- 已把指定文档中的“当前主路径”描述统一收敛为 metadata-only。
- `README.md` 当前为空文件，本次无需修改。

## 清理了什么

- `docs/CURRENT_INDUSTRIALIZATION_ANALYSIS.md`
  - 将 app / service / proto 主路径描述改为 `RaftService + MetadataService + raft_metadata_client`
  - 将 `KvService` / `raft_kv_client` 调整为已退役或 legacy 语境
  - 修正 `service` / `proto` / `state_machine abstraction` 相关现状描述
- `docs/PERSISTENCE_DURABILITY_CONTRACT.md`
  - 将当前 snapshot 主路径中的 `KvStateMachine::SaveSnapshot()` 改为通用状态机 `SaveSnapshot()` 描述
  - 避免把 KV 状态机写成当前持久化主路径
- `tests/README.md`
  - 删除已退役的 `test_kv_service` / `test_kv_service.cpp` 受管回归与说明残留
- `README.md`
  - 无修改；文件为空，无 KV 主路径描述残留

## 文档是否仍把 KV 描述为当前主路径

- 指定清理范围内的文档：否。
- 仍可见的 KV 相关词条仅保留在以下语境：
  - legacy / blocker / 兼容残留说明
  - 历史迁移背景说明
- 额外说明：
  - `docs/CONSISTENCY_LAYER_TRANSITION_PLAN.md` 仍有 `KvService` 历史迁移说明，但不是“当前主路径”表述，本次按任务范围未改动。

## Linux 验证

- 扫描：
  - `rg -n "KvService|KvStatusCode|PutRequest|GetRequest|DeleteRequest|PutResponse|GetResponse|DeleteResponse|raft_kv_client|kv_service_impl|kv.proto|KV 主路径|KV service|KV client" proto docs README.md tests/README.md CMakeLists.txt`
  - 结果：指定清理范围内不再把 KV 写成当前主入口；命中仅剩 legacy / 历史说明
- 配置：
  - `cmake --preset debug-ninja-low-parallel`
  - 结果：PASS
- 构建：
  - `cmake --build --preset debug-ninja-low-parallel --target raft_proto raft_demo raft_metadata_client`
  - 结果：PASS
- CTest：
  - `ctest --test-dir build/linux --output-on-failure -R "(MetadataClientScenarioTest|MetadataFailoverTest|MetadataMainPathTest)"`
  - 结果：PASS（17/17）

## 是否可以进入 T050

- 可以进入 `T050`。
- 当前未发现文档主路径描述方面的 KV 残留 blocker。
