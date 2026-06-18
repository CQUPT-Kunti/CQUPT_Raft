# T112 Add Dynamic Metadata Join Safety Constraints And Consistency Validation

## 1. 做了什么

- 为 dynamic metadata join 补充了两条安全与一致性验证场景，覆盖并发 join 与 join 失败隔离。
- 将 dynamic join 配置 helper 泛化为可为不同 candidate 生成独立配置，保持既有 T111 场景不变。
- 为 fake metadata leader 增加 join request 历史记录，便于验证多个 candidate 的 join 请求不会互相覆盖。
- 同步修正 `tasks.md` 中落后的 T112 描述，并在验证通过后勾选 T112。

## 2. 修改了哪些文件

- `tests/metadata_client_scenario_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t112-add-dynamic-metadata-join-safety-constraints-and-consistency-validation.md`

## 3. 新增或更新了哪些测试

- 新增 `MetadataClientScenarioTest.MetadataNodeConcurrentDynamicJoinPreservesStableRegistryAndIdentityUniqueness`
- 新增 `MetadataClientScenarioTest.MetadataNodeDynamicJoinFailureDoesNotPolluteRegistryOrCandidateIdentity`
- 更新 test helper：
  - `WriteDynamicJoinClusterConfigForCandidate(...)`
  - `FakeJoinMetadataService::join_requests()`

## 4. 如何验证 join safety

- 并发启动两个不同 `MetadataNode` candidate，分别使用独立 `node_id`、`raft_id`、`data_dir` 与 endpoint。
- 验证 fake leader 收到两个 join 请求，且每个请求都保持各自稳定的：
  - `node_id`
  - `candidate_raft_id`
  - `persistent_generation=1`
  - `local_state_hint=candidate`
- 验证两个 candidate 的本地 `node.identity` 都保持 `membership_state=candidate`，没有互相覆盖，也没有漂移为 voter。
- 验证稳定 leader 在 ViewNode registry 中仍保持 `VOTER/LEADER`，没有因为并发 join 被回退、覆盖或替换。

## 5. 如何验证 consistency 保持不变

- 在并发 join 成功后读取 ViewNode registry，确认最终只有：
  - 1 个稳定 leader 观测项
  - 2 个 candidate 对应的 learner 观测项
- 验证两个 candidate 在 registry 中均以 `LEARNER` 被观测，不会直接成为 `VOTER`。
- 验证稳定节点 endpoint、raft role、membership state 保持不变，说明 join 不会破坏已有 stable cluster 的观测一致性。
- 通过 broad `Metadata|Join|NodeIdentity` 回归确认 T110-T111 既有 identity / restart / dynamic join 语义未退化。

## 6. 如何验证 failure 不污染状态

- 预先向 ViewNode registry seed 一个稳定 metadata leader。
- 让 candidate 指向一个固定返回 `NOT_LEADER` 的 fake join leader，验证 join 失败退出。
- 失败后检查：
  - ViewNode registry 仍只有原有稳定 leader
  - candidate 未被错误注册为 learner 或 voter
  - 稳定 leader 的 `VOTER/LEADER` 状态不变
  - candidate 的 `node.identity` 仍是 `membership_state=candidate`
  - `persistent_generation` 保持 `1`
- 对同一 candidate 再次失败重试，验证 identity 文件内容保持不变，说明失败不会污染持久化状态。

## 7. 验证结果

- 定向构建：
  - `(
       flock -n 9 || exit 99
       cmake --build --preset debug-ninja-low-parallel --target test_metadata_client_scenario
     ) 9>/tmp/cqupt_raft_build.lock`
  - PASS
- 聚焦回归：
  - `ctest --preset debug-tests -R "MetadataNodeConcurrentDynamicJoinPreservesStableRegistryAndIdentityUniqueness|MetadataNodeDynamicJoinFailureDoesNotPolluteRegistryOrCandidateIdentity" --output-on-failure`
  - PASS
  - `2/2` tests passed
  - 日志：`tmp/test-logs/t112-focused.log`
- broad 回归：
  - `ctest --preset debug-tests -R "Metadata|Join|NodeIdentity" --output-on-failure`
  - PASS
  - `180/180` tests passed
  - 日志：`tmp/test-logs/t112-broad.log`

## 8. 是否 PASS / FAIL / SKIPPED

- PASS

## 9. 是否已勾选 T112

- 已勾选

## 10. 是否可以进入下一任务

- 可以。
- 当前 T112 关注的 join safety、consistency、failure isolation 都已有直接覆盖，并且 broad 回归通过。

## 风险和后续注意事项

- 本轮没有修改 metadata membership authority、learner/voter 规则、quorum 逻辑或 ViewNode 业务逻辑。
- 当前并发 join 验证覆盖的是 discovery + join admission + registry observation 的 safety boundary，不代表后续 promote 或 membership commit 已完成同等并发验证。
- 本轮未发现需要新增到 `cross-task-risk-notes.md` 的跨任务风险。

## 建议下一步任务

- 建议执行 `T113`。
- 原因：
  - T112 已经暴露并修正了 `tasks.md` 中的任务语义漂移。
  - 当前最合理的下一步是做文档收口，确认 `spec.md`、`plan.md`、`tasks.md` 没有执行日志和任务描述漂移，再继续最终阶段验收。
