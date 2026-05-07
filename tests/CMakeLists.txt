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

function(add_raft_gtest target_name source_file)
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
  )
endfunction()

# =========================
# Basic unit tests
# =========================
add_raft_gtest(test_command test_command.cpp)
add_raft_gtest(test_state_machine test_state_machine.cpp)
add_raft_gtest(test_min_heap_timer test_min_heap_timer.cpp)
add_raft_gtest(test_thread_pool test_thread_pool.cpp)
add_raft_gtest(test_kv_service test_kv_service.cpp)

# =========================
# Basic Raft behavior tests
# =========================
add_raft_gtest(test_raft_election test_raft_election.cpp)
add_raft_gtest(test_raft_log_replication test_raft_log_replication.cpp)
add_raft_gtest(test_raft_commit_apply test_raft_commit_apply.cpp)
add_raft_gtest(test_raft_split_brain test_raft_split_brain.cpp)

# =========================
# Persistence / snapshot legacy tests
# =========================
add_raft_gtest(persistence_test persistence_test.cpp)
add_raft_gtest(snapshot_test snapshot_test.cpp)
add_raft_gtest(raft_integration_test raft_integration_test.cpp)

# =========================
# Snapshot catch-up / restart / diagnosis tests
# =========================
add_raft_gtest(test_raft_snapshot_catchup test_raft_snapshot_catchup.cpp)
add_raft_gtest(test_raft_snapshot_restart test_raft_snapshot_restart.cpp)
add_raft_gtest(test_raft_snapshot_diagnosis test_raft_snapshot_diagnosis.cpp)

# =========================
# Segment log storage tests
# =========================
add_raft_gtest(test_raft_segment_storage test_raft_segment_storage.cpp)

# =========================
# Snapshot storage reliability tests
# =========================
add_raft_gtest(test_snapshot_storage_reliability test_snapshot_storage_reliability.cpp)

# =========================
# Replicator behavior tests
# =========================
add_raft_gtest(test_raft_replicator_behavior test_raft_replicator_behavior.cpp)
