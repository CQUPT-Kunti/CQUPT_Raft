# Contract: Cluster Configuration And Generation

**Purpose**: 定义统一配置文件与配置生成流程，避免硬编码节点数量、端口、路径和身份。

## Config Inputs

- `cluster_id`
- `base_dir`
- `view_node_count`
- `metadata_node_count`
- `storage_node_count`
- endpoint bind host and port ranges
- per-role data_dir template
- storage capacity template or explicit capacities
- chunk size and replica policy
- heartbeat and timeout policy
- optional fixed node_id overrides for controlled recovery

## Generated Outputs

- full cluster config file
- per-node config files
- `node.identity` creation plan for MetadataNode and optional ViewNode
- initial Raft membership with voter/learner role and raft_id
- startup command hints for Linux and Windows

## Rules

- No source code change is required to move from 1 to 3 to 5 Raft MetadataNodes.
- Raft voter count must be exactly the configured initial membership.
- StorageNode additions can be represented by adding config and starting the node.
- Generated node_id values must be stable if the same config generation seed/state is reused.
- node.identity on disk wins over regenerated suggestion unless an explicit replacement workflow is used.
- Paths must use platform-appropriate separators and avoid hard-coded `/tmp` or drive-specific assumptions.

## Validation Errors

- duplicate endpoint
- duplicate node_id
- invalid Raft voter count
- capacity <= 0
- shared data_dir between different node identities
- identity/config mismatch
- unsupported durability mode on current platform
