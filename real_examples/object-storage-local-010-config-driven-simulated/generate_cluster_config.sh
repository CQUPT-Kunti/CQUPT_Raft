#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SOURCE_CONFIG_PATH="${SOURCE_CONFIG_PATH:-$REPO_ROOT/modules/config.json}"
OUTPUT_CONFIG_PATH="${OUTPUT_CONFIG_PATH:-$SCRIPT_DIR/cluster.json}"
JOIN_CONFIG_PATH="${JOIN_CONFIG_PATH:-$SCRIPT_DIR/storage-join-store-7.json}"
METADATA_LEARNER_CONFIG_PATH="${METADATA_LEARNER_CONFIG_PATH:-$SCRIPT_DIR/metadata-learner-4.json}"
METADATA_LEARNER_2_CONFIG_PATH="${METADATA_LEARNER_2_CONFIG_PATH:-$SCRIPT_DIR/metadata-learner-5.json}"
CLUSTER_ID="example-local-010-config-driven-simulated"

if [[ ! -f "$SOURCE_CONFIG_PATH" ]]; then
  echo "missing source config: $SOURCE_CONFIG_PATH" >&2
  exit 1
fi

if ! command -v jq >/dev/null 2>&1; then
  echo "jq is required to generate cluster configs" >&2
  exit 1
fi

tmp_main="$(mktemp)"
tmp_join="$(mktemp)"
tmp_meta4="$(mktemp)"
tmp_meta5="$(mktemp)"
trap 'rm -f "$tmp_main" "$tmp_join" "$tmp_meta4" "$tmp_meta5"' EXIT

generate_config() {
  local output_path="$1"
  local include_store7="$2"
  local include_meta4="$3"
  local include_meta5="$4"
  local tmp_output="$5"

  jq -n \
    --arg cluster_id "$CLUSTER_ID" \
    --argjson include_store7 "$include_store7" \
    --argjson include_meta4 "$include_meta4" \
    --argjson include_meta5 "$include_meta5" \
    --slurpfile source "$SOURCE_CONFIG_PATH" '
    ($source[0]) as $cfg |
    ($cfg.cluster) as $cluster |
    ($cfg.raft) as $raft |
    ($cfg.store) as $store |
    ($cfg.view) as $view |
    ([($store.storage_heartbeat_interval_ms // 2000),
      ($view.self_refresh_interval_ms // 2000),
      ($view.peer_sync_interval_ms // 2000),
      2000] | min) as $local_heartbeat_interval_ms |
    {
      cluster_id: $cluster_id,
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
      metadata_nodes: (
        [
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
        ] +
        (if $include_meta4 then
          [{
            node_id: "meta-4",
            raft_id: 4,
            endpoint: "127.0.0.1:9404",
            data_dir: "nodes/meta-4/data",
            snapshot_dir: "nodes/meta-4/snapshots",
            initial_role: "candidate"
          }]
        else [] end) +
        (if $include_meta5 then
          [{
            node_id: "meta-5",
            raft_id: 5,
            endpoint: "127.0.0.1:9405",
            data_dir: "nodes/meta-5/data",
            snapshot_dir: "nodes/meta-5/snapshots",
            initial_role: "candidate"
          }]
        else [] end)
      ),
      storage_nodes: (
        [
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
        ] +
        (if $include_store7 then
          [{
            node_id: "store-7",
            endpoint: "127.0.0.1:9507",
            data_dir: "nodes/store-7/data",
            capacity_bytes: 10737418240,
            failure_domain: { zone: "zone-d", rack: "rack-d1" }
          }]
        else [] end)
      ),
      initial_raft_membership: {
        membership_epoch: 1,
        voter_raft_ids: [1, 2, 3],
        learner_raft_ids: []
      },
      chunk_policy: {
        chunk_size_bytes: ($store.chunk_size_bytes // $cluster.chunk_size_bytes),
        replica_count: ($store.replica_count // $cluster.replica_count),
        minimum_successful_writes: ($store.minimum_successful_writes // $cluster.minimum_successful_writes),
        checksum_algorithm: "sha256"
      },
      timeouts: {
        discovery_rpc_timeout_ms: ($cluster.discovery_rpc_timeout_ms // 3000),
        metadata_rpc_timeout_ms: ($cluster.metadata_rpc_timeout_ms // 3000),
        storage_rpc_timeout_ms: ($cluster.storage_rpc_timeout_ms // 3000),
        heartbeat_interval_ms: $local_heartbeat_interval_ms,
        registration_timeout_ms: ($view.register_timeout_ms // 3000),
        commit_deadline_ms: ($store.commit_object_timeout_ms // 5000),
        liveness_stale_timeout_ms: ($store.stale_timeout_ms // 5000),
        liveness_dead_timeout_ms: ($store.dead_timeout_ms // 15000)
      },
      cluster: $cluster,
      raft: $raft,
      store: $store,
      view: $view
    }' > "$tmp_output"

  mv "$tmp_output" "$output_path"
}

generate_config "$OUTPUT_CONFIG_PATH" false false false "$tmp_main"
generate_config "$JOIN_CONFIG_PATH" true false false "$tmp_join"
generate_config "$METADATA_LEARNER_CONFIG_PATH" false true false "$tmp_meta4"
generate_config "$METADATA_LEARNER_2_CONFIG_PATH" false true true "$tmp_meta5"

printf '%s\n' "$OUTPUT_CONFIG_PATH"
