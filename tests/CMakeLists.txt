include(FetchContent)
include(GoogleTest)

if (MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)

if (MSVC)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
endif()

FetchContent_MakeAvailable(googletest)

# CTest label contract for industrialization validation:
# - platform-neutral:
#   Cross-platform baseline regression. These tests can participate in the
#   generic CTest fallback interpretation on Linux/Windows/macOS.
# - platform-neutral-fallback:
#   Conservative fallback subset intended for Windows / non-Bash one-command
#   validation. This is intentionally narrower than the full
#   platform-neutral semantic bucket.
# - durability-boundary:
#   Restart / trusted-state / crash-style durability boundary semantics.
#   Being runnable through CTest does not automatically make them equivalent to
#   Linux-specific runtime proof.
# - linux-specific-failure-injection:
#   Exact fsync / directory sync / replace / prune / partial-write failure
#   injection coverage. Runtime evidence is currently claimed on Linux only.
# - linux-primary-diagnosis:
#   Linux-primary diagnosis / retained-artifact / stress bucket. These tests
#   may still exercise platform-neutral logic, but their operational
#   interpretation is intentionally different from a pure cross-platform
#   baseline rerun.
# - storage-node:
#   StorageNode / store data-plane baseline bucket. For 007 and later store
#   tests, this is the primary feature grouping label.
# - storage-node-concurrency:
#   StorageNode concurrency / backpressure / bounded-queue tests.
# - storage-node-recovery:
#   StorageNode restart / rebuild / staging cleanup / corruption recovery
#   tests.
# - storage-node-cross-platform:
#   StorageNode cross-platform durability and path-behavior contract tests.
#
# This file does not rename targets or alter test behavior. It only attaches
# labeling metadata so ctest discovery and related docs can distinguish the
# validation scope more clearly.
set(RAFT_STORE_LABELS_BASELINE
  "storage-node;platform-neutral")
set(RAFT_STORE_LABELS_BASELINE_LINUX_PRIMARY
  "storage-node;linux-primary-diagnosis")
set(RAFT_STORE_LABELS_CONCURRENCY
  "storage-node;storage-node-concurrency;platform-neutral")
set(RAFT_STORE_LABELS_CONCURRENCY_LINUX_PRIMARY
  "storage-node;storage-node-concurrency;linux-primary-diagnosis")
set(RAFT_STORE_LABELS_RECOVERY
  "storage-node;storage-node-recovery;platform-neutral")
set(RAFT_STORE_LABELS_CROSS_PLATFORM
  "storage-node;storage-node-cross-platform;platform-neutral")

function(add_store_ctest test_name target_name test_labels)
  add_test(NAME ${test_name}
    COMMAND ${target_name}
  )
  set_tests_properties(${test_name} PROPERTIES
    LABELS "${test_labels}")
endfunction()

function(add_raft_gtest target_name source_file test_labels)
  if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}")
    message(STATUS "Skip ${target_name}: ${source_file} not found")
    return()
  endif()

  add_executable(${target_name} ${source_file})

  target_link_libraries(${target_name}
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(${target_name}
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  # DISCOVERY_MODE PRE_TEST 可以避免 build 阶段直接运行测试程序。
  # 这样即使某些运行环境变量还没准备好，也不会导致 cmake --build 失败。
  gtest_discover_tests(${target_name}
    DISCOVERY_TIMEOUT 60
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
      LABELS "${test_labels}"
  )
endfunction()

function(add_raft_gtest_sources target_name test_labels)
  set(source_files ${ARGN})
  foreach(source_file IN LISTS source_files)
    if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}")
      message(STATUS "Skip ${target_name}: ${source_file} not found")
      return()
    endif()
  endforeach()

  add_executable(${target_name} ${source_files})

  target_link_libraries(${target_name}
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(${target_name}
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  gtest_discover_tests(${target_name}
    DISCOVERY_TIMEOUT 60
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
      LABELS "${test_labels}"
  )
endfunction()

set(NO_KV_SURFACE_AUDIT_SCRIPT
  "${CMAKE_CURRENT_SOURCE_DIR}/no_kv_surface_audit.cmake")
if (EXISTS "${NO_KV_SURFACE_AUDIT_SCRIPT}")
  add_custom_target(no_kv_surface_audit
    COMMAND ${CMAKE_COMMAND}
      -DRAFT_SOURCE_DIR=${CMAKE_SOURCE_DIR}
      -P "${NO_KV_SURFACE_AUDIT_SCRIPT}"
    COMMENT "Running no-KV surface audit"
    VERBATIM
  )

  add_test(NAME NoKvSurfaceAudit
    COMMAND ${CMAKE_COMMAND}
      -DRAFT_SOURCE_DIR=${CMAKE_SOURCE_DIR}
      -P "${NO_KV_SURFACE_AUDIT_SCRIPT}"
  )
  set_tests_properties(NoKvSurfaceAudit PROPERTIES
    LABELS "platform-neutral;platform-neutral-fallback;no-kv-audit")
else()
  message(STATUS "Skip no_kv_surface_audit: no_kv_surface_audit.cmake not found")
endif()

# =========================
# Platform-neutral baseline:
# basic unit tests
# =========================
add_raft_gtest(test_command test_command.cpp
  "platform-neutral;platform-neutral-fallback")
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/metadata_command_test.cpp")
  add_executable(test_metadata_command
    metadata_command_test.cpp
  )

  target_link_libraries(test_metadata_command
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_metadata_command
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  gtest_discover_tests(test_metadata_command
    DISCOVERY_TIMEOUT 60
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
      LABELS "platform-neutral;platform-neutral-fallback"
  )
else()
  message(STATUS "Skip test_metadata_command: metadata_command_test.cpp not found")
endif()
add_raft_gtest(test_metadata_records metadata_records_test.cpp
  "platform-neutral;platform-neutral-fallback")
add_raft_gtest(test_metadata_command_types metadata_command_types_test.cpp
  "platform-neutral;platform-neutral-fallback")
add_raft_gtest(test_metadata_manifest metadata_manifest_test.cpp
  "platform-neutral;platform-neutral-fallback")
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/metadata_state_machine_test.cpp")
  add_raft_gtest_sources(test_metadata_state_machine
    "platform-neutral;platform-neutral-fallback"
    metadata_state_machine_test.cpp
    metadata_state_machine_query_test.cpp
    metadata_state_machine_snapshot_test.cpp
    metadata_state_machine_concurrency_test.cpp
    metadata_state_machine_legacy_test.cpp
  )
else()
  message(STATUS "Skip test_metadata_state_machine: metadata_state_machine_test.cpp not found")
endif()
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/metadata_snapshot_test.cpp")
  add_executable(test_metadata_snapshot
    metadata_snapshot_test.cpp
  )

  target_link_libraries(test_metadata_snapshot
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_metadata_snapshot
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  gtest_discover_tests(test_metadata_snapshot
    DISCOVERY_TIMEOUT 60
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
      LABELS "platform-neutral;durability-boundary"
  )
else()
  message(STATUS "Skip test_metadata_snapshot: metadata_snapshot_test.cpp not found")
endif()
add_raft_gtest(test_metadata_failover metadata_failover_test.cpp
  "platform-neutral")
target_link_libraries(test_metadata_failover
  PRIVATE
    metadata_proto)
add_raft_gtest(test_metadata_concurrency_stress metadata_concurrency_stress_test.cpp
  "platform-neutral")
add_raft_gtest(test_metadata_recovery_stress metadata_recovery_stress_test.cpp
  "platform-neutral;durability-boundary;linux-primary-diagnosis")
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/metadata_client_scenario_test.cpp")
  add_executable(test_metadata_client_scenario
    metadata_client_scenario_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/raft_metadata_client.cpp
  )

  set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/raft_metadata_client.cpp
    PROPERTIES
      COMPILE_DEFINITIONS "main=raft_metadata_client_entry"
  )

  add_dependencies(test_metadata_client_scenario
    raft_metadata_client
  )

  target_link_libraries(test_metadata_client_scenario
    PRIVATE
      metadata_proto
      GTest::gtest_main
  )

  target_include_directories(test_metadata_client_scenario
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  target_compile_definitions(test_metadata_client_scenario
    PRIVATE
      RAFT_METADATA_CLIENT_PATH="$<TARGET_FILE:raft_metadata_client>"
  )

  gtest_discover_tests(test_metadata_client_scenario
    DISCOVERY_TIMEOUT 60
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
      LABELS "platform-neutral"
  )
else()
  message(STATUS "Skip test_metadata_client_scenario: metadata_client_scenario_test.cpp not found")
endif()

add_raft_gtest(test_min_heap_timer test_min_heap_timer.cpp
  "platform-neutral;platform-neutral-fallback")
add_raft_gtest(test_thread_pool test_thread_pool.cpp
  "platform-neutral;platform-neutral-fallback")

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/store_types_test.cpp")
  add_executable(test_store_types
    store_types_test.cpp
  )

  target_link_libraries(test_store_types
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_store_types
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(store_types test_store_types
    "${RAFT_STORE_LABELS_BASELINE}")
else()
  message(STATUS "Skip test_store_types: store_types_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/store_durable_file_test.cpp")
  add_executable(test_store_durable_file
    store_durable_file_test.cpp
  )

  target_link_libraries(test_store_durable_file
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_store_durable_file
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(store_durable_file test_store_durable_file
    "${RAFT_STORE_LABELS_CROSS_PLATFORM}")
else()
  message(STATUS "Skip test_store_durable_file: store_durable_file_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/store_chunk_index_test.cpp")
  add_executable(test_store_chunk_index
    store_chunk_index_test.cpp
  )

  target_link_libraries(test_store_chunk_index
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_store_chunk_index
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(store_chunk_index test_store_chunk_index
    "${RAFT_STORE_LABELS_BASELINE}")
else()
  message(STATUS "Skip test_store_chunk_index: store_chunk_index_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/store_executor_test.cpp")
  add_executable(test_store_executor
    store_executor_test.cpp
  )

  target_link_libraries(test_store_executor
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_store_executor
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(store_executor test_store_executor
    "${RAFT_STORE_LABELS_BASELINE}")
else()
  message(STATUS "Skip test_store_executor: store_executor_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/local_disk_chunk_store_test.cpp")
  add_executable(test_local_disk_chunk_store
    local_disk_chunk_store_test.cpp
  )

  target_link_libraries(test_local_disk_chunk_store
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_local_disk_chunk_store
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(local_disk_chunk_store test_local_disk_chunk_store
    "${RAFT_STORE_LABELS_BASELINE}")
else()
  message(STATUS "Skip test_local_disk_chunk_store: local_disk_chunk_store_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/storage_upload_integration_test.cpp")
  add_executable(test_storage_upload_integration
    storage_upload_integration_test.cpp
  )

  target_link_libraries(test_storage_upload_integration
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_storage_upload_integration
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(storage_upload_integration test_storage_upload_integration
    "${RAFT_STORE_LABELS_BASELINE_LINUX_PRIMARY}")
else()
  message(STATUS "Skip test_storage_upload_integration: storage_upload_integration_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/storage_read_integration_test.cpp")
  add_executable(test_storage_read_integration
    storage_read_integration_test.cpp
  )

  target_link_libraries(test_storage_read_integration
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_storage_read_integration
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(storage_read_integration test_storage_read_integration
    "${RAFT_STORE_LABELS_BASELINE_LINUX_PRIMARY}")
else()
  message(STATUS "Skip test_storage_read_integration: storage_read_integration_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/storage_delete_gc_test.cpp")
  add_executable(test_storage_delete_gc
    storage_delete_gc_test.cpp
  )

  target_link_libraries(test_storage_delete_gc
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_storage_delete_gc
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(storage_delete_gc test_storage_delete_gc
    "${RAFT_STORE_LABELS_BASELINE_LINUX_PRIMARY}")
else()
  message(STATUS "Skip test_storage_delete_gc: storage_delete_gc_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/storage_delete_chunk_contract_test.cpp")
  add_executable(test_storage_delete_chunk_contract
    storage_delete_chunk_contract_test.cpp
  )

  target_link_libraries(test_storage_delete_chunk_contract
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_storage_delete_chunk_contract
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(storage_delete_chunk_contract test_storage_delete_chunk_contract
    "${RAFT_STORE_LABELS_BASELINE_LINUX_PRIMARY}")
else()
  message(STATUS "Skip test_storage_delete_chunk_contract: storage_delete_chunk_contract_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/storage_garbage_collector_test.cpp")
  add_executable(test_storage_garbage_collector
    storage_garbage_collector_test.cpp
  )

  target_link_libraries(test_storage_garbage_collector
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_storage_garbage_collector
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(storage_garbage_collector test_storage_garbage_collector
    "${RAFT_STORE_LABELS_BASELINE}")
else()
  message(STATUS "Skip test_storage_garbage_collector: storage_garbage_collector_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/storage_write_chunk_contract_test.cpp")
  add_executable(test_storage_write_chunk_contract
    storage_write_chunk_contract_test.cpp
  )

  target_link_libraries(test_storage_write_chunk_contract
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_storage_write_chunk_contract
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(storage_write_chunk_contract test_storage_write_chunk_contract
    "${RAFT_STORE_LABELS_BASELINE_LINUX_PRIMARY}")
else()
  message(STATUS "Skip test_storage_write_chunk_contract: storage_write_chunk_contract_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/storage_read_chunk_contract_test.cpp")
  add_executable(test_storage_read_chunk_contract
    storage_read_chunk_contract_test.cpp
  )

  target_link_libraries(test_storage_read_chunk_contract
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_storage_read_chunk_contract
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(storage_read_chunk_contract test_storage_read_chunk_contract
    "${RAFT_STORE_LABELS_BASELINE_LINUX_PRIMARY}")
else()
  message(STATUS "Skip test_storage_read_chunk_contract: storage_read_chunk_contract_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/storage_node_service_test.cpp")
  add_executable(test_storage_node_service
    storage_node_service_test.cpp
  )

  target_link_libraries(test_storage_node_service
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_storage_node_service
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(storage_node_service test_storage_node_service
    "${RAFT_STORE_LABELS_BASELINE}")
else()
  message(STATUS "Skip test_storage_node_service: storage_node_service_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/storage_node_client_test.cpp")
  add_executable(test_storage_node_client
    storage_node_client_test.cpp
  )

  target_link_libraries(test_storage_node_client
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_storage_node_client
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(storage_node_client test_storage_node_client
    "${RAFT_STORE_LABELS_BASELINE}")
else()
  message(STATUS "Skip test_storage_node_client: storage_node_client_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/store_placement_policy_test.cpp")
  add_executable(test_store_placement_policy
    store_placement_policy_test.cpp
  )

  target_link_libraries(test_store_placement_policy
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_store_placement_policy
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(store_placement_policy test_store_placement_policy
    "${RAFT_STORE_LABELS_BASELINE}")
else()
  message(STATUS "Skip test_store_placement_policy: store_placement_policy_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/store_placement_manager_test.cpp")
  add_executable(test_store_placement_manager
    store_placement_manager_test.cpp
  )

  target_link_libraries(test_store_placement_manager
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_store_placement_manager
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(store_placement_manager test_store_placement_manager
    "${RAFT_STORE_LABELS_BASELINE}")
else()
  message(STATUS "Skip test_store_placement_manager: store_placement_manager_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/storage_heartbeat_registry_test.cpp")
  add_executable(test_storage_heartbeat_registry
    storage_heartbeat_registry_test.cpp
  )

  target_link_libraries(test_storage_heartbeat_registry
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_storage_heartbeat_registry
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(storage_heartbeat_registry test_storage_heartbeat_registry
    "${RAFT_STORE_LABELS_BASELINE}")
else()
  message(STATUS "Skip test_storage_heartbeat_registry: storage_heartbeat_registry_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/store_concurrency_stress_test.cpp")
  add_executable(test_store_concurrency_stress
    store_concurrency_stress_test.cpp
  )

  target_link_libraries(test_store_concurrency_stress
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_store_concurrency_stress
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(store_concurrency_stress test_store_concurrency_stress
    "${RAFT_STORE_LABELS_CONCURRENCY_LINUX_PRIMARY}")
else()
  message(STATUS "Skip test_store_concurrency_stress: store_concurrency_stress_test.cpp not found")
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/storage_upload_coordinator_test.cpp")
  add_executable(test_storage_upload_coordinator
    storage_upload_coordinator_test.cpp
  )

  target_link_libraries(test_storage_upload_coordinator
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_storage_upload_coordinator
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_store_ctest(storage_upload_coordinator test_storage_upload_coordinator
    "${RAFT_STORE_LABELS_BASELINE}")
else()
  message(STATUS "Skip test_storage_upload_coordinator: storage_upload_coordinator_test.cpp not found")
endif()

# =========================
# Platform-neutral baseline:
# basic Raft behavior tests
# =========================
add_raft_gtest(test_raft_election test_raft_election.cpp
  "platform-neutral")
add_raft_gtest(test_raft_log_replication test_raft_log_replication.cpp
  "platform-neutral")
add_raft_gtest(test_raft_commit_apply test_raft_commit_apply.cpp
  "platform-neutral")
add_raft_gtest(test_raft_split_brain test_raft_split_brain.cpp
  "platform-neutral")
add_raft_gtest(test_t017_leader_switch_ordering test_t017_leader_switch_ordering.cpp
  "platform-neutral")

# =========================
# Mixed restart / durability boundary coverage:
# platform-neutral recovery logic plus Linux-specific failure-injection cases
# =========================
add_raft_gtest(persistence_test persistence_test.cpp
  "platform-neutral;durability-boundary;linux-specific-failure-injection")
add_raft_gtest(snapshot_test snapshot_test.cpp
  "platform-neutral;durability-boundary")
add_raft_gtest(raft_integration_test raft_integration_test.cpp
  "platform-neutral")

# =========================
# Snapshot catch-up / restart / diagnosis:
# restart executable keeps platform-neutral logic labels, while diagnosis stays
# Linux-primary in operational interpretation
# =========================
add_raft_gtest(test_raft_snapshot_catchup test_raft_snapshot_catchup.cpp
  "platform-neutral")
add_raft_gtest_sources(test_raft_snapshot_restart
  "platform-neutral;durability-boundary;linux-specific-failure-injection"
  test_raft_snapshot_restart.cpp
  test_raft_snapshot_recovery.cpp)
add_raft_gtest(test_raft_snapshot_diagnosis test_raft_snapshot_diagnosis.cpp
  "durability-boundary;linux-primary-diagnosis")

# =========================
# Mixed segment-log durability boundary coverage:
# includes platform-neutral recovery checks, Linux-specific failure-injection
# cases, and Linux-primary stress / diagnosis buckets
# =========================
add_raft_gtest(test_raft_segment_storage test_raft_segment_storage.cpp
  "platform-neutral;durability-boundary;linux-specific-failure-injection;linux-primary-diagnosis")

# =========================
# Mixed snapshot-storage durability boundary coverage:
# includes platform-neutral fallback logic plus Linux-specific failure-injection
# =========================
add_raft_gtest(test_snapshot_storage_reliability test_snapshot_storage_reliability.cpp
  "platform-neutral;durability-boundary;linux-specific-failure-injection")

# =========================
# Platform-neutral baseline:
# follower replication / catch-up behavior
# =========================
add_raft_gtest(test_raft_replicator_behavior test_raft_replicator_behavior.cpp
  "platform-neutral")
