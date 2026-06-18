# T115 Run Final Targeted Linux Validation Set From Quickstart

## 1. 使用的 quickstart 验证范围

- targeted app build
- targeted integration / unit test build
- targeted CTest subset
- baseline local RPC startup / status / roundtrip / shutdown
- sibling `examples/object-storage-local-009-dynamic` startup / status / roundtrip / shutdown
- Windows/macOS 未执行，记录 pending

说明：

- `quickstart.md` 第二组 build 示例中的 `storage_heartbeat_registry` 在当前工程真实 target 为 `test_storage_heartbeat_registry`，因此按 `tests/CMakeLists.txt` 的实际 target 执行。
- targeted CTest 实际使用 test name regex，而不是 quickstart 示例 label 组合，以便精确覆盖当前 009 相关测试集。

## 2. targeted build 命令和结果

- app build：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target \
    view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client
) 9>/tmp/cqupt_raft_build.lock
```

- 结果：PASS
- 本轮复跑结果：`ninja: no work to do.`
- 日志：`tmp/test-logs/t115-build-apps.log`

- integration / unit test build：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target \
    integrated_object_storage_e2e integrated_object_storage_quorum \
    test_view_node_discovery test_node_identity test_storage_heartbeat_registry
) 9>/tmp/cqupt_raft_build.lock
```

- 结果：PASS
- 日志：`tmp/test-logs/t115-build-tests.log`

## 3. targeted CTest 命令和结果

- 实际执行命令：

```bash
ctest --preset debug-tests -R "IntegratedObjectStorage(E2ETest|QuorumTest)|ViewNodeDiscoveryTest|ViewFailoverTest|NodeIdentityTest|storage_heartbeat_registry" --output-on-failure
```

- 结果：PASS
- `104/104` tests passed
- disabled tests 未运行：
  - `IntegratedObjectStorageE2ETest.AppConfigParsingSmokeCliOverridesMustRespectDurableIdentityAndStartupContracts`
  - `IntegratedObjectStorageE2ETest.HappyPathUploadDownloadRoundTripViaIntegratedObjectStorage`
  - `IntegratedObjectStorageE2ETest.ChecksumMismatchDownloadFailsWithoutPublishingCorruptedFile`
- 日志：`tmp/test-logs/t115-ctest.log`

## 4. local RPC startup / status / roundtrip / shutdown 命令和结果

### baseline example

- 实际执行链路：

```bash
examples/object-storage-local-3meta-6store/qidong.sh
examples/object-storage-local-3meta-6store/rpc_demo.sh status
examples/object-storage-local-3meta-6store/rpc_demo.sh roundtrip
examples/object-storage-local-3meta-6store/tingzhi.sh
```

- 结果：PASS
- 关键验证点：
  - `status` 成功，ViewNode/MetadataNode/StorageNode 发现信息完整。
  - `roundtrip` 成功完成 bucket ensure、upload、download、checksum/integrity 校验。
  - 4 个测试文件全部 `verify OK`。
- 日志：
  - `tmp/test-logs/t115-fix-baseline-start.log`
  - `tmp/test-logs/t115-fix-baseline-status.log`
  - `tmp/test-logs/t115-fix-baseline-roundtrip.log`
  - `tmp/test-logs/t115-fix-baseline-stop.log`
  - `tmp/test-logs/t115-fix-residual-check.log`

### sibling 009 dynamic example

- 实际执行链路：

```bash
examples/object-storage-local-009-dynamic/qidong.sh
examples/object-storage-local-009-dynamic/rpc_demo.sh status
examples/object-storage-local-009-dynamic/rpc_demo.sh roundtrip
examples/object-storage-local-009-dynamic/tingzhi.sh
```

- 结果：PASS
- 关键验证点：
  - `status` 成功，2 个 ViewNode、3 个 MetadataNode、6 个 StorageNode 可见。
  - `roundtrip` 成功完成 bucket ensure、upload、download、checksum/integrity 校验。
  - 4 个测试文件全部 `verify OK`。
- 日志：
  - `tmp/test-logs/t115-dynamic-start.log`
  - `tmp/test-logs/t115-dynamic-status.log`
  - `tmp/test-logs/t115-dynamic-roundtrip.log`
  - `tmp/test-logs/t115-dynamic-stop.log`
  - `tmp/test-logs/t115-dynamic-residual-check.log`

## 5. 是否执行 sibling 009 dynamic example

- 是。
- 结果：PASS。

## 6. 失败项和失败分类

- 本轮最终结果中无失败项。
- 补充说明：
  - baseline 初次失败并不是 build/CTest 失败。
  - 清理 ignored runtime state 后问题仍可复现，因此根因不是旧 runtime data 污染。
  - 实际根因是 baseline `rpc_demo.sh roundtrip` 在 upload 前没有显式 ensure bucket，导致 `CreateWritePlan` 返回 `bucket does not exist`。
  - 已通过 example 脚本级修复补齐 bucket ensure 幂等路径，未修改生产代码、协议或测试断言。

## 7. snapshot cleanup 和 rerun-failed 结果

- 未执行。
- 原因：
  - targeted CTest 无失败项。
  - 本任务失败点在 example 脚本链路，不属于 snapshot/runtime snapshot 干扰。

## 8. log 路径

- `tmp/test-logs/t115-build-apps.log`
- `tmp/test-logs/t115-build-tests.log`
- `tmp/test-logs/t115-ctest.log`
- `tmp/test-logs/t115-fix-clean-dry-run.log`
- `tmp/test-logs/t115-fix-clean.log`
- `tmp/test-logs/t115-fix-baseline-start.log`
- `tmp/test-logs/t115-fix-baseline-status.log`
- `tmp/test-logs/t115-fix-baseline-roundtrip.log`
- `tmp/test-logs/t115-fix-baseline-stop.log`
- `tmp/test-logs/t115-fix-residual-check.log`
- `tmp/test-logs/t115-dynamic-start.log`
- `tmp/test-logs/t115-dynamic-status.log`
- `tmp/test-logs/t115-dynamic-roundtrip.log`
- `tmp/test-logs/t115-dynamic-stop.log`
- `tmp/test-logs/t115-dynamic-residual-check.log`

## 9. cleanup 结果

- baseline example 已调用 `tingzhi.sh` 完成清理。
- sibling dynamic example 已调用 `tingzhi.sh` 完成清理。
- 两轮结束后 residual check 均未发现残留 example 进程。
- 结果：`no_residual_example_processes`

## 10. Windows/macOS pending

- pending

## 11. 最终状态

- PASS

## 12. 是否已勾选 T115

- 是

## 13. 是否可以进入 T116

- 可以
