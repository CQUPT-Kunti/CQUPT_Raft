#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BIN_DIR="${BIN_DIR:-$REPO_ROOT/build/linux}"
CONFIG_PATH="$SCRIPT_DIR/cluster.json"
CONFIG_GENERATOR="$SCRIPT_DIR/generate_cluster_config.sh"
LOG_DIR="$SCRIPT_DIR/logs"
PID_DIR="$SCRIPT_DIR/pids"

declare -a STARTED_NODE_IDS=()

require_binary() {
  local target="$1"
  if [[ ! -x "$BIN_DIR/$target" ]]; then
    echo "missing binary: $BIN_DIR/$target" >&2
    exit 1
  fi
}

stop_pid_if_running() {
  local pid="$1"
  if kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null || true
    for _ in $(seq 1 20); do
      if ! kill -0 "$pid" 2>/dev/null; then
        break
      fi
      sleep 0.2
    done
    if kill -0 "$pid" 2>/dev/null; then
      kill -9 "$pid" 2>/dev/null || true
    fi
  fi
}

cleanup_started_nodes() {
  local node_id
  for ((idx=${#STARTED_NODE_IDS[@]}-1; idx>=0; --idx)); do
    node_id="${STARTED_NODE_IDS[$idx]}"
    local pid_file="$PID_DIR/$node_id.pid"
    if [[ ! -f "$pid_file" ]]; then
      continue
    fi
    local pid
    pid="$(cat "$pid_file")"
    stop_pid_if_running "$pid"
    rm -f "$pid_file"
    echo "rolled back node_id=$node_id pid=$pid" >&2
  done
}

launch_node() {
  local app_target="$1"
  local node_id="$2"
  local listen="$3"
  local data_dir="$4"
  local snapshot_dir="${5:-}"

  require_binary "$app_target"
  mkdir -p "$LOG_DIR" "$PID_DIR" "$SCRIPT_DIR/$data_dir"
  if [[ -n "$snapshot_dir" ]]; then
    mkdir -p "$SCRIPT_DIR/$snapshot_dir"
  fi

  local pid_file="$PID_DIR/$node_id.pid"
  local log_file="$LOG_DIR/$node_id.log"
  if [[ -f "$pid_file" ]]; then
    local old_pid
    old_pid="$(cat "$pid_file")"
    if kill -0 "$old_pid" 2>/dev/null; then
      echo "$node_id already running pid=$old_pid"
      return 0
    fi
    rm -f "$pid_file"
  fi

  (
    cd "$SCRIPT_DIR"
    setsid "$BIN_DIR/$app_target" \
      --config "$CONFIG_PATH" \
      --node_id "$node_id" \
      --data_dir "$data_dir" \
      --listen "$listen" \
      >"$log_file" 2>&1 < /dev/null &
    echo "$!" > "$pid_file"
  )

  local pid
  pid="$(cat "$pid_file")"
  sleep 0.3
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "failed to start $node_id, see $log_file" >&2
    return 1
  fi

  STARTED_NODE_IDS+=("$node_id")
  echo "started node_id=$node_id target=$app_target pid=$pid listen=$listen log=$log_file"
}

main() {
  trap 'cleanup_started_nodes' ERR

  "$CONFIG_GENERATOR" >/dev/null

  local -a view_nodes=(
    "view_node_app|view-1|127.0.0.1:9301|nodes/view-1/data|"
    "view_node_app|view-2|127.0.0.1:9302|nodes/view-2/data|"
  )
  local -a metadata_nodes=(
    "metadata_node_app|meta-1|127.0.0.1:9401|nodes/meta-1/data|nodes/meta-1/snapshots"
    "metadata_node_app|meta-2|127.0.0.1:9402|nodes/meta-2/data|nodes/meta-2/snapshots"
    "metadata_node_app|meta-3|127.0.0.1:9403|nodes/meta-3/data|nodes/meta-3/snapshots"
  )
  local -a storage_nodes=(
    "storage_node_app|store-1|127.0.0.1:9501|nodes/store-1/data|"
    "storage_node_app|store-2|127.0.0.1:9502|nodes/store-2/data|"
    "storage_node_app|store-3|127.0.0.1:9503|nodes/store-3/data|"
    "storage_node_app|store-4|127.0.0.1:9504|nodes/store-4/data|"
    "storage_node_app|store-5|127.0.0.1:9505|nodes/store-5/data|"
    "storage_node_app|store-6|127.0.0.1:9506|nodes/store-6/data|"
  )

  local entry
  local app_target
  local node_id
  local listen
  local data_dir
  local snapshot_dir

  for entry in "${view_nodes[@]}"; do
    IFS='|' read -r app_target node_id listen data_dir snapshot_dir <<<"$entry"
    launch_node "$app_target" "$node_id" "$listen" "$data_dir" "$snapshot_dir"
  done
  sleep 0.8

  for entry in "${metadata_nodes[@]}"; do
    IFS='|' read -r app_target node_id listen data_dir snapshot_dir <<<"$entry"
    launch_node "$app_target" "$node_id" "$listen" "$data_dir" "$snapshot_dir"
  done
  sleep 1

  for entry in "${storage_nodes[@]}"; do
    IFS='|' read -r app_target node_id listen data_dir snapshot_dir <<<"$entry"
    launch_node "$app_target" "$node_id" "$listen" "$data_dir" "$snapshot_dir"
  done

  trap - ERR
  echo "startup complete: views=2 metadata=3 storage=6"
}

main "$@"
