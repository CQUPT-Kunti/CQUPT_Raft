#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXAMPLE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$EXAMPLE_ROOT/../.." && pwd)"
BIN_DIR="${BIN_DIR:-$REPO_ROOT/build/linux}"
CONFIG_PATH="$EXAMPLE_ROOT/cluster.json"
PID_DIR="$EXAMPLE_ROOT/pids"
NODE_CONF_PATH="${1:?missing node.conf path}"

load_node_conf() {
  unset APP_TARGET NODE_ID LISTEN DATA_DIR SNAPSHOT_DIR
  # shellcheck disable=SC1090
  source "$NODE_CONF_PATH"
  : "${APP_TARGET:?missing APP_TARGET in $NODE_CONF_PATH}"
  : "${NODE_ID:?missing NODE_ID in $NODE_CONF_PATH}"
}

process_matches_example() {
  local pid="$1"
  local args
  args="$(ps -p "$pid" -o args= 2>/dev/null || true)"
  if [[ -z "$args" ]]; then
    return 1
  fi

  [[ "$args" == *"$BIN_DIR/$APP_TARGET"* ]] || return 1
  [[ "$args" == *"--config $CONFIG_PATH"* ]] || return 1
  [[ "$args" == *"--node_id $NODE_ID"* ]] || return 1
  return 0
}

main() {
  load_node_conf
  mkdir -p "$PID_DIR"

  local pid_file="$PID_DIR/$NODE_ID.pid"
  if [[ ! -f "$pid_file" ]]; then
    echo "skip node_id=$NODE_ID reason=missing_pid_file"
    return 0
  fi

  local pid
  pid="$(cat "$pid_file")"
  if [[ ! "$pid" =~ ^[0-9]+$ ]]; then
    echo "remove stale pid file node_id=$NODE_ID reason=invalid_pid pid_value=$pid"
    rm -f "$pid_file"
    return 0
  fi

  if ! kill -0 "$pid" 2>/dev/null; then
    echo "remove stale pid file node_id=$NODE_ID pid=$pid reason=process_not_running"
    rm -f "$pid_file"
    return 0
  fi

  if ! process_matches_example "$pid"; then
    echo "remove stale pid file node_id=$NODE_ID pid=$pid reason=pid_not_owned_by_this_example"
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
    echo "failed to stop node_id=$NODE_ID pid=$pid" >&2
    return 1
  fi

  rm -f "$pid_file"
  echo "stopped node_id=$NODE_ID pid=$pid"
}

main "$@"
