#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SOURCE_CONFIG_PATH="${SOURCE_CONFIG_PATH:-$REPO_ROOT/modules/config.json}"
OUTPUT_CONFIG_PATH="${OUTPUT_CONFIG_PATH:-$SCRIPT_DIR/cluster.json}"

if [[ ! -f "$SOURCE_CONFIG_PATH" ]]; then
  echo "missing source config: $SOURCE_CONFIG_PATH" >&2
  exit 1
fi

if ! command -v jq >/dev/null 2>&1; then
  echo "jq is required to generate $OUTPUT_CONFIG_PATH" >&2
  exit 1
fi

tmp_config="$(mktemp)"
trap 'rm -f "$tmp_config"' EXIT

jq -n \
  --slurpfile source "$SOURCE_CONFIG_PATH" '
  ($source[0]) as $cfg |
  ([($cfg.store.storage_heartbeat_interval_ms // 2000),
    ($cfg.view.self_refresh_interval_ms // 2000),
    ($cfg.view.peer_sync_interval_ms // 2000),
    2000] | min) as $local_heartbeat_interval_ms |
  {
    cluster_id: "example-local-009-simulated",
    base_dir: ".",
    view_nodes: [
      {
        node_id: "view-1",
        endpoint: "127.0.0.1:9301",
        peer_seeds: ["127.0.0.1:9302"],
        data_dir: "nodes/view-1/data"
      },
      {
        node_id: "view-2",
        endpoint: "127.0.0.1:9302",
        peer_seeds: ["127.0.0.1:9301"],
        data_dir: "nodes/view-2/data"
      }
    ],
    metadata_nodes: [
      {
        node_id: "meta-1",
        raft_id: 1,
        endpoint: "127.0.0.1:9401",
        data_dir: "nodes/meta-1/data",
        snapshot_dir: "nodes/meta-1/snapshots",
        initial_role: "voter"
      },
      {
        node_id: "meta-2",
        raft_id: 2,
        endpoint: "127.0.0.1:9402",
        data_dir: "nodes/meta-2/data",
        snapshot_dir: "nodes/meta-2/snapshots",
        initial_role: "voter"
      },
      {
        node_id: "meta-3",
        raft_id: 3,
        endpoint: "127.0.0.1:9403",
        data_dir: "nodes/meta-3/data",
        snapshot_dir: "nodes/meta-3/snapshots",
        initial_role: "voter"
      }
    ],
    storage_nodes: [
      {
        node_id: "store-1",
        endpoint: "127.0.0.1:9501",
        data_dir: "nodes/store-1/data",
        capacity_bytes: 10737418240,
        failure_domain: { zone: "zone-a", rack: "rack-a1" }
      },
      {
        node_id: "store-2",
        endpoint: "127.0.0.1:9502",
        data_dir: "nodes/store-2/data",
        capacity_bytes: 10737418240,
        failure_domain: { zone: "zone-a", rack: "rack-a2" }
      },
      {
        node_id: "store-3",
        endpoint: "127.0.0.1:9503",
        data_dir: "nodes/store-3/data",
        capacity_bytes: 10737418240,
        failure_domain: { zone: "zone-b", rack: "rack-b1" }
      },
      {
        node_id: "store-4",
        endpoint: "127.0.0.1:9504",
        data_dir: "nodes/store-4/data",
        capacity_bytes: 10737418240,
        failure_domain: { zone: "zone-b", rack: "rack-b2" }
      },
      {
        node_id: "store-5",
        endpoint: "127.0.0.1:9505",
        data_dir: "nodes/store-5/data",
        capacity_bytes: 10737418240,
        failure_domain: { zone: "zone-c", rack: "rack-c1" }
      },
      {
        node_id: "store-6",
        endpoint: "127.0.0.1:9506",
        data_dir: "nodes/store-6/data",
        capacity_bytes: 10737418240,
        failure_domain: { zone: "zone-c", rack: "rack-c2" }
      }
    ],
    initial_raft_membership: {
      membership_epoch: 1,
      voter_raft_ids: [1, 2, 3],
      learner_raft_ids: []
    },
    chunk_policy: {
      chunk_size_bytes: ($cfg.store.chunk_size_bytes // $cfg.cluster.chunk_size_bytes),
      replica_count: ($cfg.store.replica_count // $cfg.cluster.replica_count),
      minimum_successful_writes: ($cfg.store.minimum_successful_writes // $cfg.cluster.minimum_successful_writes),
      checksum_algorithm: "sha256"
    },
    timeouts: {
      discovery_rpc_timeout_ms: $cfg.cluster.discovery_rpc_timeout_ms,
      metadata_rpc_timeout_ms: $cfg.cluster.metadata_rpc_timeout_ms,
      storage_rpc_timeout_ms: $cfg.cluster.storage_rpc_timeout_ms,
      heartbeat_interval_ms: $local_heartbeat_interval_ms,
      registration_timeout_ms: $cfg.view.register_timeout_ms,
      commit_deadline_ms: $cfg.store.commit_object_timeout_ms,
      liveness_stale_timeout_ms: $cfg.store.stale_timeout_ms,
      liveness_dead_timeout_ms: $cfg.store.dead_timeout_ms
    },
    cluster: $cfg.cluster,
    raft: $cfg.raft,
    store: $cfg.store,
    view: $cfg.view
  }' > "$tmp_config"

mv "$tmp_config" "$OUTPUT_CONFIG_PATH"
printf '%s\n' "$OUTPUT_CONFIG_PATH"
