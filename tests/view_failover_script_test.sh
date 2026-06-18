#!/usr/bin/env bash
set -euo pipefail

RPC_DEMO_SCRIPT="${1:?missing rpc_demo.sh path}"

# shellcheck source=/dev/null
source "$RPC_DEMO_SCRIPT"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

write_status_fixture() {
  local output_file="$1"
  local survivor_health="$2"
  cat > "$output_file" <<EOF
status OK request_id=test-status cluster_id=example-local-009-dynamic target_endpoint=127.0.0.1:8302 observed_at_unix_ms=1781674528123 view_nodes=2 metadata_nodes=5 storage_nodes=7
leader_hint leader_hint.node_id=meta-1 leader_hint.raft_id=1 leader_hint.endpoint=127.0.0.1:8401 leader_hint.term=8 leader_hint.observed_at_unix_ms=1781674527705
non_authority_boundary membership_observation_source=view_node raft_membership_authority=false object_manifest_authority=false
view_node node_id=view-1 node_type=view endpoint=127.0.0.1:8301 liveness=dead health=healthy disk_pressure=low last_seen_unix_ms=1781674498966 last_sequence=174 control_plane_endpoint=127.0.0.1:8301
view_node node_id=view-2 node_type=view endpoint=127.0.0.1:8302 liveness=live health=${survivor_health} disk_pressure=low last_seen_unix_ms=1781674527299 last_sequence=202 control_plane_endpoint=127.0.0.1:8302
metadata_node node_id=meta-1 endpoint=127.0.0.1:8401 liveness=live health=healthy disk_pressure=low last_seen_unix_ms=1781674527664 last_sequence=201 raft_id=1 raft_role=leader membership_observation=voter observed_term=8 commit_index=40 membership_epoch=1
metadata_node node_id=meta-2 endpoint=127.0.0.1:8402 liveness=live health=healthy disk_pressure=low last_seen_unix_ms=1781674527441 last_sequence=200 raft_id=2 raft_role=follower membership_observation=voter observed_term=8 commit_index=40 membership_epoch=1
metadata_node node_id=meta-3 endpoint=127.0.0.1:8403 liveness=live health=healthy disk_pressure=low last_seen_unix_ms=1781674527696 last_sequence=200 raft_id=3 raft_role=follower membership_observation=voter observed_term=8 commit_index=40 membership_epoch=1
metadata_node node_id=meta-4 endpoint=127.0.0.1:8404 liveness=live health=healthy disk_pressure=low last_seen_unix_ms=1781674527175 last_sequence=36 raft_id=4 raft_role=follower membership_observation=voter observed_term=8 commit_index=40 membership_epoch=1
metadata_node node_id=meta-5 endpoint=127.0.0.1:8405 liveness=live health=healthy disk_pressure=low last_seen_unix_ms=1781674527705 last_sequence=32 raft_id=5 raft_role=follower membership_observation=voter observed_term=8 commit_index=40 membership_epoch=1
storage_node node_id=store-1 endpoint=127.0.0.1:8501 liveness=live health=healthy disk_pressure=low total_capacity_bytes=10737418240 used_capacity_bytes=0 available_capacity_bytes=10737418240 chunk_count=0 active_reads=0 active_writes=0 queued_ops=0 write_admission_overloaded=false read_admission_overloaded=false last_seen_unix_ms=1781674528112 last_sequence=162 zone=zone-a rack=rack-a1
storage_node node_id=store-2 endpoint=127.0.0.1:8502 liveness=live health=healthy disk_pressure=low total_capacity_bytes=10737418240 used_capacity_bytes=0 available_capacity_bytes=10737418240 chunk_count=0 active_reads=0 active_writes=0 queued_ops=0 write_admission_overloaded=false read_admission_overloaded=false last_seen_unix_ms=1781674527374 last_sequence=161 zone=zone-a rack=rack-a2
storage_node node_id=store-3 endpoint=127.0.0.1:8503 liveness=live health=healthy disk_pressure=low total_capacity_bytes=10737418240 used_capacity_bytes=0 available_capacity_bytes=10737418240 chunk_count=0 active_reads=0 active_writes=0 queued_ops=0 write_admission_overloaded=false read_admission_overloaded=false last_seen_unix_ms=1781674527751 last_sequence=161 zone=zone-b rack=rack-b1
storage_node node_id=store-4 endpoint=127.0.0.1:8504 liveness=live health=healthy disk_pressure=low total_capacity_bytes=10737418240 used_capacity_bytes=0 available_capacity_bytes=10737418240 chunk_count=0 active_reads=0 active_writes=0 queued_ops=0 write_admission_overloaded=false read_admission_overloaded=false last_seen_unix_ms=1781674527803 last_sequence=192 zone=zone-b rack=rack-b2
storage_node node_id=store-5 endpoint=127.0.0.1:8505 liveness=live health=healthy disk_pressure=low total_capacity_bytes=10737418240 used_capacity_bytes=0 available_capacity_bytes=10737418240 chunk_count=0 active_reads=0 active_writes=0 queued_ops=0 write_admission_overloaded=false read_admission_overloaded=false last_seen_unix_ms=1781674528112 last_sequence=192 zone=zone-c rack=rack-c1
storage_node node_id=store-6 endpoint=127.0.0.1:8506 liveness=live health=healthy disk_pressure=low total_capacity_bytes=10737418240 used_capacity_bytes=0 available_capacity_bytes=10737418240 chunk_count=0 active_reads=0 active_writes=0 queued_ops=0 write_admission_overloaded=false read_admission_overloaded=false last_seen_unix_ms=1781674527397 last_sequence=191 zone=zone-c rack=rack-c2
storage_node node_id=store-7 endpoint=127.0.0.1:8507 liveness=live health=healthy disk_pressure=low total_capacity_bytes=10737418240 used_capacity_bytes=0 available_capacity_bytes=10737418240 chunk_count=0 active_reads=0 active_writes=0 queued_ops=0 write_admission_overloaded=false read_admission_overloaded=false last_seen_unix_ms=1781674527953 last_sequence=109 zone=zone-d rack=rack-d1
EOF
}

write_partial_storage_fixture() {
  local output_file="$1"
  cat > "$output_file" <<EOF
status OK request_id=test-status cluster_id=example-local-009-dynamic target_endpoint=127.0.0.1:8302 observed_at_unix_ms=1781674528123 view_nodes=2 metadata_nodes=3 storage_nodes=0
leader_hint leader_hint.node_id=meta-1 leader_hint.raft_id=1 leader_hint.endpoint=127.0.0.1:8401 leader_hint.term=8 leader_hint.observed_at_unix_ms=1781674527705
non_authority_boundary membership_observation_source=view_node raft_membership_authority=false object_manifest_authority=false
view_node node_id=view-1 node_type=view endpoint=127.0.0.1:8301 liveness=dead health=healthy disk_pressure=low last_seen_unix_ms=1781674498966 last_sequence=174 control_plane_endpoint=127.0.0.1:8301
view_node node_id=view-2 node_type=view endpoint=127.0.0.1:8302 liveness=live health=healthy disk_pressure=low last_seen_unix_ms=1781674527299 last_sequence=202 control_plane_endpoint=127.0.0.1:8302
metadata_node node_id=meta-1 endpoint=127.0.0.1:8401 liveness=live health=healthy disk_pressure=low last_seen_unix_ms=1781674527664 last_sequence=201 raft_id=1 raft_role=leader membership_observation=voter observed_term=8 commit_index=40 membership_epoch=1
metadata_node node_id=meta-2 endpoint=127.0.0.1:8402 liveness=live health=healthy disk_pressure=low last_seen_unix_ms=1781674527441 last_sequence=200 raft_id=2 raft_role=follower membership_observation=voter observed_term=8 commit_index=40 membership_epoch=1
metadata_node node_id=meta-3 endpoint=127.0.0.1:8403 liveness=live health=healthy disk_pressure=low last_seen_unix_ms=1781674527696 last_sequence=200 raft_id=3 raft_role=follower membership_observation=voter observed_term=8 commit_index=40 membership_epoch=1
EOF
}

expanded_degraded="$TMP_DIR/expanded-degraded.log"
expanded_partial_storage="$TMP_DIR/expanded-partial-storage.log"
expanded_unavailable="$TMP_DIR/expanded-unavailable.log"

write_status_fixture "$expanded_degraded" "degraded"
write_partial_storage_fixture "$expanded_partial_storage"
write_status_fixture "$expanded_unavailable" "unavailable"

status_reports_surviving_view_ready "$expanded_degraded"
status_reports_surviving_view_ready "$expanded_partial_storage"

if status_reports_surviving_view_ready "$expanded_unavailable"; then
  echo "expected unavailable survivor status to fail validation" >&2
  exit 1
fi
