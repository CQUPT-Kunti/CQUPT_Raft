# Risk Register: Integrated Object Storage System

**Feature**: 008-integrated-object-storage-system  
**Date**: 2026-06-05

## R-001: ViewNode 被误实现为 Raft membership authority

**Risk**: 开发时为了方便把 ViewNode 注册结果直接用于 quorum 或 voter 列表。  
**Impact**: split-brain、非法 commit、membership 与 Raft log 不一致。  
**Mitigation**: 合同和任务明确 ViewNode 只观测；quorum 测试覆盖“注册不等于 voter”。  
**Owner Area**: `modules/view`, `modules/raft/node`, `tests/integrated_object_storage_quorum_test.cpp`

## R-002: 真实 payload 意外进入 Raft

**Risk**: CreateObject/CommitObject 或 task report 中携带 chunk bytes。  
**Impact**: Raft log 膨胀、snapshot 膨胀、恢复慢、违反阶段目标。  
**Mitigation**: metadata contract 禁止 payload；增加 payload boundary audit 测试。  
**Owner Area**: `proto/metadata.proto`, `modules/raft/metadata`, `modules/raft/common`, `tests/integrated_object_storage_e2e_test.cpp`

## R-003: 大文件路径仍整文件入内存

**Risk**: upload coordinator 或 client 为计算 etag 拼接完整文件。  
**Impact**: 大文件 OOM，性能目标失败。  
**Mitigation**: 基础任务先改为 streaming checksum 或调用方提供对象 checksum；测试覆盖内存边界。  
**Owner Area**: `modules/store/upload`, `apps/storage_client.cpp`

## R-004: node.identity durability 跨平台弱化

**Risk**: Linux 使用真实 fsync，而 Windows 分支 no-op success。  
**Impact**: 重启身份漂移或损坏后难以诊断。  
**Mitigation**: identity 写入 contract 明确平台语义；unsupported 返回明确错误；跨平台 durability 测试。  
**Owner Area**: `modules/store/common`, `modules/store/io`, planned identity helper

## R-005: StorageNode dynamic registration 与 placement facts 不一致

**Risk**: ViewNode 观测状态和 PlacementManager 使用的健康/容量快照不一致。  
**Impact**: 写计划选择 dead/full 节点，上传失败或 orphan chunk 增多。  
**Mitigation**: placement 只消费 live、fresh、capacity-valid snapshot；记录 decision_reasons。  
**Owner Area**: `modules/view`, `modules/store/placement`

## R-006: 协议变更范围扩大

**Risk**: 为了 ViewNode 或 streaming RPC 改动既有 metadata/storage RPC 语义。  
**Impact**: 破坏现有测试和调用方，扩大迁移范围。  
**Mitigation**: ViewNode 使用 additive contract；第一阶段使用 bounded chunk RPC；任何 proto 变更必须有 contract test。  
**Owner Area**: `proto/`, `modules/raft/service`, `modules/store/node`

## R-007: 测试启动真实多进程导致不稳定

**Risk**: E2E 测试依赖端口、时间、进程清理，容易 flake。  
**Impact**: CI 和本地验收不可靠。  
**Mitigation**: 测试 helper 管理端口和 data_dir；超时和日志路径可诊断；低并发运行完整组。  
**Owner Area**: `tests/support/integrated_cluster_test_utils.h`, `tests/CMakeLists.txt`
