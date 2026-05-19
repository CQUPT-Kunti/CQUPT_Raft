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
#
# This file does not rename targets or alter test behavior. It only attaches
# labeling metadata so ctest discovery and related docs can distinguish the
# validation scope more clearly.
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
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/metadata_state_machine_test.cpp")
  add_executable(test_metadata_state_machine
    metadata_state_machine_test.cpp
  )

  target_link_libraries(test_metadata_state_machine
    PRIVATE
      raft_core
      GTest::gtest_main
  )

  target_include_directories(test_metadata_state_machine
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  gtest_discover_tests(test_metadata_state_machine
    DISCOVERY_TIMEOUT 60
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
      LABELS "platform-neutral;platform-neutral-fallback"
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

add_raft_gtest(test_state_machine test_state_machine.cpp
  "platform-neutral;platform-neutral-fallback")
add_raft_gtest(test_min_heap_timer test_min_heap_timer.cpp
  "platform-neutral;platform-neutral-fallback")
add_raft_gtest(test_thread_pool test_thread_pool.cpp
  "platform-neutral;platform-neutral-fallback")
add_raft_gtest(test_kv_service test_kv_service.cpp
  "platform-neutral")

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
add_raft_gtest(test_raft_snapshot_restart test_raft_snapshot_restart.cpp
  "platform-neutral;durability-boundary;linux-specific-failure-injection")
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
