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
  src/state_machine.cpp
  src/raft_storage.cpp
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

enable_testing()
add_subdirectory(tests)