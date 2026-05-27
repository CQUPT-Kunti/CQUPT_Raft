cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED RAFT_SOURCE_DIR OR RAFT_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "no_kv_surface_audit requires -DRAFT_SOURCE_DIR=<repo-root>")
endif()

file(TO_CMAKE_PATH "${RAFT_SOURCE_DIR}" RAFT_SOURCE_DIR)

set(AUDIT_FAILURES "")
set(AUDIT_DEFERRED_RISKS "")
set(AUDIT_WHITELIST_NOTES "")

macro(record_failure category detail)
  list(APPEND AUDIT_FAILURES "${category}: ${detail}")
endmacro()

macro(record_deferred_risk detail)
  list(APPEND AUDIT_DEFERRED_RISKS "${detail}")
endmacro()

macro(record_whitelist detail)
  list(APPEND AUDIT_WHITELIST_NOTES "${detail}")
endmacro()

macro(check_path_absent relative_path category)
  if(EXISTS "${RAFT_SOURCE_DIR}/${relative_path}")
    record_failure("${category}" "${relative_path}")
  endif()
endmacro()

macro(scan_file_for_literal relative_path literal category)
  set(_audit_path "${RAFT_SOURCE_DIR}/${relative_path}")
  if(EXISTS "${_audit_path}")
    file(READ "${_audit_path}" _audit_content)
    string(FIND "${_audit_content}" "${literal}" _audit_index)
    if(NOT _audit_index EQUAL -1)
      record_failure("${category}" "${relative_path} contains ${literal}")
    endif()
  endif()
endmacro()

macro(scan_file_for_regex relative_path regex label category)
  set(_audit_path "${RAFT_SOURCE_DIR}/${relative_path}")
  if(EXISTS "${_audit_path}")
    file(READ "${_audit_path}" _audit_content)
    string(REGEX MATCH "${regex}" _audit_match "${_audit_content}")
    if(NOT "${_audit_match}" STREQUAL "")
      record_failure("${category}" "${relative_path} matches ${label}")
    endif()
  endif()
endmacro()

function(collect_files out_var)
  set(_collected "")
  foreach(pattern IN LISTS ARGN)
    file(GLOB_RECURSE _matches RELATIVE "${RAFT_SOURCE_DIR}" "${pattern}")
    foreach(match IN LISTS _matches)
      if(NOT IS_DIRECTORY "${RAFT_SOURCE_DIR}/${match}")
        list(APPEND _collected "${match}")
      endif()
    endforeach()
  endforeach()
  list(REMOVE_DUPLICATES _collected)
  set(${out_var} "${_collected}" PARENT_SCOPE)
endfunction()

macro(remove_paths source_list_var)
  foreach(_remove_path IN LISTS ARGN)
    list(REMOVE_ITEM ${source_list_var} "${_remove_path}")
  endforeach()
endmacro()

macro(remove_matching_paths source_list_var regex)
  set(_kept_paths "")
  foreach(_candidate_path IN LISTS ${source_list_var})
    if(NOT _candidate_path MATCHES "${regex}")
      list(APPEND _kept_paths "${_candidate_path}")
    endif()
  endforeach()
  set(${source_list_var} "${_kept_paths}")
endmacro()

macro(scan_files_for_regex file_list_var regex label category)
  foreach(relative_path IN LISTS ${file_list_var})
    scan_file_for_regex("${relative_path}" "${regex}" "${label}" "${category}")
  endforeach()
endmacro()

macro(scan_files_for_literal file_list_var literal category)
  foreach(relative_path IN LISTS ${file_list_var})
    scan_file_for_literal("${relative_path}" "${literal}" "${category}")
  endforeach()
endmacro()

macro(assert_paths_covered subset_list_var covered_list_var category)
  foreach(_audit_relative_path IN LISTS ${subset_list_var})
    list(FIND ${covered_list_var} "${_audit_relative_path}" _audit_path_index)
    if(_audit_path_index EQUAL -1)
      record_failure("${category}"
        "${_audit_relative_path} is not included in ${covered_list_var}")
    endif()
  endforeach()
endmacro()

macro(require_file_stems_registered file_list_var registration_file category)
  set(_audit_registration_path "${RAFT_SOURCE_DIR}/${registration_file}")
  if(NOT EXISTS "${_audit_registration_path}")
    record_failure("${category}" "${registration_file} is missing")
  else()
    file(READ "${_audit_registration_path}" _audit_registration_content)
    foreach(_audit_relative_path IN LISTS ${file_list_var})
      get_filename_component(_audit_file_stem "${_audit_relative_path}" NAME_WE)
      string(FIND "${_audit_registration_content}" "${_audit_file_stem}" _audit_stem_index)
      if(_audit_stem_index EQUAL -1)
        record_failure("${category}"
          "${registration_file} does not mention ${_audit_file_stem}")
      endif()
    endforeach()
  endif()
endmacro()

record_whitelist("specs/006-remove-kv-metadata-state-machine/task-reports/** 保留历史迁移说明")
record_whitelist("specs/006-remove-kv-metadata-state-machine/{research,plan,spec,tasks,quickstart}.md 保留历史上下文")
record_whitelist("tests/no_kv_surface_audit.cmake 允许出现检测关键词")
record_whitelist("tests/AGENTS.md 与 tests/support/AGENTS.md 属于维护说明，不纳入 strict fail")
record_whitelist("tests/test-reports/** 属于历史测试组织记录，不纳入 strict fail")

collect_files(PRODUCTION_SOURCE_FILES
  "modules/*"
  "apps/*"
  "proto/*")
remove_matching_paths(PRODUCTION_SOURCE_FILES "AGENTS\\.md$")

set(PRODUCTION_BUILD_FILES
  "CMakeLists.txt"
  "tests/CMakeLists.txt")

collect_files(TEST_MAIN_FILES
  "tests/*")
remove_paths(TEST_MAIN_FILES
  "tests/no_kv_surface_audit.cmake"
  "tests/AGENTS.md"
  "tests/support/AGENTS.md"
  "tests/test-reports/test-file-organization.md")
remove_matching_paths(TEST_MAIN_FILES "AGENTS\\.md$")
remove_matching_paths(TEST_MAIN_FILES "^tests/test-reports/")

collect_files(STORE_PRODUCTION_FILES
  "modules/store/*")
remove_matching_paths(STORE_PRODUCTION_FILES "AGENTS\\.md$")

set(STORAGE_PROTO_FILES "")
if(EXISTS "${RAFT_SOURCE_DIR}/proto/storage_node.proto")
  list(APPEND STORAGE_PROTO_FILES "proto/storage_node.proto")
endif()

collect_files(STORAGE_TEST_ENTRY_FILES
  "tests/store*_test.cpp"
  "tests/storage*_test.cpp"
  "tests/local_disk_chunk_store_test.cpp"
  "tests/support/store_*"
  "tests/support/storage_*")
remove_matching_paths(STORAGE_TEST_ENTRY_FILES "AGENTS\\.md$")

assert_paths_covered(STORE_PRODUCTION_FILES PRODUCTION_SOURCE_FILES
  "audit coverage gap")
assert_paths_covered(STORAGE_PROTO_FILES PRODUCTION_SOURCE_FILES
  "audit coverage gap")
assert_paths_covered(STORAGE_TEST_ENTRY_FILES TEST_MAIN_FILES
  "audit coverage gap")
require_file_stems_registered(STORAGE_TEST_ENTRY_FILES "tests/CMakeLists.txt"
  "storage test registration gap")

# Strict: retired files / old paths must stay absent.
check_path_absent("modules/raft/service/kv_service_impl.h" "forbidden production file")
check_path_absent("modules/raft/service/kv_service_impl.cpp" "forbidden production file")
check_path_absent("modules/raft/storage_node" "forbidden production path")
check_path_absent("apps/raft_kv_client.cpp" "forbidden production file")
check_path_absent("proto/kv.proto" "forbidden proto file")
check_path_absent("modules/raft/state_machine/state_machine.h" "forbidden production file")
check_path_absent("modules/raft/state_machine/state_machine.cpp" "forbidden production file")
check_path_absent("tests/test_kv_service.cpp" "forbidden test file")
check_path_absent("tests/test_state_machine.cpp" "forbidden test file")

# Strict: build graph / main test registration cannot mention retired KV entry points.
scan_file_for_literal("CMakeLists.txt" "raft_kv_client" "forbidden build target")
scan_file_for_literal("CMakeLists.txt" "kv_service_impl" "forbidden build source")
scan_file_for_literal("CMakeLists.txt" "modules/raft/storage_node"
  "forbidden build path")
scan_file_for_literal("CMakeLists.txt"
  "modules/raft/state_machine/state_machine.cpp"
  "forbidden build source")
scan_file_for_literal("tests/CMakeLists.txt" "test_kv_service" "forbidden test target")
scan_file_for_literal("tests/CMakeLists.txt" "test_state_machine" "forbidden test target")
scan_file_for_literal("tests/CMakeLists.txt" "storage_node_types_test.cpp"
  "forbidden legacy storage test entry")

if(EXISTS "${RAFT_SOURCE_DIR}/proto/storage_node.proto")
  set(_audit_storage_proto_build_path "${RAFT_SOURCE_DIR}/CMakeLists.txt")
  if(EXISTS "${_audit_storage_proto_build_path}")
    file(READ "${_audit_storage_proto_build_path}" _audit_storage_proto_build_content)
    string(FIND "${_audit_storage_proto_build_content}" "proto/storage_node.proto"
      _audit_storage_proto_build_index)
    if(_audit_storage_proto_build_index EQUAL -1)
      record_failure("storage proto registration gap"
        "CMakeLists.txt does not mention proto/storage_node.proto")
    endif()
  endif()
endif()

# Strict: production source tree cannot reintroduce retired KV business symbols.
scan_files_for_regex(PRODUCTION_SOURCE_FILES
  "CommandType::kSet([^A-Za-z0-9_]|$)"
  "CommandType::kSet"
  "forbidden production symbol")
scan_files_for_regex(PRODUCTION_SOURCE_FILES
  "CommandType::kDelete([^A-Za-z0-9_]|$)"
  "CommandType::kDelete"
  "forbidden production symbol")
scan_files_for_regex(PRODUCTION_SOURCE_FILES
  "(^|[^A-Za-z0-9_])kSet([^A-Za-z0-9_]|$)"
  "kSet token"
  "forbidden production symbol")
scan_files_for_regex(PRODUCTION_SOURCE_FILES
  "(^|[^A-Za-z0-9_])kDelete([^A-Za-z0-9_]|$)"
  "kDelete token"
  "forbidden production symbol")
scan_files_for_regex(PRODUCTION_SOURCE_FILES
  "(^|[^A-Za-z0-9_])KvStateMachine([^A-Za-z0-9_]|$)"
  "KvStateMachine"
  "forbidden production symbol")
scan_files_for_regex(PRODUCTION_SOURCE_FILES
  "(^|[^A-Za-z0-9_])CompositeKvMetadataStateMachine([^A-Za-z0-9_]|$)"
  "CompositeKvMetadataStateMachine"
  "forbidden production symbol")
scan_files_for_regex(PRODUCTION_SOURCE_FILES
  "(^|[^A-Za-z0-9_])KvService([^A-Za-z0-9_]|$)"
  "KvService"
  "forbidden production symbol")
scan_files_for_regex(PRODUCTION_SOURCE_FILES
  "(^|[^A-Za-z0-9_])KvStatusCode([^A-Za-z0-9_]|$)"
  "KvStatusCode"
  "forbidden production symbol")
scan_files_for_regex(PRODUCTION_SOURCE_FILES
  "(^|[^A-Za-z0-9_])raft_kv_client([^A-Za-z0-9_]|$)"
  "raft_kv_client"
  "forbidden production symbol")
scan_files_for_regex(PRODUCTION_SOURCE_FILES
  "(^|[^A-Za-z0-9_])PutRequest([^A-Za-z0-9_]|$)"
  "PutRequest"
  "forbidden production symbol")
scan_files_for_regex(PRODUCTION_SOURCE_FILES
  "(^|[^A-Za-z0-9_])GetRequest([^A-Za-z0-9_]|$)"
  "GetRequest"
  "forbidden production symbol")
scan_files_for_regex(PRODUCTION_SOURCE_FILES
  "(^|[^A-Za-z0-9_])DeleteRequest([^A-Za-z0-9_]|$)"
  "DeleteRequest"
  "forbidden production symbol")
scan_files_for_literal(PRODUCTION_SOURCE_FILES "SET|" "forbidden production symbol")
scan_files_for_literal(PRODUCTION_SOURCE_FILES "DEL|" "forbidden production symbol")
scan_files_for_regex(PRODUCTION_SOURCE_FILES
  "(^|[^A-Za-z0-9_])DebugGetValue([^A-Za-z0-9_]|$)"
  "DebugGetValue"
  "forbidden production symbol")
scan_files_for_literal(PRODUCTION_SOURCE_FILES "kv-service" "forbidden production symbol")
scan_files_for_regex(PRODUCTION_SOURCE_FILES
  "(^|[^A-Za-z0-9_])kv\\.proto([^A-Za-z0-9_]|$)"
  "kv.proto"
  "forbidden production symbol")
scan_files_for_literal(PRODUCTION_SOURCE_FILES "raft/state_machine/state_machine.h"
  "forbidden production include")
scan_files_for_literal(PRODUCTION_SOURCE_FILES
  "modules/raft/state_machine/state_machine.cpp"
  "forbidden production source reference")

# Strict: tests main path cannot reference retired KV symbols or retired default paths.
scan_files_for_regex(TEST_MAIN_FILES
  "CommandType::kSet([^A-Za-z0-9_]|$)"
  "CommandType::kSet"
  "forbidden test symbol")
scan_files_for_regex(TEST_MAIN_FILES
  "CommandType::kDelete([^A-Za-z0-9_]|$)"
  "CommandType::kDelete"
  "forbidden test symbol")
scan_files_for_regex(TEST_MAIN_FILES
  "(^|[^A-Za-z0-9_])SetCommand\\("
  "SetCommand("
  "forbidden test symbol")
scan_files_for_regex(TEST_MAIN_FILES
  "(^|[^A-Za-z0-9_])DeleteCommand\\("
  "DeleteCommand("
  "forbidden test symbol")
scan_files_for_regex(TEST_MAIN_FILES
  "(^|[^A-Za-z0-9_])DebugGetValue([^A-Za-z0-9_]|$)"
  "DebugGetValue"
  "forbidden test symbol")
scan_files_for_literal(TEST_MAIN_FILES "raft/state_machine/state_machine.h"
  "forbidden test include")
scan_files_for_regex(TEST_MAIN_FILES
  "(^|[^A-Za-z0-9_])KvStateMachine([^A-Za-z0-9_]|$)"
  "KvStateMachine"
  "forbidden test symbol")
scan_files_for_regex(TEST_MAIN_FILES
  "(^|[^A-Za-z0-9_])KV regression-only path([^A-Za-z0-9_]|$)"
  "KV regression-only path"
  "forbidden test doc symbol")
scan_files_for_regex(TEST_MAIN_FILES
  "(^|[^A-Za-z0-9_])KvStateMachineTest([^A-Za-z0-9_]|$)"
  "KvStateMachineTest"
  "forbidden test doc symbol")
scan_files_for_regex(TEST_MAIN_FILES
  "(^|[^A-Za-z0-9_])test_state_machine([^A-Za-z0-9_]|$)"
  "test_state_machine"
  "forbidden test doc symbol")

# Deferred to T059: script/preset fallback text still needs a metadata-only rewrite.
set(_audit_ps1_path "${RAFT_SOURCE_DIR}/test.ps1")
if(EXISTS "${_audit_ps1_path}")
  file(READ "${_audit_ps1_path}" _audit_ps1_content)
  string(REGEX MATCH "(^|[^A-Za-z0-9_])KvStateMachineTest([^A-Za-z0-9_]|$)"
    _audit_ps1_match "${_audit_ps1_content}")
  if(NOT "${_audit_ps1_match}" STREQUAL "")
    record_deferred_risk("test.ps1 仍保留 KvStateMachineTest fallback 子集说明，转交 T059")
  endif()
endif()

set(_audit_presets_path "${RAFT_SOURCE_DIR}/CMakePresets.json")
if(EXISTS "${_audit_presets_path}")
  file(READ "${_audit_presets_path}" _audit_presets_content)
  string(REGEX MATCH "(^|[^A-Za-z0-9_])KvStateMachineTest([^A-Za-z0-9_]|$)"
    _audit_presets_match "${_audit_presets_content}")
  if(NOT "${_audit_presets_match}" STREQUAL "")
    record_deferred_risk("CMakePresets.json 仍保留 KvStateMachineTest fallback filter，转交 T059")
  endif()
endif()

list(REMOVE_DUPLICATES AUDIT_FAILURES)
list(REMOVE_DUPLICATES AUDIT_DEFERRED_RISKS)
list(REMOVE_DUPLICATES AUDIT_WHITELIST_NOTES)

if(AUDIT_WHITELIST_NOTES)
  message(STATUS "no_kv_surface_audit whitelist scope:")
  foreach(note IN LISTS AUDIT_WHITELIST_NOTES)
    message(STATUS "  - ${note}")
  endforeach()
endif()

if(AUDIT_DEFERRED_RISKS)
  message(STATUS "no_kv_surface_audit deferred risks (tracked outside T058 strict-fail scope):")
  foreach(risk IN LISTS AUDIT_DEFERRED_RISKS)
    message(STATUS "  - ${risk}")
  endforeach()
endif()

if(AUDIT_FAILURES)
  list(JOIN AUDIT_FAILURES "\n  - " AUDIT_FAILURE_TEXT)
  message(FATAL_ERROR
    "no_kv_surface_audit failed.\n"
    "Strict retired KV surface regressions:\n"
    "  - ${AUDIT_FAILURE_TEXT}\n")
endif()

message(STATUS "no_kv_surface_audit passed: production code and tests main path remain strict metadata-only.")
