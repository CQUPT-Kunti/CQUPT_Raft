#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SOURCE_CONFIG_PATH="${SOURCE_CONFIG_PATH:-$REPO_ROOT/modules/config.json}"
NODES_DIR="$SCRIPT_DIR/nodes"
CLUSTER_ID="example-local-011-node-self-contained"

if [[ ! -f "$SOURCE_CONFIG_PATH" ]]; then
  echo "missing source config: $SOURCE_CONFIG_PATH" >&2
  exit 1
fi

if ! command -v jq >/dev/null 2>&1; then
  echo "jq is required to generate node configs" >&2
  exit 1
fi

mkdir -p "$NODES_DIR"

write_section_json() {
  local output_path="$1"
  local query="$2"
  local tmp_output
  tmp_output="$(mktemp)"
  jq "$query" "$SOURCE_CONFIG_PATH" >"$tmp_output"
  mv "$tmp_output" "$output_path"
}

generate_config_json() {
  local output_path="$1"
  local self_node_id="$2"
  local self_node_dir="$3"
  local include_store7="$4"
  local include_meta4="$5"
  local include_meta5="$6"
  local tmp_output
  tmp_output="$(mktemp)"

  jq -n \
    --arg cluster_id "$CLUSTER_ID" \
    --arg self_node_id "$self_node_id" \
    --arg self_node_dir "$self_node_dir" \
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
    def node_dir($id):
      if $id == $self_node_id then
        $self_node_dir
      else
        "../" + $id
      end;
    def node_data_dir($id): node_dir($id) + "/data";
    def node_snapshot_dir($id): node_dir($id) + "/snapshots";
    {
      cluster_id: $cluster_id,
      base_dir: ".",
      view_nodes: [
        {
          node_id: "view-1",
          endpoint: "127.0.0.1:9301",
          peer_seeds: ["127.0.0.1:9302"],
          data_dir: node_data_dir("view-1")
        },
        {
          node_id: "view-2",
          endpoint: "127.0.0.1:9302",
          peer_seeds: ["127.0.0.1:9301"],
          data_dir: node_data_dir("view-2")
        }
      ],
      metadata_nodes: (
        [
          {
            node_id: "meta-1",
            raft_id: 1,
            endpoint: "127.0.0.1:9401",
            data_dir: node_data_dir("meta-1"),
            snapshot_dir: node_snapshot_dir("meta-1"),
            initial_role: "voter"
          },
          {
            node_id: "meta-2",
            raft_id: 2,
            endpoint: "127.0.0.1:9402",
            data_dir: node_data_dir("meta-2"),
            snapshot_dir: node_snapshot_dir("meta-2"),
            initial_role: "voter"
          },
          {
            node_id: "meta-3",
            raft_id: 3,
            endpoint: "127.0.0.1:9403",
            data_dir: node_data_dir("meta-3"),
            snapshot_dir: node_snapshot_dir("meta-3"),
            initial_role: "voter"
          }
        ] +
        (if $include_meta4 then
          [{
            node_id: "meta-4",
            raft_id: 4,
            endpoint: "127.0.0.1:9404",
            data_dir: node_data_dir("meta-4"),
            snapshot_dir: node_snapshot_dir("meta-4"),
            initial_role: "candidate"
          }]
        else [] end) +
        (if $include_meta5 then
          [{
            node_id: "meta-5",
            raft_id: 5,
            endpoint: "127.0.0.1:9405",
            data_dir: node_data_dir("meta-5"),
            snapshot_dir: node_snapshot_dir("meta-5"),
            initial_role: "candidate"
          }]
        else [] end)
      ),
      storage_nodes: (
        [
          {
            node_id: "store-1",
            endpoint: "127.0.0.1:9501",
            data_dir: node_data_dir("store-1"),
            capacity_bytes: 10737418240,
            failure_domain: { zone: "zone-a", rack: "rack-a1" }
          },
          {
            node_id: "store-2",
            endpoint: "127.0.0.1:9502",
            data_dir: node_data_dir("store-2"),
            capacity_bytes: 10737418240,
            failure_domain: { zone: "zone-a", rack: "rack-a2" }
          },
          {
            node_id: "store-3",
            endpoint: "127.0.0.1:9503",
            data_dir: node_data_dir("store-3"),
            capacity_bytes: 10737418240,
            failure_domain: { zone: "zone-b", rack: "rack-b1" }
          },
          {
            node_id: "store-4",
            endpoint: "127.0.0.1:9504",
            data_dir: node_data_dir("store-4"),
            capacity_bytes: 10737418240,
            failure_domain: { zone: "zone-b", rack: "rack-b2" }
          },
          {
            node_id: "store-5",
            endpoint: "127.0.0.1:9505",
            data_dir: node_data_dir("store-5"),
            capacity_bytes: 10737418240,
            failure_domain: { zone: "zone-c", rack: "rack-c1" }
          },
          {
            node_id: "store-6",
            endpoint: "127.0.0.1:9506",
            data_dir: node_data_dir("store-6"),
            capacity_bytes: 10737418240,
            failure_domain: { zone: "zone-c", rack: "rack-c2" }
          }
        ] +
        (if $include_store7 then
          [{
            node_id: "store-7",
            endpoint: "127.0.0.1:9507",
            data_dir: node_data_dir("store-7"),
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

write_node_assets() {
  local node_id="$1"
  local app_target="$2"
  local listen="$3"
  local has_snapshot_dir="$4"
  local include_store7="$5"
  local include_meta4="$6"
  local include_meta5="$7"
  local node_dir="$NODES_DIR/$node_id"
  local node_config_path="$node_dir/config.json"
  local node_conf_path="$node_dir/node.conf"

  mkdir -p "$node_dir" "$node_dir/data" "$node_dir/logs" "$node_dir/pids"
  if [[ "$has_snapshot_dir" == "yes" ]]; then
    mkdir -p "$node_dir/snapshots"
  fi

  generate_config_json "$node_config_path" "$node_id" "$node_dir" "$include_store7" "$include_meta4" "$include_meta5"
  write_section_json "$node_dir/cluster.json" '.cluster'
  write_section_json "$node_dir/raft.json" '.raft'
  write_section_json "$node_dir/store.json" '.store'
  write_section_json "$node_dir/view.json" '.view'

  cat >"$node_conf_path" <<EOF
APP_TARGET="$app_target"
NODE_ID="$node_id"
LISTEN="$listen"
DATA_DIR="data"
LOG_FILE="logs/$node_id.log"
PID_FILE="pids/$node_id.pid"
CONFIG_PATH="$node_config_path"
CLUSTER_SECTION_PATH="$node_dir/cluster.json"
RAFT_SECTION_PATH="$node_dir/raft.json"
STORE_SECTION_PATH="$node_dir/store.json"
VIEW_SECTION_PATH="$node_dir/view.json"
EOF

  if [[ "$has_snapshot_dir" == "yes" ]]; then
    cat >>"$node_conf_path" <<'EOF'
SNAPSHOT_DIR="snapshots"
EOF
  fi

  cat >"$node_dir/node.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXAMPLE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
exec "$EXAMPLE_DIR/node_runner.sh" "$SCRIPT_DIR/node.conf" start
EOF

  cat >"$node_dir/start.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/node.sh"
EOF

  cat >"$node_dir/stop.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXAMPLE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
exec "$EXAMPLE_DIR/node_runner.sh" "$SCRIPT_DIR/node.conf" stop
EOF

  cat >"$node_dir/status.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXAMPLE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
exec "$EXAMPLE_DIR/node_runner.sh" "$SCRIPT_DIR/node.conf" status
EOF

  cat >"$node_dir/restart.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXAMPLE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
exec "$EXAMPLE_DIR/node_runner.sh" "$SCRIPT_DIR/node.conf" restart
EOF

  chmod +x "$node_dir/node.sh" "$node_dir/start.sh" "$node_dir/stop.sh" "$node_dir/status.sh" "$node_dir/restart.sh"
}

write_node_assets "view-1"  "view_node_app"     "127.0.0.1:9301" "no"  false false false
write_node_assets "view-2"  "view_node_app"     "127.0.0.1:9302" "no"  false false false
write_node_assets "meta-1"  "metadata_node_app" "127.0.0.1:9401" "yes" false false false
write_node_assets "meta-2"  "metadata_node_app" "127.0.0.1:9402" "yes" false false false
write_node_assets "meta-3"  "metadata_node_app" "127.0.0.1:9403" "yes" false false false
write_node_assets "meta-4"  "metadata_node_app" "127.0.0.1:9404" "yes" false true  false
write_node_assets "meta-5"  "metadata_node_app" "127.0.0.1:9405" "yes" false true  true
write_node_assets "store-1" "storage_node_app"  "127.0.0.1:9501" "no"  false false false
write_node_assets "store-2" "storage_node_app"  "127.0.0.1:9502" "no"  false false false
write_node_assets "store-3" "storage_node_app"  "127.0.0.1:9503" "no"  false false false
write_node_assets "store-4" "storage_node_app"  "127.0.0.1:9504" "no"  false false false
write_node_assets "store-5" "storage_node_app"  "127.0.0.1:9505" "no"  false false false
write_node_assets "store-6" "storage_node_app"  "127.0.0.1:9506" "no"  false false false
write_node_assets "store-7" "storage_node_app"  "127.0.0.1:9507" "no"  true  false false

ln -sfn "nodes/view-1/config.json" "$SCRIPT_DIR/cluster.json"
ln -sfn "nodes/store-7/config.json" "$SCRIPT_DIR/storage-join-store-7.json"
ln -sfn "nodes/meta-4/config.json" "$SCRIPT_DIR/metadata-learner-4.json"
ln -sfn "nodes/meta-5/config.json" "$SCRIPT_DIR/metadata-learner-5.json"

printf '%s\n' "$SCRIPT_DIR/cluster.json"
