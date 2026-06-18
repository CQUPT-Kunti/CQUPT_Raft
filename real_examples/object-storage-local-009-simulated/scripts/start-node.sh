#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXAMPLE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$EXAMPLE_ROOT/../.." && pwd)"
BIN_DIR="${BIN_DIR:-$REPO_ROOT/build/linux}"
CONFIG_PATH="$EXAMPLE_ROOT/cluster.json"
LOG_DIR="$EXAMPLE_ROOT/logs"
PID_DIR="$EXAMPLE_ROOT/pids"
NODE_CONF_PATH="${1:?missing node.conf path}"

require_binary() {
  local target="$1"
  if [[ ! -x "$BIN_DIR/$target" ]]; then
    echo "missing binary: $BIN_DIR/$target" >&2
    exit 1
  fi
}

load_node_conf() {
  unset APP_TARGET NODE_ID LISTEN DATA_DIR SNAPSHOT_DIR
  # shellcheck disable=SC1090
  source "$NODE_CONF_PATH"
  : "${APP_TARGET:?missing APP_TARGET in $NODE_CONF_PATH}"
  : "${NODE_ID:?missing NODE_ID in $NODE_CONF_PATH}"
  : "${LISTEN:?missing LISTEN in $NODE_CONF_PATH}"
  : "${DATA_DIR:?missing DATA_DIR in $NODE_CONF_PATH}"
  SNAPSHOT_DIR="${SNAPSHOT_DIR:-}"
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
  require_binary "$APP_TARGET"

  mkdir -p "$LOG_DIR" "$PID_DIR" "$EXAMPLE_ROOT/$DATA_DIR"
  if [[ -n "$SNAPSHOT_DIR" ]]; then
    mkdir -p "$EXAMPLE_ROOT/$SNAPSHOT_DIR"
  fi

  local pid_file="$PID_DIR/$NODE_ID.pid"
  local log_file="$LOG_DIR/$NODE_ID.log"
  if [[ -f "$pid_file" ]]; then
    local old_pid
    old_pid="$(cat "$pid_file")"
    if [[ "$old_pid" =~ ^[0-9]+$ ]] && kill -0 "$old_pid" 2>/dev/null && process_matches_example "$old_pid"; then
      echo "already running node_id=$NODE_ID pid=$old_pid"
      return 0
    fi
    rm -f "$pid_file"
  fi

  (
    cd "$EXAMPLE_ROOT"
    exec nohup "$BIN_DIR/$APP_TARGET" \
      --config "$CONFIG_PATH" \
      --node_id "$NODE_ID" \
      --data_dir "$DATA_DIR" \
      --listen "$LISTEN"
  ) >"$log_file" 2>&1 < /dev/null &

  local pid=$!
  echo "$pid" > "$pid_file"
  sleep 0.3
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "failed to start $NODE_ID, see $log_file" >&2
    return 1
  fi

  echo "started node_id=$NODE_ID target=$APP_TARGET pid=$pid listen=$LISTEN log=$log_file"
}

main "$@"
