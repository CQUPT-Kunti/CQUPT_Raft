#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "raft.grpc.pb.h"
#include "raft/config.h"
#include "raft/raft_node.h"

namespace raftdemo
{
  namespace
  {

    using namespace std::chrono_literals;

    std::filesystem::path TestBinaryDir()
    {
      return std::filesystem::current_path();
    }

    std::filesystem::path MakeTestRoot(const std::string &test_name)
    {
      std::random_device rd;
      return TestBinaryDir() / "raft_test_data" / "kv_service" /
             (test_name + "_" + std::to_string(rd()));
    }

    snapshotConfig MakeSnapshotConfig(const std::filesystem::path &root, int node_id)
    {
      snapshotConfig cfg;
      cfg.enabled = true;
      cfg.snapshot_dir = (root / ("node_" + std::to_string(node_id))).string();
      cfg.log_threshold = 8;
      cfg.snapshot_interval = std::chrono::minutes(10);
      cfg.max_snapshot_count = 2;
      cfg.load_on_startup = true;
      cfg.file_prefix = "snapshot";
      return cfg;
    }

    NodeConfig MakeSingleNodeConfig(const std::filesystem::path &root, int port)
    {
      NodeConfig cfg;
      cfg.node_id = 1;
      cfg.address = "127.0.0.1:" + std::to_string(port);
      cfg.election_timeout_min = std::chrono::milliseconds(200);
      cfg.election_timeout_max = std::chrono::milliseconds(350);
      cfg.heartbeat_interval = std::chrono::milliseconds(60);
      cfg.rpc_deadline = std::chrono::milliseconds(250);
      cfg.data_dir = (root / "data" / "node_1").string();
      return cfg;
    }

    std::vector<NodeConfig> BuildThreeNodeConfigs(const std::filesystem::path &root, int base_port)
    {
      NodeConfig n1;
      n1.node_id = 1;
      n1.address = "127.0.0.1:" + std::to_string(base_port + 1);
      n1.peers = {
          PeerConfig{2, "127.0.0.1:" + std::to_string(base_port + 2)},
          PeerConfig{3, "127.0.0.1:" + std::to_string(base_port + 3)},
      };
      n1.election_timeout_min = std::chrono::milliseconds(250);
      n1.election_timeout_max = std::chrono::milliseconds(500);
      n1.heartbeat_interval = std::chrono::milliseconds(80);
      n1.rpc_deadline = std::chrono::milliseconds(250);
      n1.data_dir = (root / "data" / "node_1").string();

      NodeConfig n2 = n1;
      n2.node_id = 2;
      n2.address = "127.0.0.1:" + std::to_string(base_port + 2);
      n2.peers = {
          PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
          PeerConfig{3, "127.0.0.1:" + std::to_string(base_port + 3)},
      };
      n2.data_dir = (root / "data" / "node_2").string();

      NodeConfig n3 = n1;
      n3.node_id = 3;
      n3.address = "127.0.0.1:" + std::to_string(base_port + 3);
      n3.peers = {
          PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
          PeerConfig{2, "127.0.0.1:" + std::to_string(base_port + 2)},
      };
      n3.data_dir = (root / "data" / "node_3").string();

      return {n1, n2, n3};
    }

    class SingleNodeRunner
    {
    public:
      SingleNodeRunner(NodeConfig config, snapshotConfig snapshot_config)
          : node_(std::make_shared<RaftNode>(std::move(config), std::move(snapshot_config))) {}

      ~SingleNodeRunner()
      {
        if (node_)
        {
          node_->Stop();
        }
        if (wait_thread_.joinable())
        {
          wait_thread_.join();
        }
      }

      void Start()
      {
        node_->Start();
        wait_thread_ = std::thread([node = node_]()
                                   { node->Wait(); });
      }

      bool WaitForLeader(std::chrono::milliseconds timeout) const
      {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
          if (node_->Describe().find("role=Leader") != std::string::npos)
          {
            return true;
          }
          std::this_thread::sleep_for(50ms);
        }
        return false;
      }

      std::shared_ptr<RaftNode> node() const { return node_; }

    private:
      std::shared_ptr<RaftNode> node_;
      std::thread wait_thread_;
    };

    class ThreeNodeRunner
    {
    public:
      ThreeNodeRunner(std::vector<NodeConfig> configs, std::vector<snapshotConfig> snapshots)
      {
        for (std::size_t i = 0; i < configs.size(); ++i)
        {
          nodes_.push_back(std::make_shared<RaftNode>(configs[i], snapshots[i]));
        }
      }

      ~ThreeNodeRunner()
      {
        for (const auto &node : nodes_)
        {
          node->Stop();
        }
        for (auto &thread : wait_threads_)
        {
          if (thread.joinable())
          {
            thread.join();
          }
        }
      }

      void Start()
      {
        for (const auto &node : nodes_)
        {
          node->Start();
        }
        for (const auto &node : nodes_)
        {
          wait_threads_.emplace_back([node]()
                                     { node->Wait(); });
        }
      }

      std::shared_ptr<RaftNode> WaitForLeader(std::chrono::milliseconds timeout) const
      {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
          for (const auto &node : nodes_)
          {
            if (node->Describe().find("role=Leader") != std::string::npos)
            {
              return node;
            }
          }
          std::this_thread::sleep_for(50ms);
        }
        return nullptr;
      }

      const std::vector<std::shared_ptr<RaftNode>> &nodes() const { return nodes_; }

    private:
      std::vector<std::shared_ptr<RaftNode>> nodes_;
      std::vector<std::thread> wait_threads_;
    };

    std::unique_ptr<raft::KvService::Stub> MakeStub(const std::string &address)
    {
      auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
      return raft::KvService::NewStub(channel);
    }

  } // namespace

  TEST(RaftKvServiceTest, SingleNodeSupportsPutGetDeleteAndStatusHealth)
  {
    const auto root = MakeTestRoot("single_node");
    const int port = 55061;
    SingleNodeRunner runner(MakeSingleNodeConfig(root, port),
                            MakeSnapshotConfig(root / "snapshots", 1));
    runner.Start();

    ASSERT_TRUE(runner.WaitForLeader(5s));

    auto stub = MakeStub("127.0.0.1:" + std::to_string(port));

    {
      grpc::ClientContext context;
      raft::PutRequest request;
      raft::PutResponse response;
      request.set_key("alpha");
      request.set_value("1");
      ASSERT_TRUE(stub->Put(&context, request, &response).ok());
      EXPECT_EQ(response.code(), raft::KV_STATUS_CODE_OK);
      EXPECT_GT(response.log_index(), 0);
    }

    {
      grpc::ClientContext context;
      raft::GetRequest request;
      raft::GetResponse response;
      request.set_key("alpha");
      ASSERT_TRUE(stub->Get(&context, request, &response).ok());
      EXPECT_EQ(response.code(), raft::KV_STATUS_CODE_OK);
      EXPECT_TRUE(response.found());
      EXPECT_EQ(response.value(), "1");
    }

    {
      grpc::ClientContext context;
      raft::DeleteRequest request;
      raft::DeleteResponse response;
      request.set_key("alpha");
      ASSERT_TRUE(stub->Delete(&context, request, &response).ok());
      EXPECT_EQ(response.code(), raft::KV_STATUS_CODE_OK);
    }

    {
      grpc::ClientContext context;
      raft::GetRequest request;
      raft::GetResponse response;
      request.set_key("alpha");
      ASSERT_TRUE(stub->Get(&context, request, &response).ok());
      EXPECT_EQ(response.code(), raft::KV_STATUS_CODE_KEY_NOT_FOUND);
      EXPECT_FALSE(response.found());
    }

    {
      grpc::ClientContext context;
      raft::PutRequest request;
      raft::PutResponse response;
      request.set_key("bad|key");
      request.set_value("x");
      ASSERT_TRUE(stub->Put(&context, request, &response).ok());
      EXPECT_EQ(response.code(), raft::KV_STATUS_CODE_INVALID_ARGUMENT);
    }

    {
      grpc::ClientContext context;
      raft::StatusRequest request;
      raft::StatusResponse response;
      ASSERT_TRUE(stub->Status(&context, request, &response).ok());
      EXPECT_EQ(response.node_id(), 1);
      EXPECT_EQ(response.role(), "Leader");
      EXPECT_EQ(response.leader_id(), 1);
      EXPECT_GE(response.commit_index(), 2);
      EXPECT_GE(response.metrics().propose_success_count(), 2);
      EXPECT_GE(response.metrics().rpc_metrics_size(), 1);
    }

    {
      grpc::ClientContext context;
      raft::HealthRequest request;
      raft::HealthResponse response;
      ASSERT_TRUE(stub->Health(&context, request, &response).ok());
      EXPECT_TRUE(response.ok());
      EXPECT_EQ(response.node_id(), 1);
    }

    {
      grpc::ClientContext context;
      raft::MetricsRequest request;
      raft::MetricsResponse response;
      ASSERT_TRUE(stub->Metrics(&context, request, &response).ok());
      EXPECT_EQ(response.node_id(), 1);
      EXPECT_EQ(response.role(), "Leader");
      EXPECT_GE(response.metrics().propose_success_count(), 2);
      bool saw_put_metric = false;
      for (const auto &metric : response.metrics().rpc_metrics())
      {
        if (metric.name() == "kv_put")
        {
          saw_put_metric = true;
          EXPECT_GE(metric.success_count(), 1);
        }
      }
      EXPECT_TRUE(saw_put_metric);
    }
  }

  TEST(RaftKvServiceTest, ThreeNodeFollowerRedirectsWritesAndReadsToLeader)
  {
    const auto root = MakeTestRoot("redirect");
    const int base_port = 55110;
    auto configs = BuildThreeNodeConfigs(root, base_port);
    std::vector<snapshotConfig> snapshots;
    for (int node_id = 1; node_id <= 3; ++node_id)
    {
      snapshots.push_back(MakeSnapshotConfig(root / "snapshots", node_id));
    }

    ThreeNodeRunner runner(configs, snapshots);
    runner.Start();

    const auto leader = runner.WaitForLeader(6s);
    ASSERT_NE(leader, nullptr);
    const auto leader_status = leader->GetStatusSnapshot();

    std::shared_ptr<RaftNode> follower;
    for (const auto &node : runner.nodes())
    {
      if (node != leader)
      {
        follower = node;
        break;
      }
    }
    ASSERT_NE(follower, nullptr);

    auto leader_stub = MakeStub(leader_status.address);
    auto follower_stub = MakeStub(follower->GetStatusSnapshot().address);

    {
      grpc::ClientContext context;
      raft::PutRequest request;
      raft::PutResponse response;
      request.set_key("redirect-key");
      request.set_value("value-1");
      ASSERT_TRUE(leader_stub->Put(&context, request, &response).ok());
      ASSERT_EQ(response.code(), raft::KV_STATUS_CODE_OK);
    }

    {
      grpc::ClientContext context;
      raft::PutRequest request;
      raft::PutResponse response;
      request.set_key("redirect-key");
      request.set_value("value-2");
      ASSERT_TRUE(follower_stub->Put(&context, request, &response).ok());
      EXPECT_EQ(response.code(), raft::KV_STATUS_CODE_NOT_LEADER);
      EXPECT_EQ(response.leader_id(), leader_status.node_id);
      EXPECT_EQ(response.leader_address(), leader_status.address);
    }

    {
      grpc::ClientContext context;
      raft::GetRequest request;
      raft::GetResponse response;
      request.set_key("redirect-key");
      ASSERT_TRUE(follower_stub->Get(&context, request, &response).ok());
      EXPECT_EQ(response.code(), raft::KV_STATUS_CODE_NOT_LEADER);
      EXPECT_EQ(response.leader_id(), leader_status.node_id);
      EXPECT_EQ(response.leader_address(), leader_status.address);
    }

    {
      grpc::ClientContext context;
      raft::StatusRequest request;
      raft::StatusResponse response;
      ASSERT_TRUE(leader_stub->Status(&context, request, &response).ok());
      EXPECT_EQ(response.role(), "Leader");
      EXPECT_EQ(response.peers_size(), 2);
      EXPECT_GE(response.commit_index(), 2);
      for (const auto &peer : response.peers())
      {
        EXPECT_GE(peer.next_index(), peer.match_index());
      }
    }
  }

  TEST(RaftKvServiceTest, DataDirectoryIdentityMismatchIsRejected)
  {
    const auto root = MakeTestRoot("identity");
    NodeConfig config = MakeSingleNodeConfig(root, 55261);
    snapshotConfig snapshot = MakeSnapshotConfig(root / "snapshots", 1);

    auto node = std::make_unique<RaftNode>(config, snapshot);
    ASSERT_NE(node, nullptr);
    node.reset();

    NodeConfig wrong = config;
    wrong.node_id = 2;
    wrong.address = "127.0.0.1:55262";
    EXPECT_THROW((void)std::make_unique<RaftNode>(wrong, snapshot), std::runtime_error);
  }

} // namespace raftdemo
