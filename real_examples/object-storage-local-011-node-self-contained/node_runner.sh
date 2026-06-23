#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <node.conf> [start|stop|restart|status]" >&2
  exit 1
fi

NODE_CONF_PATH="$1"
ACTION="${2:-start}"

if [[ ! -f "$NODE_CONF_PATH" ]]; then
  echo "missing node.conf: $NODE_CONF_PATH" >&2
  exit 1
fi

# shellcheck source=/dev/null
source "$NODE_CONF_PATH"

: "${APP_TARGET:?missing APP_TARGET}"
: "${NODE_ID:?missing NODE_ID}"
: "${LISTEN:?missing LISTEN}"
: "${DATA_DIR:?missing DATA_DIR}"
: "${LOG_FILE:?missing LOG_FILE}"
: "${PID_FILE:?missing PID_FILE}"

NODE_CONF_DIR="$(cd "$(dirname "$NODE_CONF_PATH")" && pwd)"
EXAMPLE_DIR="$(cd "$NODE_CONF_DIR/../.." && pwd)"
REPO_ROOT="$(cd "$EXAMPLE_DIR/../.." && pwd)"
BIN_DIR="${BIN_DIR:-$REPO_ROOT/build/linux}"
CONFIG_PATH="${CONFIG_PATH:-$EXAMPLE_DIR/cluster.json}"

resolve_node_path() {
  local path_value="$1"
  if [[ "$path_value" = /* ]]; then
    printf '%s\n' "$path_value"
  else
    printf '%s\n' "$NODE_CONF_DIR/$path_value"
  fi
}

DATA_DIR_PATH="$(resolve_node_path "$DATA_DIR")"
LOG_FILE_PATH="$(resolve_node_path "$LOG_FILE")"
PID_FILE_PATH="$(resolve_node_path "$PID_FILE")"
if [[ -n "${SNAPSHOT_DIR:-}" ]]; then
  SNAPSHOT_DIR_PATH="$(resolve_node_path "$SNAPSHOT_DIR")"
fi

require_binary() {
  if [[ ! -x "$BIN_DIR/$APP_TARGET" ]]; then
    echo "missing binary: $BIN_DIR/$APP_TARGET" >&2
    exit 1
  fi
}

process_matches_node() {
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

stop_pid_if_running() {
  local pid="$1"
  if ! kill -0 "$pid" 2>/dev/null; then
    return 0
  fi

  kill "$pid" 2>/dev/null || true
  for _ in $(seq 1 25); do
    if ! kill -0 "$pid" 2>/dev/null; then
      return 0
    fi
    sleep 0.2
  done

  kill -9 "$pid" 2>/dev/null || true
  for _ in $(seq 1 10); do
    if ! kill -0 "$pid" 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done

  return 1
}

start_node() {
  require_binary

  mkdir -p \
    "$(dirname "$DATA_DIR_PATH")" \
    "$(dirname "$LOG_FILE_PATH")" \
    "$(dirname "$PID_FILE_PATH")"

  if [[ -n "${SNAPSHOT_DIR:-}" ]]; then
    mkdir -p "$SNAPSHOT_DIR_PATH"
  fi

  if [[ -f "$PID_FILE_PATH" ]]; then
    local old_pid
    old_pid="$(cat "$PID_FILE_PATH")"
    if [[ "$old_pid" =~ ^[0-9]+$ ]] && kill -0 "$old_pid" 2>/dev/null; then
      if process_matches_node "$old_pid"; then
        echo "$NODE_ID already running pid=$old_pid"
        return 0
      fi
      echo "$NODE_ID pid file points to unrelated process pid=$old_pid" >&2
      return 1
    fi
    rm -f "$PID_FILE_PATH"
  fi

  (
    cd "$EXAMPLE_DIR"
    setsid "$BIN_DIR/$APP_TARGET" \
      --config "$CONFIG_PATH" \
      --node_id "$NODE_ID" \
      --data_dir "$DATA_DIR_PATH" \
      --listen "$LISTEN" \
      >"$LOG_FILE_PATH" 2>&1 < /dev/null &
    echo "$!" > "$PID_FILE_PATH"
  )

  local pid
  pid="$(cat "$PID_FILE_PATH")"
  sleep 0.3
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "failed to start $NODE_ID, see $LOG_FILE_PATH" >&2
    return 1
  fi

  echo "started node_id=$NODE_ID target=$APP_TARGET pid=$pid listen=$LISTEN log=$LOG_FILE_PATH"
}

stop_node() {
  if [[ ! -f "$PID_FILE_PATH" ]]; then
    echo "skip node_id=$NODE_ID reason=missing_pid_file"
    return 0
  fi

  local pid
  pid="$(cat "$PID_FILE_PATH")"
  if [[ ! "$pid" =~ ^[0-9]+$ ]]; then
    echo "remove stale pid file node_id=$NODE_ID reason=invalid_pid pid_value=$pid"
    rm -f "$PID_FILE_PATH"
    return 0
  fi

  if ! kill -0 "$pid" 2>/dev/null; then
    echo "remove stale pid file node_id=$NODE_ID pid=$pid reason=process_not_running"
    rm -f "$PID_FILE_PATH"
    return 0
  fi

  if ! process_matches_node "$pid"; then
    echo "refusing to stop unrelated pid=$pid for node_id=$NODE_ID" >&2
    return 1
  fi

  if ! stop_pid_if_running "$pid"; then
    echo "failed to stop node_id=$NODE_ID pid=$pid" >&2
    return 1
  fi

  rm -f "$PID_FILE_PATH"
  echo "stopped node_id=$NODE_ID pid=$pid"
}

status_node() {
  if [[ ! -f "$PID_FILE_PATH" ]]; then
    echo "node_id=$NODE_ID status=stopped reason=missing_pid_file"
    return 1
  fi

  local pid
  pid="$(cat "$PID_FILE_PATH")"
  if [[ ! "$pid" =~ ^[0-9]+$ ]]; then
    echo "node_id=$NODE_ID status=stopped reason=invalid_pid pid_value=$pid"
    return 1
  fi

  if ! kill -0 "$pid" 2>/dev/null; then
    echo "node_id=$NODE_ID status=stopped pid=$pid reason=process_not_running"
    return 1
  fi

  if ! process_matches_node "$pid"; then
    echo "node_id=$NODE_ID status=unknown pid=$pid reason=pid_not_owned_by_this_node" >&2
    return 1
  fi

  echo "node_id=$NODE_ID status=running pid=$pid listen=$LISTEN log=$LOG_FILE_PATH"
}

case "$ACTION" in
  start)
    start_node
    ;;
  stop)
    stop_node
    ;;
  restart)
    stop_node || true
    start_node
    ;;
  status)
    status_node
    ;;
  *)
    echo "unsupported action: $ACTION (expected: start|stop|restart|status)" >&2
    exit 1
    ;;
esac
