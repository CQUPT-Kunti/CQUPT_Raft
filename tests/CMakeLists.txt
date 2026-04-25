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
  add_executable(${target_name} ${source_file})
  target_link_libraries(${target_name}
    PRIVATE
      raft_core
      GTest::gtest_main
  )
  gtest_discover_tests(${target_name} DISCOVERY_TIMEOUT 60)
endfunction()

# add_raft_gtest(test_command test_command.cpp)
# add_raft_gtest(test_state_machine test_state_machine.cpp)
# add_raft_gtest(test_min_heap_timer test_min_heap_timer.cpp)
# add_raft_gtest(test_thread_pool test_thread_pool.cpp)
# add_raft_gtest(test_raft_election test_raft_election.cpp)
# add_raft_gtest(test_raft_log_replication test_raft_log_replication.cpp)
# add_raft_gtest(test_raft_commit_apply test_raft_commit_apply.cpp)
# add_raft_gtest(persistence_test persistence_test.cpp)
# add_raft_gtest(snapshot_test snapshot_test.cpp)
# add_raft_gtest(raft_integration_test raft_integration_test.cpp)


