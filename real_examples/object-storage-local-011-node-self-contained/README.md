# object-storage-local-011-node-self-contained

这个目录是 `task11` 风格的独立示例，目标不是替换 `010`，而是补充一种新的节点组织方式：

- 每个 `node` 目录都自带自己的 `config.json`
- 每个 `node` 目录都自带自己的 `node.conf`
- 每个 `node` 目录都自带拆开的参数文件：
  - `cluster.json`
  - `raft.json`
  - `store.json`
  - `view.json`
- 每个 `node` 目录都自带：
  - `node.sh`
  - `start.sh`
  - `stop.sh`
  - `status.sh`
  - `restart.sh`

也就是说，单独进入某个节点目录后，这个节点只需要管理自己的配置和自己的启动脚本，不必再回到别的目录读共享配置。

配置源仍然来自：

```text
modules/config.json
```

生成方式：

```bash
cd real_examples/object-storage-local-011-node-self-contained
./generate_cluster_config.sh
```

批量启动：

```bash
./qidong.sh
```

批量停止：

```bash
./tingzhi.sh
```

单节点操作示例：

```bash
cd nodes/store-3
./node.sh
./start.sh
./status.sh
./restart.sh
./stop.sh
```

单独查看该节点自己的参数：

```bash
cd nodes/store-3
cat config.json
cat cluster.json
cat raft.json
cat store.json
cat view.json
```

当前目录内保留了根级别兼容入口：

- `cluster.json`
- `storage-join-store-7.json`
- `metadata-learner-4.json`
- `metadata-learner-5.json`

它们只是指向节点自有 `config.json` 的兼容入口别名，真正的配置权威仍在各自节点目录下。 
