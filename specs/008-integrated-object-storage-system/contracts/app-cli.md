# Contract: App And CLI Entrypoints

**Purpose**: 定义独立 app 的启动参数与用户可执行流程。

## Required Apps

- `view_node_app`
- `metadata_node_app` or `raft_metadata_node_app`
- `storage_node_app`
- `storage_client`

## Optional App

- `storage_bench`

## Common Startup Parameters

- `--config <path>`: required cluster or per-node config path.
- `--node_id <id>`: optional controlled override; must be rejected if it conflicts with durable identity.
- `--data_dir <path>`: optional safe override for local testing; must not silently reuse another node identity.
- `--listen <host:port>`: optional endpoint override for local testing.
- `--role <view|meta|store>`: optional only for multi-role config tools, not required for role-specific apps.

## Client Commands

### generate-config

```text
storage_client generate-config --out <path> --base-dir <dir>
```

**Expected behavior**:

- Parse local cluster topology parameters such as cluster_id, node counts, port bases, chunk policy, replica policy, storage capacity, and timeout policy.
- Delegate cluster topology generation and validation to the shared cluster config module.
- Write a reusable local cluster config file for later ViewNode / MetadataNode / StorageNode / storage_client startup.
- Print clear success summary including output path, node counts, and initial metadata quorum.
- Return non-zero on invalid arguments, validation failure, or output path write failure.

### upload

```text
storage_client upload --config <path> --bucket <bucket> --object <key> --file <source>
```

**Expected behavior**:

- Discover MetadataNode through ViewNode.
- Create write plan.
- Write chunks to StorageNode.
- Commit object.
- Print object_id, version, size, checksum, chunk count, and request_id.
- Optional overrides may include `--object-id`, `--request-id`, `--chunk-size`, `--replicas`, `--min-writes`, and `--concurrency`.
- If `--object-id` is omitted, the CLI may derive a stable safe object_id from bucket/object key instead of requiring users to invent one manually.

### download

```text
storage_client download --config <path> --bucket <bucket> --object <key> --out <destination>
```

**Expected behavior**:

- Discover MetadataNode through ViewNode.
- Get COMMITTED manifest.
- Read chunks from StorageNode.
- Verify chunk checksums and final object checksum.
- Print PASS-style integrity result or clear failure reason.
- Optional overrides may include `--object-id`, `--version`, `--request-id`, and `--concurrency`.

### status

```text
storage_client status --config <path>
```

**Expected behavior**:

- Query ViewNode cluster view.
- Print ViewNode/MetadataNode/StorageNode liveness, capacity, leader hint, and membership observation.

## Exit Semantics

- `0`: command completed and integrity checks passed where applicable.
- non-zero: invalid arguments, discovery failure, quorum failure, checksum mismatch, capacity failure, IO failure, or durability unsupported.

## Output Rules

- Include request_id, node_id, leader hint, status code, and diagnostic message.
- Do not print raw file payload.
- Do not print full Raft logs or node logs.
