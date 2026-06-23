#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_GENERATOR="$SCRIPT_DIR/generate_cluster_config.sh"

declare -a STARTED_NODE_DIRS=()

rollback_started_nodes() {
  local node_dir
  for ((idx=${#STARTED_NODE_DIRS[@]}-1; idx>=0; --idx)); do
    node_dir="${STARTED_NODE_DIRS[$idx]}"
    "$node_dir/stop.sh" >/dev/null 2>&1 || true
  done
}

start_node_dir() {
  local node_dir="$1"
  "$node_dir/start.sh"
  STARTED_NODE_DIRS+=("$node_dir")
}

main() {
  trap 'rollback_started_nodes' ERR

  "$CONFIG_GENERATOR" >/dev/null

  start_node_dir "$SCRIPT_DIR/nodes/view-1"
  start_node_dir "$SCRIPT_DIR/nodes/view-2"
  sleep 0.8

  start_node_dir "$SCRIPT_DIR/nodes/meta-1"
  start_node_dir "$SCRIPT_DIR/nodes/meta-2"
  start_node_dir "$SCRIPT_DIR/nodes/meta-3"
  sleep 1

  start_node_dir "$SCRIPT_DIR/nodes/store-1"
  start_node_dir "$SCRIPT_DIR/nodes/store-2"
  start_node_dir "$SCRIPT_DIR/nodes/store-3"
  start_node_dir "$SCRIPT_DIR/nodes/store-4"
  start_node_dir "$SCRIPT_DIR/nodes/store-5"
  start_node_dir "$SCRIPT_DIR/nodes/store-6"

  trap - ERR
  echo "startup complete: views=2 metadata=3 storage=6"
}

main "$@"
