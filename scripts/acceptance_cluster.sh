#!/usr/bin/env bash
set -Eeuo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
RAFT_DEMO_BIN="${RAFT_DEMO_BIN:-${BUILD_DIR}/raft_demo}"
RAFT_KV_CLIENT_BIN="${RAFT_KV_CLIENT_BIN:-${BUILD_DIR}/raft_kv_client}"
RUN_ROOT="${PROJECT_ROOT}/build/tests/raft_test_data/deploy_acceptance/run_$(date +%Y%m%d_%H%M%S)"
CONFIG_FILE="${RUN_ROOT}/config.txt"
LOG_DIR="${RUN_ROOT}/logs"
DATA_ROOT="${RUN_ROOT}/raft_data"
SNAPSHOT_ROOT="${RUN_ROOT}/raft_snapshots"

declare -A ADDRS
declare -A PIDS

ADDRS[1]="127.0.0.1:56061"
ADDRS[2]="127.0.0.1:56062"
ADDRS[3]="127.0.0.1:56063"

cleanup() {
  for id in 1 2 3; do
    if [[ -n "${PIDS[$id]:-}" ]] && kill -0 "${PIDS[$id]}" 2>/dev/null; then
      kill "${PIDS[$id]}" 2>/dev/null || true
      wait "${PIDS[$id]}" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

require_bin() {
  if [[ ! -x "$1" ]]; then
    echo "missing executable: $1" >&2
    exit 1
  fi
}

start_node() {
  local id="$1"
  mkdir -p "${LOG_DIR}"
  "${RAFT_DEMO_BIN}" "${CONFIG_FILE}" "${id}" >"${LOG_DIR}/node_${id}.log" 2>&1 &
  PIDS[$id]=$!
  echo "started node ${id} pid=${PIDS[$id]}"
}

stop_node() {
  local id="$1"
  if [[ -z "${PIDS[$id]:-}" ]]; then
    return
  fi
  if kill -0 "${PIDS[$id]}" 2>/dev/null; then
    kill "${PIDS[$id]}"
    wait "${PIDS[$id]}" || true
  fi
  unset PIDS[$id]
  echo "stopped node ${id}"
}

extract_field() {
  local text="$1"
  local field="$2"
  echo "${text}" | sed -n "s/.*${field}=\\([^ ]*\\).*/\\1/p" | head -n1
}

status_line() {
  local id="$1"
  "${RAFT_KV_CLIENT_BIN}" "${ADDRS[$id]}" status 2>/dev/null | head -n1 || true
}

status_dump() {
  local id="$1"
  "${RAFT_KV_CLIENT_BIN}" "${ADDRS[$id]}" status 2>/dev/null || true
}

wait_for_leader() {
  local timeout_seconds="$1"
  local deadline=$((SECONDS + timeout_seconds))
  while (( SECONDS < deadline )); do
    for id in 1 2 3; do
      if [[ -z "${PIDS[$id]:-}" ]]; then
        continue
      fi
      local line
      line="$(status_line "$id")"
      if [[ -n "$line" ]] && [[ "$(extract_field "$line" "role")" == "Leader" ]]; then
        echo "$id"
        return 0
      fi
    done
    sleep 0.2
  done
  return 1
}

wait_for_peer_match_index() {
  local leader_id="$1"
  local peer_id="$2"
  local expected_match_index="$3"
  local timeout_seconds="$4"
  local deadline=$((SECONDS + timeout_seconds))
  while (( SECONDS < deadline )); do
    local dump
    dump="$(status_dump "${leader_id}")"
    local peer_line
    peer_line="$(grep "^peer_id=${peer_id} " <<<"${dump}" || true)"
    if [[ -n "${peer_line}" ]]; then
      local match_index
      match_index="$(extract_field "${peer_line}" "match_index")"
      if [[ -n "${match_index}" ]] && (( match_index >= expected_match_index )); then
        return 0
      fi
    fi
    sleep 0.2
  done
  return 1
}

write_config() {
  mkdir -p "${RUN_ROOT}"
  cat >"${CONFIG_FILE}" <<EOF
node.1=${ADDRS[1]}
node.2=${ADDRS[2]}
node.3=${ADDRS[3]}
data_root=${DATA_ROOT}
snapshot_root=${SNAPSHOT_ROOT}
snapshot_enabled=true
snapshot_log_threshold=4
heartbeat_interval_ms=100
election_timeout_min_ms=400
election_timeout_max_ms=700
rpc_deadline_ms=300
log_level=info
EOF
}

run_put() {
  local addr="$1"
  local key="$2"
  local value="$3"
  "${RAFT_KV_CLIENT_BIN}" "$addr" put "$key" "$value"
}

run_get_expect_value() {
  local addr="$1"
  local key="$2"
  local expected="$3"
  local output
  output="$("${RAFT_KV_CLIENT_BIN}" "$addr" get "$key")"
  echo "$output"
  if ! grep -q "value=${expected}" <<<"$output"; then
    echo "unexpected get result for ${key}: ${output}" >&2
    exit 1
  fi
}

require_bin "${RAFT_DEMO_BIN}"
require_bin "${RAFT_KV_CLIENT_BIN}"
write_config

echo "acceptance run root: ${RUN_ROOT}"

start_node 1
start_node 2
start_node 3

LEADER_ID="$(wait_for_leader 15)" || {
  echo "failed to elect initial leader" >&2
  exit 1
}
echo "initial leader=${LEADER_ID}"

RESTART_FOLLOWER_ID=1
if [[ "${RESTART_FOLLOWER_ID}" == "${LEADER_ID}" ]]; then
  RESTART_FOLLOWER_ID=2
fi
if [[ "${RESTART_FOLLOWER_ID}" == "${LEADER_ID}" ]]; then
  RESTART_FOLLOWER_ID=3
fi

stop_node "${RESTART_FOLLOWER_ID}"
sleep 1

PUT_OUTPUT="$(run_put "${ADDRS[$LEADER_ID]}" "accept-key" "accept-ok")"
echo "${PUT_OUTPUT}"
PUT_LOG_INDEX="$(extract_field "${PUT_OUTPUT}" "log_index")"
if [[ -z "${PUT_LOG_INDEX}" ]]; then
  echo "failed to extract log_index from put output: ${PUT_OUTPUT}" >&2
  exit 1
fi
run_get_expect_value "${ADDRS[$LEADER_ID]}" "accept-key" "accept-ok"

start_node "${RESTART_FOLLOWER_ID}"
wait_for_peer_match_index "${LEADER_ID}" "${RESTART_FOLLOWER_ID}" "${PUT_LOG_INDEX}" 20 || {
  echo "restarted follower ${RESTART_FOLLOWER_ID} did not catch up to log index ${PUT_LOG_INDEX}" >&2
  exit 1
}

stop_node "${LEADER_ID}"
sleep 1

NEW_LEADER_ID="$(wait_for_leader 20)" || {
  echo "failed to elect leader after restarting follower" >&2
  exit 1
}
echo "new leader=${NEW_LEADER_ID}"

run_get_expect_value "${ADDRS[$NEW_LEADER_ID]}" "accept-key" "accept-ok"
run_put "${ADDRS[$NEW_LEADER_ID]}" "accept-key-2" "after-failover"
run_get_expect_value "${ADDRS[$NEW_LEADER_ID]}" "accept-key-2" "after-failover"

echo "acceptance script passed"
