cmake_minimum_required(VERSION 3.20)

if(WIN32)
  if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "D:/Tools/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
  endif()

  if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows"
        CACHE STRING "Vcpkg target triplet")
  endif()
endif()

project(raft_grpc_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 让 VS Code / clangd 能拿到真实编译参数
# 注意：这个对 Makefile / Ninja 生效，Visual Studio 生成器会忽略
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =========================
# Build parallelism control
# =========================
# Ninja 默认会使用较高并发，gRPC / protobuf / absl 编译时容易占满 CPU 和内存。
# 这里通过 CMake job pool 限制 Ninja 的编译、链接和 proto 生成并发。
#
# 可在重新配置时覆盖：
#   cmake -S . -B build -DRAFT_BUILD_JOBS=2 -DRAFT_LINK_JOBS=1 -DRAFT_PROTO_JOBS=1
#
# 更稳但更慢：
#   -DRAFT_BUILD_JOBS=1 -DRAFT_LINK_JOBS=1 -DRAFT_PROTO_JOBS=1
set(RAFT_BUILD_JOBS 2 CACHE STRING "Max concurrent compile jobs for Ninja")
set(RAFT_LINK_JOBS 1 CACHE STRING "Max concurrent link jobs for Ninja")
set(RAFT_PROTO_JOBS 1 CACHE STRING "Max concurrent protobuf/gRPC codegen jobs for Ninja")

if(CMAKE_GENERATOR MATCHES "Ninja")
  set_property(GLOBAL PROPERTY JOB_POOLS
    raft_compile_pool=${RAFT_BUILD_JOBS}
    raft_link_pool=${RAFT_LINK_JOBS}
    raft_proto_pool=${RAFT_PROTO_JOBS}
  )

  set(CMAKE_JOB_POOL_COMPILE raft_compile_pool)
  set(CMAKE_JOB_POOL_LINK raft_link_pool)

  set(RAFT_PROTO_JOB_POOL_ARGS JOB_POOL raft_proto_pool)
else()
  set(RAFT_PROTO_JOB_POOL_ARGS)
endif()

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(Threads REQUIRED)

message(STATUS "Using protobuf ${Protobuf_VERSION}")
message(STATUS "Using gRPC ${gRPC_VERSION}")

set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/raft.proto)
set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(PROJECT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

file(MAKE_DIRECTORY ${GENERATED_DIR})

if(CMAKE_CROSSCOMPILING)
  find_program(_PROTOBUF_PROTOC protoc REQUIRED)
  find_program(_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin REQUIRED)
else()
  set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
  set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
endif()

set(PROTO_SRCS ${GENERATED_DIR}/raft.pb.cc)
set(PROTO_HDRS ${GENERATED_DIR}/raft.pb.h)
set(GRPC_SRCS ${GENERATED_DIR}/raft.grpc.pb.cc)
set(GRPC_HDRS ${GENERATED_DIR}/raft.grpc.pb.h)

add_custom_command(
  OUTPUT
    ${PROTO_SRCS}
    ${PROTO_HDRS}
    ${GRPC_SRCS}
    ${GRPC_HDRS}
  COMMAND ${_PROTOBUF_PROTOC}
  ARGS
    --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
    --cpp_out=${GENERATED_DIR}
    --grpc_out=${GENERATED_DIR}
    --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN_EXECUTABLE}
    ${PROTO_FILE}
  DEPENDS ${PROTO_FILE}
  ${RAFT_PROTO_JOB_POOL_ARGS}
  VERBATIM
)

add_library(raft_proto
  ${PROTO_SRCS}
  ${GRPC_SRCS}
)

target_include_directories(raft_proto
  PUBLIC
    ${GENERATED_DIR}
)

target_link_libraries(raft_proto
  PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
)

add_library(raft_core
  src/min_heap_timer.cpp
  src/thread_pool.cpp
  src/command.cpp
  src/raft_node.cpp
  src/raft_service_impl.cpp
  src/kv_service_impl.cpp
  src/state_machine.cpp
  src/raft_storage.cpp
  src/snapshot_storage.cpp
  src/replicator.cpp
)

target_include_directories(raft_core
  PUBLIC
    ${PROJECT_INCLUDE_DIR}
    ${GENERATED_DIR}
)

target_link_libraries(raft_core
  PUBLIC
    raft_proto
    Threads::Threads
)

add_executable(raft_demo
  src/main.cpp
)

target_link_libraries(raft_demo
  PRIVATE
    raft_core
)

add_executable(raft_kv_client
  src/raft_kv_client.cpp
)

target_link_libraries(raft_kv_client
  PRIVATE
    raft_proto
)

enable_testing()
add_subdirectory(tests)
