#pragma once

// 008 阶段 ownership notes：
//
// 本文件只服务 integrated object storage 相关集成测试的共享 helper，
// 用于承载跨多个测试文件重复出现的集群级辅助能力，不承载生产逻辑。
//
// 后续允许放入的 helper 范围：
// - 临时集群目录、临时文件、测试进程的 RAII 清理辅助
// - 测试用 ClusterConfig / node.identity 输入生成辅助
// - ViewNode / MetadataNode / StorageNode 的测试启动与停止辅助
// - 测试端口分配、endpoint 组装与冲突规避辅助
// - 测试文件生成、SHA-256 计算与上传后校验辅助
// - leader election、node registration、heartbeat / liveness 的等待工具
// - StorageNode 故障、重启、checksum mismatch、quorum 不足等场景注入辅助
//
// 明确禁止：
// - 不替代真实业务逻辑，不把 helper 写成“测试专用控制面”
// - 不绕过 Raft quorum / commit 直接制造 COMMITTED 对象
// - 不伪造 StorageNode durable success、flush success 或 publish success
// - 不把真实 payload / chunk bytes 写入 Raft log、snapshot 或 metadata helper
// - 不把 ViewNode 注册结果解释为 Raft voter membership
// - 不依赖固定端口、固定 /tmp、固定 Linux-only 路径或 shell 语义
// - 不提前塞入 T026 / T050 / T080 等后续任务的具体测试流程
//
// 维护约束：
// - 优先保持 header-only，避免为 tests/support 新增链接目标
// - 新增 helper 时保持跨平台语义清晰，Linux / Windows 路径与清理行为可诊断
// - 这里只放共享测试辅助；单个测试文件私有逻辑应留在对应测试源码中
