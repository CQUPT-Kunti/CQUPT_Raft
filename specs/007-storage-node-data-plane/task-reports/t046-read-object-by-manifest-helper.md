# T046 Read Object By Manifest Helper

- 修改文件
  - `tests/support/storage_read_test_utils.h`
  - `tests/storage_read_integration_test.cpp`
  - `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - `specs/007-storage-node-data-plane/task-reports/t046-read-object-by-manifest-helper.md`
  - `specs/007-storage-node-data-plane/tasks.md`

- 做了什么
  - 新增 `tests/support/storage_read_test_utils.h`，把测试侧 `ReadObject by manifest` 逻辑抽成 header-only helper。
  - 新增 `storedemo::test::CountingReplicaReader`，统一统计 data-plane 读取次数、chunk 顺序和副本调用顺序。
  - 新增 `storedemo::test::ReadObjectByManifestRequest` / `ReadObjectByManifestResult`，明确 helper 的输入输出边界。
  - 新增 `storedemo::test::ReadObjectByManifest(...)`，固定 metadata lookup first、committed-only gate、manifest offset ordering、replica selection / fallback、checksum/size 对齐语义。
  - 将 `tests/storage_read_integration_test.cpp` 中原本内联的 helper 和计数 reader 删除，改为复用 `tests/support/storage_read_test_utils.h`。

- ReadObject by manifest helper 输入、输出、成功/失败语义
  - 输入：
    - `MetadataStateMachine`
    - `CountingReplicaReader`
    - `ReadObjectByManifestRequest`
      - `bucket`
      - `object_key`
      - `request_id_prefix`
      - 可选 `candidate_resolver`
  - 输出：
    - `ReadObjectByManifestResult`
      - `status`
      - `error_detail`
      - `payload`
      - `chunk_results`
  - 成功语义：
    - helper 先调用 `HeadObject`
    - 只有 `HeadObject` 返回 committed object 时才继续读 manifest 和 data-plane
    - manifest 会按 `offset`（再按 `chunk_id` 兜底）排序后依次读取
    - 每个 chunk 会复用 T045 的 `SelectReadReplicas(...)` 和 `ReadChunkWithReplicaFallback(...)`
    - 读取成功后要求 `metadata.size` 和 `metadata.checksum.value` 与 committed manifest 一致，再把 payload 拼接到最终结果
  - 失败语义：
    - `PENDING / DELETED / not found` 都在 metadata gate 处返回，不触发 data-plane read
    - manifest 缺失返回 `kNotFound`
    - manifest `replica_nodes` 为空返回 `kInvalidArgument`
    - 任一 chunk 的 fallback 失败时，helper 直接返回该明确错误，不继续拼接后续 chunk
    - helper 不决定 object committed 可见性，也不做 repair / scrub / quarantine 写回

- 是否使用 `tests/test_file/test_file.deb`
  - 是。`storage_read_integration_test` 现有 committed/pending/deleted/fallback 场景继续使用 `tests/test_file/test_file.deb`。

- 验证命令、PASS/FAIL、日志路径
  - `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - `ctest --test-dir build/linux -R "storage_read|read_object" --output-on-failure 2>&1 | tee tmp/007/t046-read-object-helper.log`
  - PASS
  - 日志路径：`tmp/007/t046-read-object-helper.log`
  - 说明：仓库当前没有单独的 `read_object` 测试 target，实际命中的是 `storage_read_integration` 和 `storage_read_chunk_contract`

- 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志摘要
  - 本次验证最终 PASS，无失败项。

- Windows 验证判断
  - 本任务只新增测试 helper 和平台无关读取集成测试。
  - 当前无 Windows 编译/测试环境，不伪造 Windows PASS。
  - 本任务未新增 `T046-WIN`。

- 是否通过 T046
  - 是。

- 是否可以进入 T047
  - 可以。T046 已把测试侧读取逻辑集中到 `tests/support`，T047 可以直接在该 helper 之上继续覆盖副本失败、checksum mismatch 和 quarantine 相关场景。

- 当前任务发现的不合理点 / 警告 / 风险
  - 当前 helper 仍主要复用 T045 的最小副本事实语义，默认不接真实 registry / heartbeat / failure cache。
  - helper 虽然支持可选 `candidate_resolver`，但当前仓库还没有真实 runtime 节点事实来源，后续仍需由 T047/T066 继续补强。
  - 当前 helper 只做测试侧复用，不是生产 API，也不会触发 repair / scrub / corruption healing。
  - `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`。

- 是否更新 module-notes.md / AGENTS.md
  - 未更新 `module-notes.md`
  - 未更新 `AGENTS.md`

- module-notes.md 是否补充 .cpp 关键函数 / helper
  - 不需要。
  - 本任务未修改 `modules/store/*` 生产代码。

- 是否修改高频文档及原因
  - 修改了 `specs/007-storage-node-data-plane/tasks.md`
    - 原因：标记 T046 完成。
  - 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
    - 原因：保留 T045 后仍存在的 registry / corruption-healing 风险，同时把“后续由 T046 提供共享 helper”的表述更新成已完成状态。

- common-risk-notes.md 读取结果
  - 已读取并保留以下仍未关闭风险：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027、T045。
  - 重点确认：T024 corrupted 自动回写、T019/T020 timeout/cancellation、Windows 待验证、restart rebuild/staging cleanup、registry / repair / scrub / corruption healing 风险均未误删。

- common-risk-notes.md 新增/删除/保留情况
  - 新增：无
  - 删除：无
  - 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027、T045
  - 变更：更新 T045 条目描述，反映 T046 已完成共享测试 helper，但 registry facts / repair / scrub / corruption healing 仍待后续任务处理
