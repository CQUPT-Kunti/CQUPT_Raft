#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
declare -a STARTED_NODE_SCRIPTS=()

cleanup_started_nodes() {
  local node_script
  for ((idx=${#STARTED_NODE_SCRIPTS[@]}-1; idx>=0; --idx)); do
    node_script="${STARTED_NODE_SCRIPTS[$idx]}"
    bash "$node_script" stop || true
  done
}

start_node_dir() {
  local node_dir="$1"
  local node_script="$SCRIPT_DIR/nodes/$node_dir/node.sh"
  local output
  output="$(bash "$node_script" start 2>&1)"
  printf '%s\n' "$output"
  if [[ "$output" == started* ]]; then
    STARTED_NODE_SCRIPTS+=("$node_script")
  fi
}

main() {
  trap 'cleanup_started_nodes' ERR

  start_node_dir "view-1"
  start_node_dir "view-2"
  sleep 0.8

  start_node_dir "meta-1"
  start_node_dir "meta-2"
  start_node_dir "meta-3"
  sleep 1

  start_node_dir "store-1"
  start_node_dir "store-2"
  start_node_dir "store-3"
  start_node_dir "store-4"
  start_node_dir "store-5"
  start_node_dir "store-6"

  trap - ERR
  echo "startup complete: views=2 metadata=3 storage=6"
}

main "$@"
