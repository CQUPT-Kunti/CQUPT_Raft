cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED RAFT_SOURCE_DIR OR RAFT_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "no_kv_surface_audit requires -DRAFT_SOURCE_DIR=<repo-root>")
endif()

file(TO_CMAKE_PATH "${RAFT_SOURCE_DIR}" RAFT_SOURCE_DIR)

set(AUDIT_FAILURES "")
set(AUDIT_BLOCKERS "")

macro(record_failure category detail)
  list(APPEND AUDIT_FAILURES "${category}: ${detail}")
endmacro()

macro(record_blocker detail)
  list(APPEND AUDIT_BLOCKERS "${detail}")
endmacro()

macro(check_path_absent relative_path category)
  if(EXISTS "${RAFT_SOURCE_DIR}/${relative_path}")
    record_failure("${category}" "${relative_path}")
  endif()
endmacro()

macro(check_literal_absent relative_path literal category)
  set(_audit_path "${RAFT_SOURCE_DIR}/${relative_path}")
  if(EXISTS "${_audit_path}")
    file(READ "${_audit_path}" _audit_content)
    string(FIND "${_audit_content}" "${literal}" _audit_index)
    if(NOT _audit_index EQUAL -1)
      record_failure("${category}" "${relative_path} contains ${literal}")
    endif()
  endif()
endmacro()

macro(check_literal_present relative_path literal)
  set(_audit_path "${RAFT_SOURCE_DIR}/${relative_path}")
  if(EXISTS "${_audit_path}")
    file(READ "${_audit_path}" _audit_content)
    string(FIND "${_audit_content}" "${literal}" _audit_index)
    if(NOT _audit_index EQUAL -1)
      record_blocker("${relative_path} contains ${literal}")
    endif()
  endif()
endmacro()

macro(check_path_present relative_path)
  if(EXISTS "${RAFT_SOURCE_DIR}/${relative_path}")
    record_blocker("${relative_path} still exists")
  endif()
endmacro()

# Strict no-KV surface failures: these surfaces are expected to be fully retired.
check_path_absent("modules/raft/service/kv_service_impl.h" "forbidden service file")
check_path_absent("modules/raft/service/kv_service_impl.cpp" "forbidden service file")
check_path_absent("tests/test_kv_service.cpp" "forbidden test file")
check_path_absent("apps/raft_kv_client.cpp" "forbidden client file")
check_path_absent("proto/kv.proto" "forbidden proto file")

check_literal_absent("CMakeLists.txt" "raft_kv_client" "forbidden build target")
check_literal_absent("CMakeLists.txt" "kv_service_impl" "forbidden build source")
check_literal_absent("tests/CMakeLists.txt" "test_kv_service" "forbidden test target")

check_literal_absent("proto/raft.proto" "service KvService" "forbidden proto service")
check_literal_absent("proto/raft.proto" "enum KvStatusCode" "forbidden proto enum")
check_literal_absent("proto/raft.proto" "message PutRequest" "forbidden proto message")
check_literal_absent("proto/raft.proto" "message GetRequest" "forbidden proto message")
check_literal_absent("proto/raft.proto" "message DeleteRequest" "forbidden proto message")
check_literal_absent("proto/raft.proto" "message PutResponse" "forbidden proto message")
check_literal_absent("proto/raft.proto" "message GetResponse" "forbidden proto message")
check_literal_absent("proto/raft.proto" "message DeleteResponse" "forbidden proto message")

check_literal_absent("test.sh" "raft_kv_client" "forbidden script entry")
check_literal_absent("test.sh" "test_kv_service" "forbidden script entry")
check_literal_absent("test.sh" "kv_service_impl" "forbidden script entry")
check_literal_absent("test.sh" "KV fallback" "forbidden script entry")

check_literal_absent("test.ps1" "raft_kv_client" "forbidden script entry")
check_literal_absent("test.ps1" "test_kv_service" "forbidden script entry")
check_literal_absent("test.ps1" "kv_service_impl" "forbidden script entry")
check_literal_absent("test.ps1" "KV fallback" "forbidden script entry")

check_literal_absent("README.md" "raft_kv_client" "forbidden README entry")
check_literal_absent("README.md" "KvService" "forbidden README entry")
check_literal_absent("README.md" "KV fallback" "forbidden README entry")

check_literal_absent("docs/PERSISTENCE_DURABILITY_CONTRACT.md" "KvStateMachine::SaveSnapshot()" "stale current-path doc")
check_literal_absent("docs/PERSISTENCE_DURABILITY_CONTRACT.md" "KV fallback" "forbidden current-path doc")

check_literal_absent("tests/README.md" "test_kv_service" "stale test doc")
check_literal_absent("tests/README.md" "test_kv_service.cpp" "stale test doc")
check_literal_absent("tests/README.md" "raft_kv_client" "stale test doc")
check_literal_absent("tests/README.md" "KV fallback" "stale test doc")

check_literal_absent("docs/CURRENT_INDUSTRIALIZATION_ANALYSIS.md"
  "KV client / KV service 仍被当作当前主要外部接口"
  "stale current-path doc")
check_literal_absent("docs/CURRENT_INDUSTRIALIZATION_ANALYSIS.md"
  "`proto/raft.proto` 中的 `KvService`"
  "stale current-path doc")
check_literal_absent("docs/CURRENT_INDUSTRIALIZATION_ANALYSIS.md"
  "`KvService` 与 Raft service 同处一个 proto"
  "stale current-path doc")
check_literal_absent("docs/CURRENT_INDUSTRIALIZATION_ANALYSIS.md"
  "`RaftService` 和 `KvService` 在同一个 `proto/raft.proto`"
  "stale current-path doc")

# Known blockers: report clearly but do not fail T050 on them.
check_literal_present("modules/raft/common/command.h" "kSet")
check_literal_present("modules/raft/common/command.h" "kDelete")
check_literal_present("modules/raft/common/command.cpp" "CommandType::kSet")
check_literal_present("modules/raft/common/command.cpp" "CommandType::kDelete")
check_path_present("modules/raft/state_machine/state_machine.h")
check_path_present("modules/raft/state_machine/state_machine.cpp")
check_path_present("tests/test_state_machine.cpp")
check_literal_present("modules/raft/state_machine/state_machine.h" "KvStateMachine")
check_literal_present("modules/raft/node/raft_node.cpp" "KvStateMachine")
check_literal_present("tests/support/raft_snapshot_restart_test_utils.h" "SetCommand(")
check_literal_present("tests/support/raft_snapshot_restart_test_utils.h" "DeleteCommand(")

list(REMOVE_DUPLICATES AUDIT_FAILURES)
list(REMOVE_DUPLICATES AUDIT_BLOCKERS)

if(AUDIT_BLOCKERS)
  message(STATUS "no_kv_surface_audit known blockers (tolerated in T050):")
  foreach(blocker IN LISTS AUDIT_BLOCKERS)
    message(STATUS "  - ${blocker}")
  endforeach()
endif()

if(AUDIT_FAILURES)
  list(JOIN AUDIT_FAILURES "\n  - " AUDIT_FAILURE_TEXT)
  if(AUDIT_BLOCKERS)
    list(JOIN AUDIT_BLOCKERS "\n  - " AUDIT_BLOCKER_TEXT)
    message(FATAL_ERROR
      "no_kv_surface_audit failed.\n"
      "Strict retired KV surface regressions:\n"
      "  - ${AUDIT_FAILURE_TEXT}\n"
      "Known blockers intentionally not promoted to failure in T050:\n"
      "  - ${AUDIT_BLOCKER_TEXT}\n")
  else()
    message(FATAL_ERROR
      "no_kv_surface_audit failed.\n"
      "Strict retired KV surface regressions:\n"
      "  - ${AUDIT_FAILURE_TEXT}\n")
  endif()
endif()

message(STATUS "no_kv_surface_audit passed: retired KV service/client/proto/doc surfaces remain absent.")
