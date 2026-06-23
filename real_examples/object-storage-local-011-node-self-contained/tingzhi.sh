#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

main() {
  local -a ordered_node_dirs=(
    "$SCRIPT_DIR/nodes/store-6"
    "$SCRIPT_DIR/nodes/store-5"
    "$SCRIPT_DIR/nodes/store-4"
    "$SCRIPT_DIR/nodes/store-3"
    "$SCRIPT_DIR/nodes/store-2"
    "$SCRIPT_DIR/nodes/store-1"
    "$SCRIPT_DIR/nodes/meta-3"
    "$SCRIPT_DIR/nodes/meta-2"
    "$SCRIPT_DIR/nodes/meta-1"
    "$SCRIPT_DIR/nodes/view-2"
    "$SCRIPT_DIR/nodes/view-1"
  )

  local node_dir
  for node_dir in "${ordered_node_dirs[@]}"; do
    if [[ -x "$node_dir/stop.sh" ]]; then
      "$node_dir/stop.sh"
    else
      echo "skip node_dir=$node_dir reason=missing_stop_script"
    fi
  done
}

main "$@"
