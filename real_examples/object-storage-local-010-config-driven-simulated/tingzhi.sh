#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BIN_DIR="${BIN_DIR:-$REPO_ROOT/build/linux}"
CONFIG_PATH="$SCRIPT_DIR/cluster.json"
PID_DIR="$SCRIPT_DIR/pids"

process_matches_example() {
  local pid="$1"
  local app_target="$2"
  local node_id="$3"

  local args
  args="$(ps -p "$pid" -o args= 2>/dev/null || true)"
  if [[ -z "$args" ]]; then
    return 1
  fi

  [[ "$args" == *"$BIN_DIR/$app_target"* ]] || return 1
  [[ "$args" == *"--config $CONFIG_PATH"* ]] || return 1
  [[ "$args" == *"--node_id $node_id"* ]] || return 1
  return 0
}

stop_node() {
  local app_target="$1"
  local node_id="$2"
  local pid_file="$PID_DIR/$node_id.pid"

  if [[ ! -f "$pid_file" ]]; then
    echo "skip node_id=$node_id reason=missing_pid_file"
    return 0
  fi

  local pid
  pid="$(cat "$pid_file")"
  if [[ ! "$pid" =~ ^[0-9]+$ ]]; then
    echo "remove stale pid file node_id=$node_id reason=invalid_pid pid_value=$pid"
    rm -f "$pid_file"
    return 0
  fi

  if ! kill -0 "$pid" 2>/dev/null; then
    echo "remove stale pid file node_id=$node_id pid=$pid reason=process_not_running"
    rm -f "$pid_file"
    return 0
  fi

  if ! process_matches_example "$pid" "$app_target" "$node_id"; then
    echo "remove stale pid file node_id=$node_id pid=$pid reason=pid_not_owned_by_this_example"
    rm -f "$pid_file"
    return 0
  fi

  kill "$pid" 2>/dev/null || true
  for _ in $(seq 1 25); do
    if ! kill -0 "$pid" 2>/dev/null; then
      break
    fi
    sleep 0.2
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid" 2>/dev/null || true
    for _ in $(seq 1 10); do
      if ! kill -0 "$pid" 2>/dev/null; then
        break
      fi
      sleep 0.1
    done
  fi

  if kill -0 "$pid" 2>/dev/null; then
    echo "failed to stop node_id=$node_id pid=$pid" >&2
    return 1
  fi

  rm -f "$pid_file"
  echo "stopped node_id=$node_id pid=$pid"
}

main() {
  mkdir -p "$PID_DIR"

  local -a ordered_nodes=(
    "storage_node_app|store-6"
    "storage_node_app|store-5"
    "storage_node_app|store-4"
    "storage_node_app|store-3"
    "storage_node_app|store-2"
    "storage_node_app|store-1"
    "metadata_node_app|meta-3"
    "metadata_node_app|meta-2"
    "metadata_node_app|meta-1"
    "view_node_app|view-2"
    "view_node_app|view-1"
  )

  local entry
  local app_target
  local node_id
  for entry in "${ordered_nodes[@]}"; do
    IFS='|' read -r app_target node_id <<<"$entry"
    stop_node "$app_target" "$node_id"
  done
}

main "$@"
