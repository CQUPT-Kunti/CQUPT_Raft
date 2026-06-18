#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

main() {
  local -a ordered_nodes=(
    "store-6"
    "store-5"
    "store-4"
    "store-3"
    "store-2"
    "store-1"
    "meta-3"
    "meta-2"
    "meta-1"
    "view-2"
    "view-1"
  )

  local node_dir
  for node_dir in "${ordered_nodes[@]}"; do
    bash "$SCRIPT_DIR/nodes/$node_dir/node.sh" stop
  done
}

main "$@"
