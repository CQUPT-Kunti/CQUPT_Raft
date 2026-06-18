#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXAMPLE_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
ACTION="${1:-start}"
case "$ACTION" in
  start) exec bash "$EXAMPLE_ROOT/scripts/start-node.sh" "$SCRIPT_DIR/node.conf" ;;
  stop) exec bash "$EXAMPLE_ROOT/scripts/stop-node.sh" "$SCRIPT_DIR/node.conf" ;;
  *) echo "unsupported action: $ACTION (expected: start|stop)" >&2; exit 1 ;;
esac
