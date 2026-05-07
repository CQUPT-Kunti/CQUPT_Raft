#include <grpcpp/grpcpp.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "raft.grpc.pb.h"

namespace
{

  void PrintUsage()
  {
    std::cerr
        << "usage:\n"
        << "  raft_kv_client <addr> put <key> <value>\n"
        << "  raft_kv_client <addr> get <key>\n"
        << "  raft_kv_client <addr> delete <key>\n"
        << "  raft_kv_client <addr> status\n"
        << "  raft_kv_client <addr> health\n"
        << "  raft_kv_client <addr> metrics\n";
  }

  std::unique_ptr<raft::KvService::Stub> MakeStub(const std::string &address)
  {
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    return raft::KvService::NewStub(channel);
  }

  int RunPut(raft::KvService::Stub *stub, const std::string &key, const std::string &value)
  {
    raft::PutRequest request;
    request.set_key(key);
    request.set_value(value);

    raft::PutResponse response;
    grpc::ClientContext context;
    const grpc::Status status = stub->Put(&context, request, &response);
    if (!status.ok())
    {
      std::cerr << "rpc failed: " << status.error_message() << '\n';
      return 1;
    }

    std::cout << "code=" << response.code()
              << " term=" << response.term()
              << " leader_id=" << response.leader_id()
              << " leader_address=" << response.leader_address()
              << " log_index=" << response.log_index()
              << " message=" << response.message() << '\n';
    return response.code() == raft::KV_STATUS_CODE_OK ? 0 : 2;
  }

  int RunDelete(raft::KvService::Stub *stub, const std::string &key)
  {
    raft::DeleteRequest request;
    request.set_key(key);

    raft::DeleteResponse response;
    grpc::ClientContext context;
    const grpc::Status status = stub->Delete(&context, request, &response);
    if (!status.ok())
    {
      std::cerr << "rpc failed: " << status.error_message() << '\n';
      return 1;
    }

    std::cout << "code=" << response.code()
              << " term=" << response.term()
              << " leader_id=" << response.leader_id()
              << " leader_address=" << response.leader_address()
              << " log_index=" << response.log_index()
              << " message=" << response.message() << '\n';
    return response.code() == raft::KV_STATUS_CODE_OK ? 0 : 2;
  }

  int RunGet(raft::KvService::Stub *stub, const std::string &key)
  {
    raft::GetRequest request;
    request.set_key(key);

    raft::GetResponse response;
    grpc::ClientContext context;
    const grpc::Status status = stub->Get(&context, request, &response);
    if (!status.ok())
    {
      std::cerr << "rpc failed: " << status.error_message() << '\n';
      return 1;
    }

    std::cout << "code=" << response.code()
              << " term=" << response.term()
              << " leader_id=" << response.leader_id()
              << " leader_address=" << response.leader_address()
              << " found=" << (response.found() ? "true" : "false")
              << " value=" << response.value()
              << " message=" << response.message() << '\n';
    return response.code() == raft::KV_STATUS_CODE_OK ? 0 : 2;
  }

  int RunStatus(raft::KvService::Stub *stub)
  {
    raft::StatusRequest request;
    raft::StatusResponse response;
    grpc::ClientContext context;
    const grpc::Status status = stub->Status(&context, request, &response);
    if (!status.ok())
    {
      std::cerr << "rpc failed: " << status.error_message() << '\n';
      return 1;
    }

    std::cout << "node_id=" << response.node_id()
              << " address=" << response.address()
              << " role=" << response.role()
              << " term=" << response.term()
              << " leader_id=" << response.leader_id()
              << " leader_address=" << response.leader_address()
              << " commit_index=" << response.commit_index()
              << " last_applied=" << response.last_applied()
              << " last_log_index=" << response.last_log_index()
              << " snapshot_index=" << response.snapshot_index() << '\n';

    for (const auto &peer : response.peers())
    {
      std::cout << "peer_id=" << peer.peer_id()
                << " address=" << peer.address()
                << " match_index=" << peer.match_index()
                << " next_index=" << peer.next_index() << '\n';
    }
    return 0;
  }

  int RunHealth(raft::KvService::Stub *stub)
  {
    raft::HealthRequest request;
    raft::HealthResponse response;
    grpc::ClientContext context;
    const grpc::Status status = stub->Health(&context, request, &response);
    if (!status.ok())
    {
      std::cerr << "rpc failed: " << status.error_message() << '\n';
      return 1;
    }

    std::cout << "ok=" << (response.ok() ? "true" : "false")
              << " node_id=" << response.node_id()
              << " role=" << response.role()
              << " term=" << response.term()
              << " leader_id=" << response.leader_id()
              << " message=" << response.message() << '\n';
    return response.ok() ? 0 : 2;
  }

  int RunMetrics(raft::KvService::Stub *stub)
  {
    raft::MetricsRequest request;
    raft::MetricsResponse response;
    grpc::ClientContext context;
    const grpc::Status status = stub->Metrics(&context, request, &response);
    if (!status.ok())
    {
      std::cerr << "rpc failed: " << status.error_message() << '\n';
      return 1;
    }

    const auto &metrics = response.metrics();
    std::cout << "node_id=" << response.node_id()
              << " role=" << response.role()
              << " term=" << response.term()
              << " leader_id=" << response.leader_id()
              << " leader_address=" << response.leader_address()
              << " propose_success=" << metrics.propose_success_count()
              << " propose_failure=" << metrics.propose_failure_count()
              << " elections=" << metrics.election_count()
              << " leader_changes=" << metrics.leader_change_count()
              << " snapshot_success=" << metrics.snapshot_success_count()
              << " snapshot_failure=" << metrics.snapshot_failure_count()
              << " storage_persist_failure=" << metrics.storage_persist_failure_count()
              << '\n';
    for (const auto &rpc_metric : metrics.rpc_metrics())
    {
      std::cout << "rpc=" << rpc_metric.name()
                << " success=" << rpc_metric.success_count()
                << " failure=" << rpc_metric.failure_count()
                << " total_latency_us=" << rpc_metric.total_latency_us()
                << " max_latency_us=" << rpc_metric.max_latency_us() << '\n';
    }
    return 0;
  }

} // namespace

int main(int argc, char **argv)
{
  if (argc < 3)
  {
    PrintUsage();
    return 2;
  }

  const std::string address = argv[1];
  const std::string command = argv[2];
  auto stub = MakeStub(address);

  if (command == "put")
  {
    if (argc != 5)
    {
      PrintUsage();
      return 2;
    }
    return RunPut(stub.get(), argv[3], argv[4]);
  }
  if (command == "get")
  {
    if (argc != 4)
    {
      PrintUsage();
      return 2;
    }
    return RunGet(stub.get(), argv[3]);
  }
  if (command == "delete")
  {
    if (argc != 4)
    {
      PrintUsage();
      return 2;
    }
    return RunDelete(stub.get(), argv[3]);
  }
  if (command == "status")
  {
    return RunStatus(stub.get());
  }
  if (command == "health")
  {
    return RunHealth(stub.get());
  }
  if (command == "metrics")
  {
    return RunMetrics(stub.get());
  }

  PrintUsage();
  return 2;
}
