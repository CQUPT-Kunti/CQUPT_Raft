#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BIN_DIR="${BIN_DIR:-$REPO_ROOT/build/linux}"
CLIENT_BIN="$BIN_DIR/storage_client"
METADATA_CLIENT_BIN="$BIN_DIR/raft_metadata_client"
STORAGE_NODE_BIN="$BIN_DIR/storage_node_app"
METADATA_NODE_BIN="$BIN_DIR/metadata_node_app"
CONFIG_GENERATOR="$SCRIPT_DIR/generate_cluster_config.sh"
CONFIG_PATH="$SCRIPT_DIR/cluster.json"
JOIN_CONFIG_PATH="$SCRIPT_DIR/storage-join-store-7.json"
METADATA_LEARNER_CONFIG_PATH="$SCRIPT_DIR/metadata-learner-4.json"
METADATA_LEARNER_2_CONFIG_PATH="$SCRIPT_DIR/metadata-learner-5.json"
BUCKET="${BUCKET:-example-bucket}"
TEST_FILES_DIR="${TEST_FILES_DIR:-$REPO_ROOT/tests/test_file}"
OBJECT_PREFIX_ROOT="${OBJECT_PREFIX_ROOT:-tests/test_file/runtime-storage-join}"
DOWNLOAD_DIR="$SCRIPT_DIR/downloads"
LOG_DIR="$SCRIPT_DIR/logs"
PID_DIR="$SCRIPT_DIR/pids"
ACTION="${1:-roundtrip}"
DYNAMIC_STORAGE_NODE_ID="${DYNAMIC_STORAGE_NODE_ID:-store-7}"
DYNAMIC_STORAGE_LISTEN="${DYNAMIC_STORAGE_LISTEN:-127.0.0.1:9507}"
DYNAMIC_STORAGE_DATA_DIR="${DYNAMIC_STORAGE_DATA_DIR:-nodes/store-7/data}"
DYNAMIC_METADATA_NODE_ID="${DYNAMIC_METADATA_NODE_ID:-meta-4}"
DYNAMIC_METADATA_LISTEN="${DYNAMIC_METADATA_LISTEN:-127.0.0.1:9404}"
DYNAMIC_METADATA_DATA_DIR="${DYNAMIC_METADATA_DATA_DIR:-nodes/meta-4/data}"
DYNAMIC_METADATA_SNAPSHOT_DIR="${DYNAMIC_METADATA_SNAPSHOT_DIR:-nodes/meta-4/snapshots}"
DYNAMIC_METADATA_NODE_ID_2="${DYNAMIC_METADATA_NODE_ID_2:-meta-5}"
DYNAMIC_METADATA_LISTEN_2="${DYNAMIC_METADATA_LISTEN_2:-127.0.0.1:9405}"
DYNAMIC_METADATA_DATA_DIR_2="${DYNAMIC_METADATA_DATA_DIR_2:-nodes/meta-5/data}"
DYNAMIC_METADATA_SNAPSHOT_DIR_2="${DYNAMIC_METADATA_SNAPSHOT_DIR_2:-nodes/meta-5/snapshots}"
JOIN_WAIT_SECONDS="${JOIN_WAIT_SECONDS:-30}"
STATUS_POLL_INTERVAL_SECONDS="${STATUS_POLL_INTERVAL_SECONDS:-1}"
METADATA_JOIN_WAIT_SECONDS="${METADATA_JOIN_WAIT_SECONDS:-45}"
FAILOVER_STOP_VIEW_NODE_ID="${FAILOVER_STOP_VIEW_NODE_ID:-view-1}"
FAILOVER_STOP_VIEW_ENDPOINT="${FAILOVER_STOP_VIEW_ENDPOINT:-127.0.0.1:9301}"
FAILOVER_SURVIVOR_VIEW_NODE_ID="${FAILOVER_SURVIVOR_VIEW_NODE_ID:-view-2}"
FAILOVER_SURVIVOR_VIEW_ENDPOINT="${FAILOVER_SURVIVOR_VIEW_ENDPOINT:-127.0.0.1:9302}"
FAILOVER_SURVIVOR_CONFIG_PATH="$SCRIPT_DIR/logs/failover-view-2-client.json"
PARALLEL_STATUS_LOG="$SCRIPT_DIR/logs/status.log"
PARALLEL_CREATE_BUCKET_LOG="$SCRIPT_DIR/logs/create-bucket.log"
PARALLEL_UPLOAD_LOG="$SCRIPT_DIR/logs/upload.log"
PARALLEL_DOWNLOAD_LOG="$SCRIPT_DIR/logs/download.log"
PARALLEL_UPLOAD_CONCURRENCY=""
PARALLEL_MAX_INFLIGHT_BYTES=""
PARALLEL_REPLICA_FANOUT_CONCURRENCY=""
PARALLEL_PREFERRED_MIN_BYTES=$((1024 * 1024 * 1024))
PARALLEL_PREFERRED_MAX_BYTES=$((10 * 1024 * 1024 * 1024))
PARALLEL_REQUIRED_STABLE_LIVE_POLLS="${PARALLEL_REQUIRED_STABLE_LIVE_POLLS:-3}"

declare -a TEST_FILES=()

PARALLEL_SOURCE_FILE=""
PARALLEL_SOURCE_FILE_SIZE_BYTES=0

require_client() {
  if [[ ! -x "$CLIENT_BIN" ]]; then
    echo "missing binary: $CLIENT_BIN" >&2
    exit 1
  fi
}

require_storage_node_binary() {
  if [[ ! -x "$STORAGE_NODE_BIN" ]]; then
    echo "missing binary: $STORAGE_NODE_BIN" >&2
    exit 1
  fi
}

require_metadata_node_binary() {
  if [[ ! -x "$METADATA_NODE_BIN" ]]; then
    echo "missing binary: $METADATA_NODE_BIN" >&2
    exit 1
  fi
}

require_metadata_client() {
  if [[ ! -x "$METADATA_CLIENT_BIN" ]]; then
    echo "missing binary: $METADATA_CLIENT_BIN" >&2
    exit 1
  fi
}

require_test_files_dir() {
  if [[ ! -d "$TEST_FILES_DIR" ]]; then
    echo "missing test files dir: $TEST_FILES_DIR" >&2
    exit 1
  fi
}

ensure_generated_cluster_config() {
  "$CONFIG_GENERATOR" >/dev/null
}

read_config_number() {
  local query="$1"
  jq -r "$query" "$CONFIG_PATH"
}

load_parallel_runtime_config() {
  PARALLEL_UPLOAD_CONCURRENCY="$(read_config_number '.store.upload_concurrency')"
  PARALLEL_MAX_INFLIGHT_BYTES="$(read_config_number '.store.max_inflight_bytes')"
  PARALLEL_REPLICA_FANOUT_CONCURRENCY="$(read_config_number '.store.replica_fanout_concurrency')"
}

collect_test_files() {
  mapfile -t TEST_FILES < <(find "$TEST_FILES_DIR" -type f | sort)
  if [[ "${#TEST_FILES[@]}" -eq 0 ]]; then
    echo "no files found under $TEST_FILES_DIR" >&2
    exit 1
  fi
}

select_parallel_test_file() {
  local -a candidates=()
  mapfile -t candidates < <(find "$TEST_FILES_DIR" -type f -printf '%s\t%p\n' | sort -nr)
  if [[ "${#candidates[@]}" -eq 0 ]]; then
    echo "no files found under $TEST_FILES_DIR" >&2
    exit 1
  fi

  local entry
  local size
  local path
  for entry in "${candidates[@]}"; do
    size="${entry%%$'\t'*}"
    path="${entry#*$'\t'}"
    if (( size >= PARALLEL_PREFERRED_MIN_BYTES && size <= PARALLEL_PREFERRED_MAX_BYTES )); then
      PARALLEL_SOURCE_FILE="$path"
      PARALLEL_SOURCE_FILE_SIZE_BYTES="$size"
      TEST_FILES=("$path")
      return 0
    fi
  done

  entry="${candidates[0]}"
  size="${entry%%$'\t'*}"
  path="${entry#*$'\t'}"
  PARALLEL_SOURCE_FILE="$path"
  PARALLEL_SOURCE_FILE_SIZE_BYTES="$size"
  TEST_FILES=("$path")
}

relative_path_from_test_dir() {
  local source_file="$1"
  local relative_path
  relative_path="${source_file#$TEST_FILES_DIR/}"
  printf '%s\n' "$relative_path"
}

roundtrip_object_prefix() {
  printf '%s/%s-%s\n' "$OBJECT_PREFIX_ROOT" "$(date +%s)" "$$"
}

object_key_for_file() {
  local object_prefix="$1"
  local source_file="$2"
  local relative_path
  relative_path="$(relative_path_from_test_dir "$source_file")"
  printf '%s/%s\n' "$object_prefix" "$relative_path"
}

download_path_for_file() {
  local object_prefix="$1"
  local source_file="$2"
  local relative_path
  relative_path="$(relative_path_from_test_dir "$source_file")"
  printf '%s/%s/%s\n' "$DOWNLOAD_DIR" "${object_prefix#tests/test_file/}" "$relative_path"
}

run_status() {
  local client_config
  client_config="$(active_client_config_path)"
  echo "[status]"
  (
    cd "$SCRIPT_DIR"
    "$CLIENT_BIN" status --config "$client_config"
  )
  emit_dynamic_metadata_diagnostics
}

run_status_capture() {
  local output_file="$1"
  local client_config
  client_config="$(active_client_config_path)"
  echo "[status]"
  (
    cd "$SCRIPT_DIR"
    "$CLIENT_BIN" status --config "$client_config"
  ) | tee "$output_file"
  emit_dynamic_metadata_diagnostics
}

format_bytes() {
  local size="$1"
  if command -v numfmt >/dev/null 2>&1; then
    numfmt --to=iec-i --suffix=B "$size"
    return 0
  fi
  printf '%sB\n' "$size"
}

sha256_file() {
  sha256sum "$1" | awk '{print $1}'
}

file_size_bytes() {
  stat -c '%s' "$1"
}

view_pid_file() {
  local node_id="$1"
  printf '%s/%s.pid\n' "$PID_DIR" "$node_id"
}

view_log_file() {
  local node_id="$1"
  printf '%s/%s.log\n' "$LOG_DIR" "$node_id"
}

process_matches_view_node() {
  local pid="$1"
  local node_id="$2"
  local args
  args="$(ps -p "$pid" -o args= 2>/dev/null || true)"
  if [[ -z "$args" ]]; then
    return 1
  fi

  [[ "$args" == *"$BIN_DIR/view_node_app"* ]] || return 1
  [[ "$args" == *"--config $CONFIG_PATH"* ]] || return 1
  [[ "$args" == *"--node_id $node_id"* ]] || return 1
  return 0
}

view_node_running() {
  local node_id="$1"
  local pid_file
  pid_file="$(view_pid_file "$node_id")"
  if [[ ! -f "$pid_file" ]]; then
    return 1
  fi

  local pid
  pid="$(cat "$pid_file")"
  [[ "$pid" =~ ^[0-9]+$ ]] || return 1
  kill -0 "$pid" 2>/dev/null || return 1
  process_matches_view_node "$pid" "$node_id"
}

failover_view_active() {
  ! view_node_running "$FAILOVER_STOP_VIEW_NODE_ID" && \
    view_node_running "$FAILOVER_SURVIVOR_VIEW_NODE_ID"
}

write_surviving_view_client_config() {
  mkdir -p "$LOG_DIR"

  local tmp_config
  tmp_config="$(mktemp)"
  awk -v endpoint="$FAILOVER_SURVIVOR_VIEW_ENDPOINT" '
    BEGIN { inserted = 0 }
    {
      print
      if (!inserted && $0 ~ /"cluster_id"[[:space:]]*:/) {
        print "  \"view_endpoint\": \"" endpoint "\","
        inserted = 1
      }
    }
    END {
      if (!inserted) {
        exit 1
      }
    }
  ' "$CONFIG_PATH" > "$tmp_config"
  mv "$tmp_config" "$FAILOVER_SURVIVOR_CONFIG_PATH"
}

active_client_config_path() {
  if failover_view_active; then
    write_surviving_view_client_config
    printf '%s\n' "$FAILOVER_SURVIVOR_CONFIG_PATH"
    return 0
  fi
  printf '%s\n' "$CONFIG_PATH"
}

extract_status_target_endpoint() {
  local output_file="$1"
  sed -n -E 's/^status OK .* target_endpoint=([^ ]+).*/\1/p' "$output_file" | tail -n1 || true
}

extract_status_summary_count() {
  local summary_line="$1"
  local field_name="$2"
  sed -n -E "s/^status OK .* ${field_name}=([0-9]+)( .*)?$/\\1/p" <<<"$summary_line" | tail -n1 || true
}

status_reports_live_cluster() {
  local output_file="$1"
  local summary_line
  local non_authority_line
  local metadata_live_count
  local storage_live_count
  local metadata_node_count
  local storage_node_count

  summary_line="$(grep -E '^status OK ' "$output_file" | tail -n1 || true)"
  non_authority_line="$(grep -E '^non_authority_boundary ' "$output_file" | tail -n1 || true)"
  metadata_live_count="$(grep -E '^metadata_node ' "$output_file" | grep -E 'liveness=live' | wc -l | awk '{print $1}')"
  storage_live_count="$(grep -E '^storage_node ' "$output_file" | grep -E 'liveness=live' | wc -l | awk '{print $1}')"
  metadata_node_count="$(extract_status_summary_count "$summary_line" "metadata_nodes")"
  storage_node_count="$(extract_status_summary_count "$summary_line" "storage_nodes")"

  [[ -n "$summary_line" ]] || return 1
  [[ "$metadata_node_count" =~ ^[0-9]+$ ]] || return 1
  [[ "$storage_node_count" =~ ^[0-9]+$ ]] || return 1
  (( metadata_node_count >= 1 )) || return 1
  (( storage_node_count >= 1 )) || return 1
  [[ -n "$non_authority_line" ]] || return 1
  [[ "$non_authority_line" == *"raft_membership_authority=false"* ]] || return 1
  [[ "$non_authority_line" == *"object_manifest_authority=false"* ]] || return 1
  (( metadata_live_count == metadata_node_count )) || return 1
  (( storage_live_count == storage_node_count )) || return 1

  if grep -E '^leader_hint ' "$output_file" | grep -E 'leader_hint\.endpoint=' >/dev/null 2>&1; then
    return 0
  fi

  grep -E '^metadata_node ' "$output_file" | grep -E 'raft_role=leader|membership_observation=voter' >/dev/null 2>&1
}

wait_for_live_cluster() {
  local required_stable_polls="$1"
  local stable_polls=0
  local started_at
  local deadline
  local output_file
  started_at="$(date +%s)"
  deadline=$((started_at + JOIN_WAIT_SECONDS))
  output_file="$(mktemp)"
  trap 'rm -f "$output_file"' RETURN

  while (( $(date +%s) <= deadline )); do
    run_status_capture "$output_file" >/dev/null
    if status_reports_live_cluster "$output_file"; then
      stable_polls=$((stable_polls + 1))
      if (( stable_polls >= required_stable_polls )); then
        cp "$output_file" "$PARALLEL_STATUS_LOG"
        echo "[parallel-roundtrip] cluster live and stable polls=$stable_polls"
        trap - RETURN
        return 0
      fi
    else
      stable_polls=0
    fi
    sleep "$STATUS_POLL_INTERVAL_SECONDS"
  done

  echo "[parallel-roundtrip] FAILED reason=cluster_not_stable wait_seconds=$JOIN_WAIT_SECONDS required_stable_polls=$required_stable_polls" >&2
  tail -n 50 "$output_file" >&2 || true
  return 1
}

status_reports_surviving_view_ready() {
  local output_file="$1"
  local summary_line
  local non_authority_line
  local target_endpoint
  local survivor_line
  local metadata_live_count
  local metadata_node_count
  target_endpoint="$(extract_status_target_endpoint "$output_file")"
  summary_line="$(grep -E '^status OK ' "$output_file" | tail -n1 || true)"
  non_authority_line="$(grep -E '^non_authority_boundary ' "$output_file" | tail -n1 || true)"
  metadata_live_count="$(grep -E '^metadata_node ' "$output_file" | grep -E 'liveness=live' | wc -l | awk '{print $1}')"
  metadata_node_count="$(extract_status_summary_count "$summary_line" "metadata_nodes")"

  [[ -n "$summary_line" ]] || return 1
  [[ "$target_endpoint" == "$FAILOVER_SURVIVOR_VIEW_ENDPOINT" ]] || return 1
  [[ -n "$non_authority_line" ]] || return 1
  [[ "$non_authority_line" == *"raft_membership_authority=false"* ]] || return 1
  [[ "$non_authority_line" == *"object_manifest_authority=false"* ]] || return 1
  [[ "$metadata_node_count" =~ ^[0-9]+$ ]] || return 1
  (( metadata_node_count >= 1 )) || return 1
  (( metadata_live_count >= 1 )) || return 1

  if ! grep -E '^leader_hint ' "$output_file" | grep -E 'leader_hint\.endpoint=' >/dev/null 2>&1; then
    grep -E '^metadata_node ' "$output_file" | grep -E 'raft_role=leader|membership_observation=voter' >/dev/null 2>&1 || return 1
  fi

  survivor_line="$(grep -E '^view_node ' "$output_file" | grep -F "node_id=$FAILOVER_SURVIVOR_VIEW_NODE_ID" | tail -n1 || true)"
  [[ -n "$survivor_line" ]] || return 1
  [[ "$survivor_line" == *"endpoint=$FAILOVER_SURVIVOR_VIEW_ENDPOINT"* ]] || return 1
  [[ "$survivor_line" == *"liveness=live"* ]] || return 1
  [[ "$survivor_line" != *"health=unavailable"* ]] || return 1
}

stop_view_node_for_failover() {
  local node_id="$FAILOVER_STOP_VIEW_NODE_ID"
  local pid_file
  pid_file="$(view_pid_file "$node_id")"
  if [[ ! -f "$pid_file" ]]; then
    echo "[failover-view] node_id=$node_id already stopped"
    return 0
  fi

  local pid
  pid="$(cat "$pid_file")"
  if [[ ! "$pid" =~ ^[0-9]+$ ]]; then
    rm -f "$pid_file"
    echo "[failover-view] removed invalid pid file node_id=$node_id"
    return 0
  fi

  if ! kill -0 "$pid" 2>/dev/null; then
    rm -f "$pid_file"
    echo "[failover-view] removed stale pid file node_id=$node_id pid=$pid"
    return 0
  fi

  if ! process_matches_view_node "$pid" "$node_id"; then
    echo "[failover-view] refusing to stop unrelated pid=$pid for node_id=$node_id" >&2
    return 1
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
    echo "[failover-view] FAILED reason=view_stop_failed node_id=$node_id pid=$pid" >&2
    return 1
  fi

  rm -f "$pid_file"
  echo "[failover-view] stopped node_id=$node_id endpoint=$FAILOVER_STOP_VIEW_ENDPOINT pid=$pid"
}

extract_metadata_leader_endpoint() {
  local output_file="$1"
  local endpoint
  endpoint="$(sed -n -E 's/.*leader_hint\.endpoint=([^ ]+).*/\1/p' "$output_file" | head -n1 || true)"
  if [[ -n "$endpoint" ]]; then
    printf '%s\n' "$endpoint"
    return 0
  fi

  endpoint="$(grep -E '^metadata_node ' "$output_file" \
    | grep -E 'raft_role=leader' \
    | sed -n -E 's/.*endpoint=([^ ]+).*/\1/p' \
    | head -n1 || true)"
  if [[ -n "$endpoint" ]]; then
    printf '%s\n' "$endpoint"
    return 0
  fi

  return 1
}

ensure_bucket_exists() {
  require_metadata_client

  local status_output
  local leader_endpoint
  status_output="$(mktemp)"

  run_status_capture "$status_output" >/dev/null
  if ! leader_endpoint="$(extract_metadata_leader_endpoint "$status_output")"; then
    echo "[bucket] FAILED reason=missing_metadata_leader_endpoint" >&2
    tail -n 50 "$status_output" >&2 || true
    rm -f "$status_output"
    return 1
  fi

  if (
    cd "$SCRIPT_DIR"
    "$METADATA_CLIENT_BIN" "$leader_endpoint" list-objects --bucket "$BUCKET"
  ) >/dev/null 2>&1; then
    echo "[bucket] existing bucket=$BUCKET leader=$leader_endpoint"
    rm -f "$status_output"
    return 0
  fi

  local request_id="runtime-ensure-bucket-${BUCKET//[^a-zA-Z0-9]/-}-$(date +%s)-$$"
  echo "[bucket] create bucket=$BUCKET leader=$leader_endpoint request_id=$request_id"
  (
    cd "$SCRIPT_DIR"
    "$METADATA_CLIENT_BIN" "$leader_endpoint" create-bucket \
      --request-id "$request_id" \
      --bucket "$BUCKET"
  ) | tee "$PARALLEL_CREATE_BUCKET_LOG"
  rm -f "$status_output"
}

process_matches_dynamic_storage() {
  local pid="$1"
  local args
  args="$(ps -p "$pid" -o args= 2>/dev/null || true)"
  if [[ -z "$args" ]]; then
    return 1
  fi

  [[ "$args" == *"$STORAGE_NODE_BIN"* ]] || return 1
  [[ "$args" == *"--config $JOIN_CONFIG_PATH"* ]] || return 1
  [[ "$args" == *"--node_id $DYNAMIC_STORAGE_NODE_ID"* ]] || return 1
  return 0
}

cleanup_dynamic_storage_if_needed() {
  local pid_file="$PID_DIR/$DYNAMIC_STORAGE_NODE_ID.pid"
  if [[ ! -f "$pid_file" ]]; then
    return 0
  fi

  local pid
  pid="$(cat "$pid_file")"
  if [[ ! "$pid" =~ ^[0-9]+$ ]]; then
    rm -f "$pid_file"
    return 0
  fi

  if ! kill -0 "$pid" 2>/dev/null; then
    rm -f "$pid_file"
    return 0
  fi

  if ! process_matches_dynamic_storage "$pid"; then
    echo "[join-storage] refusing to stop unrelated pid=$pid for node_id=$DYNAMIC_STORAGE_NODE_ID" >&2
    return 1
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
  fi
  rm -f "$pid_file"
}

metadata_slot_action_name() {
  case "$1" in
    1) printf '%s\n' "join-metadata-learner" ;;
    2) printf '%s\n' "join-metadata-learner-2" ;;
    *) return 1 ;;
  esac
}

metadata_slot_config_path() {
  case "$1" in
    1) printf '%s\n' "$METADATA_LEARNER_CONFIG_PATH" ;;
    2) printf '%s\n' "$METADATA_LEARNER_2_CONFIG_PATH" ;;
    *) return 1 ;;
  esac
}

metadata_slot_node_id() {
  case "$1" in
    1) printf '%s\n' "$DYNAMIC_METADATA_NODE_ID" ;;
    2) printf '%s\n' "$DYNAMIC_METADATA_NODE_ID_2" ;;
    *) return 1 ;;
  esac
}

metadata_slot_listen() {
  case "$1" in
    1) printf '%s\n' "$DYNAMIC_METADATA_LISTEN" ;;
    2) printf '%s\n' "$DYNAMIC_METADATA_LISTEN_2" ;;
    *) return 1 ;;
  esac
}

metadata_slot_data_dir() {
  case "$1" in
    1) printf '%s\n' "$DYNAMIC_METADATA_DATA_DIR" ;;
    2) printf '%s\n' "$DYNAMIC_METADATA_DATA_DIR_2" ;;
    *) return 1 ;;
  esac
}

metadata_slot_snapshot_dir() {
  case "$1" in
    1) printf '%s\n' "$DYNAMIC_METADATA_SNAPSHOT_DIR" ;;
    2) printf '%s\n' "$DYNAMIC_METADATA_SNAPSHOT_DIR_2" ;;
    *) return 1 ;;
  esac
}

metadata_slot_pid_file() {
  local slot="$1"
  printf '%s/%s.pid\n' "$PID_DIR" "$(metadata_slot_node_id "$slot")"
}

metadata_slot_log_file() {
  local slot="$1"
  printf '%s/%s.log\n' "$LOG_DIR" "$(metadata_slot_node_id "$slot")"
}

metadata_slot_identity_file() {
  local slot="$1"
  printf '%s/%s/node.identity\n' "$SCRIPT_DIR" "$(metadata_slot_data_dir "$slot")"
}

process_matches_dynamic_metadata_slot() {
  local pid="$1"
  local slot="$2"
  local config_path
  local node_id
  local args
  config_path="$(metadata_slot_config_path "$slot")"
  node_id="$(metadata_slot_node_id "$slot")"
  args="$(ps -p "$pid" -o args= 2>/dev/null || true)"
  if [[ -z "$args" ]]; then
    return 1
  fi

  [[ "$args" == *"$METADATA_NODE_BIN"* ]] || return 1
  [[ "$args" == *"--config $config_path"* ]] || return 1
  [[ "$args" == *"--node_id $node_id"* ]] || return 1
  return 0
}

process_matches_dynamic_metadata() {
  process_matches_dynamic_metadata_slot "$1" 1
}

cleanup_dynamic_metadata_slot_if_needed() {
  local slot="$1"
  local action_name
  local node_id
  local pid_file
  action_name="$(metadata_slot_action_name "$slot")"
  node_id="$(metadata_slot_node_id "$slot")"
  pid_file="$(metadata_slot_pid_file "$slot")"
  if [[ ! -f "$pid_file" ]]; then
    return 0
  fi

  local pid
  pid="$(cat "$pid_file")"
  if [[ ! "$pid" =~ ^[0-9]+$ ]]; then
    rm -f "$pid_file"
    return 0
  fi

  if ! kill -0 "$pid" 2>/dev/null; then
    rm -f "$pid_file"
    return 0
  fi

  if ! process_matches_dynamic_metadata_slot "$pid" "$slot"; then
    echo "[$action_name] refusing to stop unrelated pid=$pid for node_id=$node_id" >&2
    return 1
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
  fi
  rm -f "$pid_file"
}

cleanup_dynamic_metadata_if_needed() {
  cleanup_dynamic_metadata_slot_if_needed 1
}

reset_dynamic_metadata_slot_state_if_stopped() {
  local slot="$1"
  local action_name
  local node_id
  local pid_file
  local log_file
  local data_dir
  local snapshot_dir
  action_name="$(metadata_slot_action_name "$slot")"
  node_id="$(metadata_slot_node_id "$slot")"
  pid_file="$(metadata_slot_pid_file "$slot")"
  log_file="$(metadata_slot_log_file "$slot")"
  data_dir="$(metadata_slot_data_dir "$slot")"
  snapshot_dir="$(metadata_slot_snapshot_dir "$slot")"

  if dynamic_metadata_running_slot "$slot"; then
    echo "[$action_name] reusing running node_id=$node_id"
    return 0
  fi

  if [[ -f "$pid_file" ]]; then
    local stale_pid
    stale_pid="$(cat "$pid_file")"
    if [[ "$stale_pid" =~ ^[0-9]+$ ]] && kill -0 "$stale_pid" 2>/dev/null; then
      if ! process_matches_dynamic_metadata_slot "$stale_pid" "$slot"; then
        echo "[$action_name] pid file exists but pid=$stale_pid is not owned by this example" >&2
        return 1
      fi
    fi
    rm -f "$pid_file"
  fi

  rm -f "$log_file"
  rm -rf "$SCRIPT_DIR/$data_dir" "$SCRIPT_DIR/$snapshot_dir"
  echo "[$action_name] reset stopped node state node_id=$node_id data_dir=$data_dir snapshot_dir=$snapshot_dir"
}

launch_dynamic_storage() {
  require_storage_node_binary
  mkdir -p "$LOG_DIR" "$PID_DIR" "$SCRIPT_DIR/$DYNAMIC_STORAGE_DATA_DIR"

  local pid_file="$PID_DIR/$DYNAMIC_STORAGE_NODE_ID.pid"
  local log_file="$LOG_DIR/$DYNAMIC_STORAGE_NODE_ID.log"

  if [[ -f "$pid_file" ]]; then
    local old_pid
    old_pid="$(cat "$pid_file")"
    if [[ "$old_pid" =~ ^[0-9]+$ ]] && kill -0 "$old_pid" 2>/dev/null; then
      if process_matches_dynamic_storage "$old_pid"; then
        echo "[join-storage] node_id=$DYNAMIC_STORAGE_NODE_ID already running pid=$old_pid"
        return 0
      fi
      echo "[join-storage] pid file exists but pid=$old_pid is not owned by this example" >&2
      return 1
    fi
    rm -f "$pid_file"
  fi

  (
    cd "$SCRIPT_DIR"
    exec nohup "$STORAGE_NODE_BIN" \
      --config "$JOIN_CONFIG_PATH" \
      --node_id "$DYNAMIC_STORAGE_NODE_ID" \
      --data_dir "$DYNAMIC_STORAGE_DATA_DIR" \
      --listen "$DYNAMIC_STORAGE_LISTEN"
  ) >"$log_file" 2>&1 < /dev/null &

  local pid=$!
  echo "$pid" > "$pid_file"
  sleep 0.3
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "[join-storage] failed to start node_id=$DYNAMIC_STORAGE_NODE_ID, see $log_file" >&2
    return 1
  fi

  echo "[join-storage] started node_id=$DYNAMIC_STORAGE_NODE_ID pid=$pid listen=$DYNAMIC_STORAGE_LISTEN log=$log_file data_dir=$DYNAMIC_STORAGE_DATA_DIR"
}

launch_dynamic_metadata_learner_slot() {
  local slot="$1"
  local action_name
  local config_path
  local node_id
  local listen
  local data_dir
  local snapshot_dir
  local pid_file
  local log_file
  require_metadata_node_binary
  action_name="$(metadata_slot_action_name "$slot")"
  config_path="$(metadata_slot_config_path "$slot")"
  node_id="$(metadata_slot_node_id "$slot")"
  listen="$(metadata_slot_listen "$slot")"
  data_dir="$(metadata_slot_data_dir "$slot")"
  snapshot_dir="$(metadata_slot_snapshot_dir "$slot")"
  pid_file="$(metadata_slot_pid_file "$slot")"
  log_file="$(metadata_slot_log_file "$slot")"
  mkdir -p "$LOG_DIR" "$PID_DIR" "$SCRIPT_DIR/$data_dir" "$SCRIPT_DIR/$snapshot_dir"

  if [[ -f "$pid_file" ]]; then
    local old_pid
    old_pid="$(cat "$pid_file")"
    if [[ "$old_pid" =~ ^[0-9]+$ ]] && kill -0 "$old_pid" 2>/dev/null; then
      if process_matches_dynamic_metadata_slot "$old_pid" "$slot"; then
        echo "[$action_name] node_id=$node_id already running pid=$old_pid"
        return 0
      fi
      echo "[$action_name] pid file exists but pid=$old_pid is not owned by this example" >&2
      return 1
    fi
    rm -f "$pid_file"
  fi

  (
    cd "$SCRIPT_DIR"
    exec nohup "$METADATA_NODE_BIN" \
      --config "$config_path" \
      --node_id "$node_id" \
      --data_dir "$data_dir" \
      --listen "$listen"
  ) >"$log_file" 2>&1 < /dev/null &

  local pid=$!
  echo "$pid" > "$pid_file"
  sleep 0.3
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "[$action_name] failed to start node_id=$node_id, see $log_file" >&2
    return 1
  fi

  echo "[$action_name] started node_id=$node_id pid=$pid listen=$listen log=$log_file data_dir=$data_dir snapshot_dir=$snapshot_dir"
}

launch_dynamic_metadata_learner() {
  launch_dynamic_metadata_learner_slot 1
}

status_reports_dynamic_storage_live() {
  local output_file="$1"
  local summary_line
  local node_line

  summary_line="$(grep -E '^status OK ' "$output_file" | tail -n1 || true)"
  node_line="$(grep -E '^storage_node ' "$output_file" | grep -E "node_id=${DYNAMIC_STORAGE_NODE_ID}([[:space:]]|$)" | tail -n1 || true)"

  [[ -n "$summary_line" ]] || return 1
  [[ "$summary_line" == *"storage_nodes=7"* ]] || return 1
  [[ -n "$node_line" ]] || return 1
  if [[ ! "$node_line" =~ liveness=([Ll][Ii][Vv][Ee]) ]]; then
    return 1
  fi
  return 0
}

wait_for_dynamic_storage_live() {
  local output_file
  output_file="$(mktemp)"
  trap 'rm -f "$output_file"' RETURN

  local deadline=$((SECONDS + JOIN_WAIT_SECONDS))
  while (( SECONDS < deadline )); do
    run_status_capture "$output_file"
    if status_reports_dynamic_storage_live "$output_file"; then
      echo "[join-storage] observed node_id=$DYNAMIC_STORAGE_NODE_ID in cluster status as LIVE"
      return 0
    fi
    sleep "$STATUS_POLL_INTERVAL_SECONDS"
  done

  echo "[join-storage] FAILED reason=dynamic_storage_not_live node_id=$DYNAMIC_STORAGE_NODE_ID wait_seconds=$JOIN_WAIT_SECONDS" >&2
  echo "[join-storage] last_status_output:" >&2
  tail -n 50 "$output_file" >&2 || true
  if [[ -f "$LOG_DIR/$DYNAMIC_STORAGE_NODE_ID.log" ]]; then
    echo "[join-storage] storage_log_tail path=$LOG_DIR/$DYNAMIC_STORAGE_NODE_ID.log" >&2
    tail -n 50 "$LOG_DIR/$DYNAMIC_STORAGE_NODE_ID.log" >&2 || true
  fi
  return 1
}

assert_dynamic_storage_identity_logged() {
  local log_file="$LOG_DIR/$DYNAMIC_STORAGE_NODE_ID.log"
  local identity_file="$SCRIPT_DIR/$DYNAMIC_STORAGE_DATA_DIR/node.identity"
  local deadline=$((SECONDS + 5))
  while (( SECONDS < deadline )); do
    if [[ -f "$identity_file" ]]; then
      if [[ -f "$log_file" ]] && \
        grep -E "storage_node_app OK .*node_id=${DYNAMIC_STORAGE_NODE_ID} .*identity_" "$log_file" >/dev/null 2>&1; then
        echo "[join-storage] identity evidence found path=$log_file identity=$identity_file"
      else
        echo "[join-storage] identity file ready path=$identity_file"
      fi
      return 0
    fi
    sleep 0.2
  done

  if [[ ! -f "$identity_file" ]]; then
    echo "[join-storage] FAILED reason=missing_identity_file path=$identity_file" >&2
  fi
  if [[ ! -f "$log_file" ]]; then
    echo "[join-storage] FAILED reason=missing_storage_log path=$log_file" >&2
    return 1
  fi

  echo "[join-storage] FAILED reason=missing_identity_startup_evidence path=$log_file" >&2
  tail -n 50 "$log_file" >&2 || true
  return 1
}

dynamic_metadata_log_file() {
  metadata_slot_log_file 1
}

dynamic_metadata_identity_file() {
  metadata_slot_identity_file 1
}

dynamic_metadata_running_slot() {
  local slot="$1"
  local pid_file
  pid_file="$(metadata_slot_pid_file "$slot")"
  if [[ ! -f "$pid_file" ]]; then
    return 1
  fi

  local pid
  pid="$(cat "$pid_file")"
  [[ "$pid" =~ ^[0-9]+$ ]] || return 1
  kill -0 "$pid" 2>/dev/null || return 1
  process_matches_dynamic_metadata_slot "$pid" "$slot"
}

dynamic_metadata_running() {
  dynamic_metadata_running_slot 1
}

latest_dynamic_metadata_join_status_slot() {
  local slot="$1"
  local log_file
  log_file="$(metadata_slot_log_file "$slot")"
  if [[ ! -f "$log_file" ]]; then
    return 1
  fi

  grep -E "metadata_node_app candidate join (bootstrap|status|warning)" "$log_file" | tail -n1 || true
}

latest_dynamic_metadata_join_status() {
  latest_dynamic_metadata_join_status_slot 1
}

latest_dynamic_metadata_batch_promote_status_slot() {
  local slot="$1"
  local log_file
  log_file="$(metadata_slot_log_file "$slot")"
  if [[ ! -f "$log_file" ]]; then
    return 1
  fi

  grep -E "promotion_status=batch_promoted|learner_status=promoted" "$log_file" | tail -n1 || true
}

historical_dynamic_metadata_pre_promote_line_slot() {
  local slot="$1"
  local log_file
  log_file="$(metadata_slot_log_file "$slot")"
  if [[ ! -f "$log_file" ]]; then
    return 1
  fi

  grep -E "metadata_node_app candidate join (bootstrap|status|warning)" "$log_file" \
    | grep -E "committed_voter_count=3" \
    | grep -E "committed_quorum_size=2" \
    | grep -E "learner_status=pending|learner_status=ready_to_promote" \
    | tail -n1 || true
}

historical_dynamic_metadata_batch_promote_line_slot() {
  local slot="$1"
  local log_file
  log_file="$(metadata_slot_log_file "$slot")"
  if [[ ! -f "$log_file" ]]; then
    return 1
  fi

  grep -E "promotion_status=batch_promoted|learner_status=promoted" "$log_file" \
    | grep -E "committed_voter_count=5" \
    | grep -E "committed_quorum_size=3" \
    | tail -n1 || true
}

dynamic_metadata_log_has_committed_voter_count_four_slot() {
  local slot="$1"
  local log_file
  log_file="$(metadata_slot_log_file "$slot")"
  if [[ ! -f "$log_file" ]]; then
    return 1
  fi

  grep -E "committed_voter_count=4" "$log_file" >/dev/null 2>&1
}

emit_dynamic_metadata_diagnostics_slot() {
  local slot="$1"
  if ! dynamic_metadata_running_slot "$slot"; then
    return 0
  fi

  local log_file
  log_file="$(metadata_slot_log_file "$slot")"
  if [[ ! -f "$log_file" ]]; then
    return 0
  fi

  local latest_join_line
  latest_join_line="$(latest_dynamic_metadata_join_status_slot "$slot" || true)"
  if [[ -n "$latest_join_line" ]]; then
    echo "[metadata-learner-diagnostics] $latest_join_line"
  fi
}

emit_dynamic_metadata_diagnostics() {
  emit_dynamic_metadata_diagnostics_slot 1
  emit_dynamic_metadata_diagnostics_slot 2
}

assert_dynamic_metadata_identity_logged_slot() {
  local slot="$1"
  local action_name
  local node_id
  local log_file
  local identity_file
  action_name="$(metadata_slot_action_name "$slot")"
  node_id="$(metadata_slot_node_id "$slot")"
  log_file="$(metadata_slot_log_file "$slot")"
  identity_file="$(metadata_slot_identity_file "$slot")"
  local deadline=$((SECONDS + 8))
  while (( SECONDS < deadline )); do
    if [[ -f "$identity_file" ]]; then
      if [[ -f "$log_file" ]] && \
        grep -E "metadata_node_app OK .*node_id=${node_id} .*identity_" "$log_file" >/dev/null 2>&1; then
        echo "[$action_name] identity evidence found path=$log_file identity=$identity_file"
      else
        echo "[$action_name] identity file ready path=$identity_file"
      fi
      return 0
    fi
    sleep 0.2
  done

  if [[ ! -f "$identity_file" ]]; then
    echo "[$action_name] FAILED reason=missing_identity_file path=$identity_file" >&2
  fi
  if [[ ! -f "$log_file" ]]; then
    echo "[$action_name] FAILED reason=missing_metadata_log path=$log_file" >&2
    return 1
  fi

  echo "[$action_name] FAILED reason=missing_identity_startup_evidence path=$log_file" >&2
  tail -n 50 "$log_file" >&2 || true
  return 1
}

assert_dynamic_metadata_identity_logged() {
  assert_dynamic_metadata_identity_logged_slot 1
}

status_reports_dynamic_metadata_seen_slot() {
  local output_file="$1"
  local slot="$2"
  local expected_metadata_nodes="$3"
  local node_id
  local summary_line
  local node_line
  node_id="$(metadata_slot_node_id "$slot")"

  summary_line="$(grep -E '^status OK ' "$output_file" | tail -n1 || true)"
  node_line="$(grep -E '^metadata_node ' "$output_file" | grep -E "node_id=${node_id}([[:space:]]|$)" | tail -n1 || true)"

  [[ -n "$summary_line" ]] || return 1
  [[ "$summary_line" == *"metadata_nodes=${expected_metadata_nodes}"* ]] || return 1
  [[ -n "$node_line" ]] || return 1
  [[ "$node_line" == *"membership_observation=learner"* ]] || return 1
  if [[ ! "$node_line" =~ liveness=([Ll][Ii][Vv][Ee]) ]]; then
    return 1
  fi
  return 0
}

status_reports_dynamic_metadata_seen() {
  status_reports_dynamic_metadata_seen_slot "$1" 1 4
}

latest_dynamic_metadata_status_is_ready_or_pending_slot() {
  local slot="$1"
  local latest_join_line
  latest_join_line="$(latest_dynamic_metadata_join_status_slot "$slot" || true)"
  [[ -n "$latest_join_line" ]] || return 1
  [[ "$latest_join_line" == *"committed_voter_count=3"* ]] || return 1
  [[ "$latest_join_line" == *"committed_quorum_size=2"* ]] || return 1
  [[ "$latest_join_line" == *"learner_status=pending"* || "$latest_join_line" == *"learner_status=ready_to_promote"* ]] || return 1
  return 0
}

latest_dynamic_metadata_status_is_ready_or_pending() {
  latest_dynamic_metadata_status_is_ready_or_pending_slot 1
}

wait_for_dynamic_metadata_status_slot() {
  local slot="$1"
  local expected_metadata_nodes="$2"
  local action_name
  local node_id
  local output_file
  action_name="$(metadata_slot_action_name "$slot")"
  node_id="$(metadata_slot_node_id "$slot")"
  output_file="$(mktemp)"
  trap 'rm -f "$output_file"' RETURN

  local deadline=$((SECONDS + METADATA_JOIN_WAIT_SECONDS))
  while (( SECONDS < deadline )); do
    run_status_capture "$output_file"
    if status_reports_dynamic_metadata_seen_slot "$output_file" "$slot" "$expected_metadata_nodes" && \
      latest_dynamic_metadata_status_is_ready_or_pending_slot "$slot"; then
      echo "[$action_name] observed node_id=$node_id in cluster status as LIVE learner"
      latest_dynamic_metadata_join_status_slot "$slot" || true
      return 0
    fi
    sleep "$STATUS_POLL_INTERVAL_SECONDS"
  done

  echo "[$action_name] FAILED reason=dynamic_metadata_not_observed node_id=$node_id wait_seconds=$METADATA_JOIN_WAIT_SECONDS" >&2
  echo "[$action_name] last_status_output:" >&2
  tail -n 50 "$output_file" >&2 || true
  local log_file
  log_file="$(metadata_slot_log_file "$slot")"
  if [[ -f "$log_file" ]]; then
    echo "[$action_name] metadata_log_tail path=$log_file" >&2
    tail -n 80 "$log_file" >&2 || true
  fi
  return 1
}

wait_for_dynamic_metadata_status() {
  wait_for_dynamic_metadata_status_slot 1 4
}

wait_for_blocked_single_promote_slot() {
  local slot="$1"
  local node_id
  node_id="$(metadata_slot_node_id "$slot")"
  local deadline=$((SECONDS + METADATA_JOIN_WAIT_SECONDS))
  while (( SECONDS < deadline )); do
    local latest_join_line
    latest_join_line="$(latest_dynamic_metadata_join_status_slot "$slot" || true)"
    if [[ -n "$latest_join_line" ]] && \
      [[ "$latest_join_line" == *"learner_status=ready_to_promote"* ]] && \
      [[ "$latest_join_line" == *"promotion_status=waiting_for_pair"* ]] && \
      [[ "$latest_join_line" == *"promotion_block_reason=even_voter_count"* ]] && \
      [[ "$latest_join_line" == *"committed_voter_count=3"* ]] && \
      [[ "$latest_join_line" == *"committed_quorum_size=2"* ]]; then
      echo "[promote-metadata-learner] blocked promote observed node_id=$node_id"
      echo "$latest_join_line"
      return 0
    fi
    sleep "$STATUS_POLL_INTERVAL_SECONDS"
  done

  echo "[promote-metadata-learner] FAILED reason=blocked_promote_not_observed node_id=$node_id wait_seconds=$METADATA_JOIN_WAIT_SECONDS" >&2
  local log_file
  log_file="$(metadata_slot_log_file "$slot")"
  if [[ -f "$log_file" ]]; then
    echo "[promote-metadata-learner] metadata_log_tail path=$log_file" >&2
    tail -n 80 "$log_file" >&2 || true
  fi
  return 1
}

wait_for_blocked_single_promote() {
  wait_for_blocked_single_promote_slot 1
}

latest_join_or_promote_line_slot() {
  local slot="$1"
  local batch_line
  batch_line="$(latest_dynamic_metadata_batch_promote_status_slot "$slot" || true)"
  if [[ -n "$batch_line" ]]; then
    printf '%s\n' "$batch_line"
    return 0
  fi
  latest_dynamic_metadata_join_status_slot "$slot" || true
}

latest_line_reports_pre_promote_ready_slot() {
  local slot="$1"
  local latest_line
  latest_line="$(latest_join_or_promote_line_slot "$slot" || true)"
  [[ -n "$latest_line" ]] || return 1
  [[ "$latest_line" == *"committed_voter_count=3"* ]] || return 1
  [[ "$latest_line" == *"committed_quorum_size=2"* ]] || return 1
  [[ "$latest_line" == *"learner_status=ready_to_promote"* || "$latest_line" == *"learner_status=pending"* ]] || return 1
  [[ "$latest_line" != *"promotion_status=batch_promoted"* ]] || return 1
  return 0
}

wait_for_two_learners_ready_pre_promote() {
  local output_file
  output_file="$(mktemp)"
  trap 'rm -f "$output_file"' RETURN
  local deadline=$((SECONDS + METADATA_JOIN_WAIT_SECONDS))
  while (( SECONDS < deadline )); do
    local learner1_line
    local learner2_line
    learner1_line="$(latest_join_or_promote_line_slot 1 || true)"
    learner2_line="$(latest_join_or_promote_line_slot 2 || true)"

    run_status_capture "$output_file" >/dev/null
    if latest_line_reports_pre_promote_ready_slot 1 && \
      latest_line_reports_pre_promote_ready_slot 2 && \
      status_reports_dynamic_metadata_seen_slot "$output_file" 1 5 && \
      status_reports_dynamic_metadata_seen_slot "$output_file" 2 5; then
      echo "[promote-metadata-learners] observed two non-voter learners while committed voters remain 3 and quorum remains 2"
      echo "$learner1_line"
      echo "$learner2_line"
      trap - RETURN
      return 0
    fi

    local learner1_pre_line
    local learner2_pre_line
    local learner1_batch_line
    local learner2_batch_line
    learner1_pre_line="$(historical_dynamic_metadata_pre_promote_line_slot 1 || true)"
    learner2_pre_line="$(historical_dynamic_metadata_pre_promote_line_slot 2 || true)"
    learner1_batch_line="$(historical_dynamic_metadata_batch_promote_line_slot 1 || true)"
    learner2_batch_line="$(historical_dynamic_metadata_batch_promote_line_slot 2 || true)"

    if [[ -n "$learner1_pre_line" && -n "$learner2_pre_line" ]] && \
      [[ -n "$learner1_batch_line" || -n "$learner2_batch_line" ]] && \
      status_reports_batch_promoted "$output_file"; then
      echo "[promote-metadata-learners] batch promote already completed before explicit observe command; using learner log history"
      echo "$learner1_pre_line"
      echo "$learner2_pre_line"
      trap - RETURN
      return 0
    fi
    sleep "$STATUS_POLL_INTERVAL_SECONDS"
  done

  echo "[promote-metadata-learners] FAILED reason=two_ready_learners_not_observed wait_seconds=$METADATA_JOIN_WAIT_SECONDS" >&2
  trap - RETURN
  return 1
}

status_reports_batch_promoted() {
  local output_file="$1"
  local summary_line
  local meta4_line
  local meta5_line

  summary_line="$(grep -E '^status OK ' "$output_file" | tail -n1 || true)"
  meta4_line="$(grep -E '^metadata_node ' "$output_file" | grep -E "node_id=${DYNAMIC_METADATA_NODE_ID}([[:space:]]|$)" | tail -n1 || true)"
  meta5_line="$(grep -E '^metadata_node ' "$output_file" | grep -E "node_id=${DYNAMIC_METADATA_NODE_ID_2}([[:space:]]|$)" | tail -n1 || true)"

  [[ -n "$summary_line" ]] || return 1
  [[ "$summary_line" == *"metadata_nodes=5"* ]] || return 1
  [[ -n "$meta4_line" ]] || return 1
  [[ -n "$meta5_line" ]] || return 1
  [[ "$meta4_line" == *"membership_observation=voter"* ]] || return 1
  [[ "$meta5_line" == *"membership_observation=voter"* ]] || return 1
  return 0
}

wait_for_batch_promote_observed() {
  local output_file
  output_file="$(mktemp)"
  trap 'rm -f "$output_file"' RETURN

  local deadline=$((SECONDS + METADATA_JOIN_WAIT_SECONDS))
  while (( SECONDS < deadline )); do
    local learner1_promote_line
    local learner2_promote_line
    learner1_promote_line="$(historical_dynamic_metadata_batch_promote_line_slot 1 || true)"
    learner2_promote_line="$(historical_dynamic_metadata_batch_promote_line_slot 2 || true)"

    run_status_capture "$output_file" >/dev/null
    if status_reports_batch_promoted "$output_file"; then
      if [[ "$learner1_promote_line" == *"committed_voter_count=5"* && \
            "$learner1_promote_line" == *"committed_quorum_size=3"* && \
            "$learner1_promote_line" == *"promotion_status=batch_promoted"* ]]; then
        echo "[promote-metadata-learners] batch promote observed via learner-1 and cluster status"
        echo "$learner1_promote_line"
        trap - RETURN
        return 0
      fi
      if [[ "$learner2_promote_line" == *"committed_voter_count=5"* && \
            "$learner2_promote_line" == *"committed_quorum_size=3"* && \
            "$learner2_promote_line" == *"promotion_status=batch_promoted"* ]]; then
        echo "[promote-metadata-learners] batch promote observed via learner-2 and cluster status"
        echo "$learner2_promote_line"
        trap - RETURN
        return 0
      fi
      echo "[promote-metadata-learners] batch promote observed via cluster status; learner promote log may flush after shutdown"
      trap - RETURN
      return 0
    fi

    if dynamic_metadata_log_has_committed_voter_count_four_slot 1 || \
      dynamic_metadata_log_has_committed_voter_count_four_slot 2; then
      echo "[promote-metadata-learners] FAILED reason=committed_four_voter_history_detected" >&2
      trap - RETURN
      return 1
    fi
    if [[ "$learner1_promote_line" == *"committed_voter_count=5"* && \
          "$learner1_promote_line" == *"committed_quorum_size=3"* && \
          "$learner1_promote_line" == *"promotion_status=batch_promoted"* ]]; then
      echo "[promote-metadata-learners] batch promote observed via learner-1"
      echo "$learner1_promote_line"
      trap - RETURN
      return 0
    fi
    if [[ "$learner2_promote_line" == *"committed_voter_count=5"* && \
          "$learner2_promote_line" == *"committed_quorum_size=3"* && \
          "$learner2_promote_line" == *"promotion_status=batch_promoted"* ]]; then
      echo "[promote-metadata-learners] batch promote observed via learner-2"
      echo "$learner2_promote_line"
      trap - RETURN
      return 0
    fi
    sleep "$STATUS_POLL_INTERVAL_SECONDS"
  done

  echo "[promote-metadata-learners] FAILED reason=batch_promote_not_observed wait_seconds=$METADATA_JOIN_WAIT_SECONDS" >&2
  trap - RETURN
  return 1
}

count_dynamic_storage_payload_files() {
  if [[ ! -d "$SCRIPT_DIR/$DYNAMIC_STORAGE_DATA_DIR" ]]; then
    printf '0\n'
    return 0
  fi

  find "$SCRIPT_DIR/$DYNAMIC_STORAGE_DATA_DIR" -type f ! -name 'node.identity' | wc -l | awk '{print $1}'
}

run_upload_one() {
  local object_prefix="$1"
  local source_file="$2"
  local object_key
  local client_config
  client_config="$(active_client_config_path)"
  object_key="$(object_key_for_file "$object_prefix" "$source_file")"
  echo "[upload] file=$source_file object=$object_key"
  (
    cd "$SCRIPT_DIR"
    "$CLIENT_BIN" upload \
      --config "$client_config" \
      --bucket "$BUCKET" \
      --object "$object_key" \
      --file "$source_file" \
      --concurrency 3
  )
}

run_download_one() {
  local object_prefix="$1"
  local source_file="$2"
  local object_key
  local download_file
  local client_config
  client_config="$(active_client_config_path)"
  object_key="$(object_key_for_file "$object_prefix" "$source_file")"
  download_file="$(download_path_for_file "$object_prefix" "$source_file")"
  mkdir -p "$(dirname "$download_file")"
  rm -f "$download_file"
  echo "[download] object=$object_key out=$download_file"
  (
    cd "$SCRIPT_DIR"
    "$CLIENT_BIN" download \
      --config "$client_config" \
      --bucket "$BUCKET" \
      --object "$object_key" \
      --out "$download_file" \
      --concurrency 3
  )
}

verify_local_copy_one() {
  local object_prefix="$1"
  local source_file="$2"
  local download_file
  local source_size
  local download_size
  download_file="$(download_path_for_file "$object_prefix" "$source_file")"
  source_size="$(file_size_bytes "$source_file")"
  download_size="$(file_size_bytes "$download_file")"
  if [[ "$source_size" == "$download_size" ]]; then
    echo "[verify] OK source=$source_file downloaded=$download_file size_bytes=$download_size"
    return 0
  fi
  echo "[verify] FAILED source=$source_file downloaded=$download_file source_size=$source_size download_size=$download_size" >&2
  return 1
}

assert_parallel_upload_config_observed() {
  local expected_requested="requested_concurrency=$PARALLEL_UPLOAD_CONCURRENCY"
  local expected_effective="effective_concurrency=$PARALLEL_UPLOAD_CONCURRENCY"
  local expected_bytes="max_inflight_payload_bytes=$PARALLEL_MAX_INFLIGHT_BYTES"
  local expected_fanout="effective_replica_fanout_concurrency=$PARALLEL_REPLICA_FANOUT_CONCURRENCY"

  grep -F "$expected_requested" "$PARALLEL_UPLOAD_LOG" >/dev/null
  grep -F "$expected_effective" "$PARALLEL_UPLOAD_LOG" >/dev/null
  grep -F "$expected_bytes" "$PARALLEL_UPLOAD_LOG" >/dev/null
  grep -F "$expected_fanout" "$PARALLEL_UPLOAD_LOG" >/dev/null
}

run_parallel_roundtrip() {
  load_parallel_runtime_config
  mkdir -p "$LOG_DIR" "$DOWNLOAD_DIR"
  : > "$PARALLEL_CREATE_BUCKET_LOG"
  : > "$PARALLEL_STATUS_LOG"
  : > "$PARALLEL_UPLOAD_LOG"
  : > "$PARALLEL_DOWNLOAD_LOG"

  wait_for_live_cluster "$PARALLEL_REQUIRED_STABLE_LIVE_POLLS"
  ensure_bucket_exists
  select_parallel_test_file

  local object_prefix
  local source_file
  local object_key
  local download_file
  local client_config
  local upload_started_at
  local upload_finished_at
  local download_started_at
  local download_finished_at
  local upload_elapsed
  local download_elapsed
  local download_size

  object_prefix="$(roundtrip_object_prefix)"
  source_file="$PARALLEL_SOURCE_FILE"
  object_key="$(object_key_for_file "$object_prefix" "$source_file")"
  download_file="$(download_path_for_file "$object_prefix" "$source_file")"
  client_config="$(active_client_config_path)"

  echo "[parallel-roundtrip] source_file=$source_file size_bytes=$PARALLEL_SOURCE_FILE_SIZE_BYTES size_human=$(format_bytes "$PARALLEL_SOURCE_FILE_SIZE_BYTES")"

  mkdir -p "$(dirname "$download_file")"
  rm -f "$download_file"

  upload_started_at="$(date +%s)"
  (
    cd "$SCRIPT_DIR"
    "$CLIENT_BIN" upload \
      --config "$client_config" \
      --bucket "$BUCKET" \
      --object "$object_key" \
      --file "$source_file"
  ) 2>&1 | tee "$PARALLEL_UPLOAD_LOG"
  upload_finished_at="$(date +%s)"
  upload_elapsed=$((upload_finished_at - upload_started_at))

  assert_parallel_upload_config_observed
  wait_for_live_cluster "$PARALLEL_REQUIRED_STABLE_LIVE_POLLS"

  download_started_at="$(date +%s)"
  (
    cd "$SCRIPT_DIR"
    "$CLIENT_BIN" download \
      --config "$client_config" \
      --bucket "$BUCKET" \
      --object "$object_key" \
      --out "$download_file" \
      --concurrency "$PARALLEL_UPLOAD_CONCURRENCY"
  ) 2>&1 | tee "$PARALLEL_DOWNLOAD_LOG"
  download_finished_at="$(date +%s)"
  download_elapsed=$((download_finished_at - download_started_at))

  download_size="$(file_size_bytes "$download_file")"
  if [[ "$download_size" != "$PARALLEL_SOURCE_FILE_SIZE_BYTES" ]]; then
    echo "[parallel-roundtrip] FAILED size mismatch source_size=$PARALLEL_SOURCE_FILE_SIZE_BYTES download_size=$download_size" >&2
    return 1
  fi

  echo "[parallel-roundtrip] PASS"
  echo "[parallel-roundtrip] upload_concurrency=$PARALLEL_UPLOAD_CONCURRENCY"
  echo "[parallel-roundtrip] max_inflight_bytes=$PARALLEL_MAX_INFLIGHT_BYTES"
  echo "[parallel-roundtrip] replica_fanout_concurrency=$PARALLEL_REPLICA_FANOUT_CONCURRENCY"
  echo "[parallel-roundtrip] source_file=$source_file"
  echo "[parallel-roundtrip] source_size_bytes=$PARALLEL_SOURCE_FILE_SIZE_BYTES"
  echo "[parallel-roundtrip] source_size_human=$(format_bytes "$PARALLEL_SOURCE_FILE_SIZE_BYTES")"
  echo "[parallel-roundtrip] upload_seconds=$upload_elapsed"
  echo "[parallel-roundtrip] download_seconds=$download_elapsed"
  echo "[parallel-roundtrip] downloaded_size_bytes=$download_size"
  echo "[parallel-roundtrip] download_file=$download_file"
}

run_roundtrip() {
  ensure_bucket_exists
  collect_test_files
  local object_prefix
  object_prefix="$(roundtrip_object_prefix)"
  local payload_before
  local payload_after
  local status_output
  payload_before="$(count_dynamic_storage_payload_files)"
  status_output="$(mktemp)"
  trap 'rm -f "$status_output"' RETURN

  echo "[roundtrip] object_prefix=$object_prefix"
  run_status_capture "$status_output"
  if ! view_node_running "$FAILOVER_STOP_VIEW_NODE_ID" && \
    view_node_running "$FAILOVER_SURVIVOR_VIEW_NODE_ID"; then
    if ! status_reports_surviving_view_ready "$status_output"; then
      echo "[roundtrip] FAILED reason=surviving_view_not_ready endpoint=$FAILOVER_SURVIVOR_VIEW_ENDPOINT" >&2
      tail -n 50 "$status_output" >&2 || true
      trap - RETURN
      return 1
    fi
    echo "[roundtrip] confirmed surviving_view_endpoint=$FAILOVER_SURVIVOR_VIEW_ENDPOINT"
  fi

  local source_file
  for source_file in "${TEST_FILES[@]}"; do
    run_upload_one "$object_prefix" "$source_file"
  done
  for source_file in "${TEST_FILES[@]}"; do
    run_download_one "$object_prefix" "$source_file"
  done
  for source_file in "${TEST_FILES[@]}"; do
    verify_local_copy_one "$object_prefix" "$source_file"
  done

  payload_after="$(count_dynamic_storage_payload_files)"
  if (( payload_after > payload_before )); then
    echo "[placement] observed dynamic storage participation node_id=$DYNAMIC_STORAGE_NODE_ID payload_files_before=$payload_before payload_files_after=$payload_after"
  else
    echo "[placement] dynamic storage participation not directly observed node_id=$DYNAMIC_STORAGE_NODE_ID payload_files_before=$payload_before payload_files_after=$payload_after"
  fi
  trap - RETURN
}

run_failover_view() {
  local status_output
  status_output="$(mktemp)"
  trap 'rm -f "$status_output"' RETURN

  stop_view_node_for_failover

  local deadline=$((SECONDS + JOIN_WAIT_SECONDS))
  while (( SECONDS < deadline )); do
    run_status_capture "$status_output"
    if status_reports_surviving_view_ready "$status_output"; then
      echo "[failover-view] surviving view ready node_id=$FAILOVER_SURVIVOR_VIEW_NODE_ID endpoint=$FAILOVER_SURVIVOR_VIEW_ENDPOINT"
      trap - RETURN
      return 0
    fi
    sleep "$STATUS_POLL_INTERVAL_SECONDS"
  done

  echo "[failover-view] FAILED reason=surviving_view_status_unavailable node_id=$FAILOVER_SURVIVOR_VIEW_NODE_ID endpoint=$FAILOVER_SURVIVOR_VIEW_ENDPOINT wait_seconds=$JOIN_WAIT_SECONDS" >&2
  tail -n 50 "$status_output" >&2 || true
  local survivor_log
  survivor_log="$(view_log_file "$FAILOVER_SURVIVOR_VIEW_NODE_ID")"
  if [[ -f "$survivor_log" ]]; then
    echo "[failover-view] survivor_log_tail path=$survivor_log" >&2
    tail -n 50 "$survivor_log" >&2 || true
  fi
  trap - RETURN
  return 1
}

run_join_storage() {
  local joined=false
  trap 'if [[ "$joined" == "false" ]]; then cleanup_dynamic_storage_if_needed || true; fi' RETURN
  launch_dynamic_storage
  wait_for_dynamic_storage_live
  assert_dynamic_storage_identity_logged
  joined=true
  trap - RETURN
}

run_join_metadata_learner_slot() {
  local slot="$1"
  local expected_metadata_nodes="$2"
  local joined=false
  trap 'if [[ "$joined" == "false" ]]; then cleanup_dynamic_metadata_slot_if_needed '"$slot"' || true; fi' RETURN
  reset_dynamic_metadata_slot_state_if_stopped "$slot"
  launch_dynamic_metadata_learner_slot "$slot"
  assert_dynamic_metadata_identity_logged_slot "$slot"
  wait_for_dynamic_metadata_status_slot "$slot" "$expected_metadata_nodes"
  joined=true
  trap - RETURN
}

run_join_metadata_learner() {
  run_join_metadata_learner_slot 1 4
}

run_join_metadata_learner_2() {
  run_join_metadata_learner_slot 2 5
}

run_promote_metadata_learner() {
  wait_for_blocked_single_promote
}

run_promote_metadata_learners() {
  wait_for_two_learners_ready_pre_promote
  wait_for_batch_promote_observed
}

main() {
  require_client
  require_test_files_dir
  ensure_generated_cluster_config
  case "$ACTION" in
    status)
      run_status
      ;;
    join-storage)
      run_join_storage
      ;;
    join-metadata-learner)
      run_join_metadata_learner
      ;;
    join-metadata-learner-2)
      run_join_metadata_learner_2
      ;;
    promote-metadata-learner)
      run_promote_metadata_learner
      ;;
    promote-metadata-learners)
      run_promote_metadata_learners
      ;;
    failover-view)
      run_failover_view
      ;;
    upload)
      ensure_bucket_exists
      collect_test_files
      local object_prefix
      local client_config
      client_config="$(active_client_config_path)"
      object_prefix="$(roundtrip_object_prefix)"
      local source_file
      for source_file in "${TEST_FILES[@]}"; do
        (
          cd "$SCRIPT_DIR"
          "$CLIENT_BIN" upload \
            --config "$client_config" \
            --bucket "$BUCKET" \
            --object "$(object_key_for_file "$object_prefix" "$source_file")" \
            --file "$source_file"
        )
      done
      ;;
    parallel-roundtrip)
      run_parallel_roundtrip
      ;;
    download)
      echo "download action is not supported standalone in sibling 009 script; use roundtrip" >&2
      exit 1
      ;;
    roundtrip)
      run_roundtrip
      ;;
    *)
      echo "unsupported action: $ACTION (expected: status|join-storage|join-metadata-learner|join-metadata-learner-2|promote-metadata-learner|promote-metadata-learners|failover-view|upload|parallel-roundtrip|roundtrip)" >&2
      exit 1
      ;;
  esac
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  main "$@"
fi
