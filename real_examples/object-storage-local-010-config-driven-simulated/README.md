# object-storage-local-010-config-driven-simulated

这个独立本地模拟目录用于验证：运行时直接使用 `modules/config.json` 中的 `cluster`、`raft`、`store`、`view` 四段参数生成集群配置，并在真实本地多进程环境下完成大文件上传和下载。

目录结构与 `real_examples/object-storage-local-009-simulated` 保持一致，但配置生成不再保留旧示例里的静态 1 MiB / 3s 参数，而是统一从仓库当前的 `modules/config.json` 生成：

- `cluster.json`
- `storage-join-store-7.json`
- `metadata-learner-4.json`
- `metadata-learner-5.json`

常用命令：

```bash
./generate_cluster_config.sh
./qidong.sh
./rpc_demo.sh parallel-roundtrip
./tingzhi.sh
```

如果需要完整重建运行数据，可删除本目录下的 `logs/`、`pids/`、`downloads/`、`nodes/` 后重新启动。快照/身份目录也都只落在本目录内，不影响其他示例。 
