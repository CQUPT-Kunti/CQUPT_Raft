#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "metadata.grpc.pb.h"
#include "raft/common/config.h"
#include "raft/node/raft_node.h"

namespace raftdemo
{
  namespace
  {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    std::filesystem::path MakeTestRoot(const std::string &test_name)
    {
      std::random_device rd;
      return std::filesystem::temp_directory_path() /
             ("metadata_failover_" + test_name + "_" +
              std::to_string(static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count())) +
              "_" + std::to_string(rd()));
    }

    int PickBasePort()
    {
      std::random_device rd;
      const int jitter = static_cast<int>(rd() % 1000);
      const auto tick = static_cast<int>(Clock::now().time_since_epoch().count() % 1000);
      return 43000 + jitter + tick;
    }

    std::vector<NodeConfig> BuildThreeNodeConfigs(const std::filesystem::path &root,
                                                  const int base_port)
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
      n1.data_dir = (root / "node_1_data").string();

      NodeConfig n2 = n1;
      n2.node_id = 2;
      n2.address = "127.0.0.1:" + std::to_string(base_port + 2);
      n2.peers = {
          PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
          PeerConfig{3, "127.0.0.1:" + std::to_string(base_port + 3)},
      };
      n2.data_dir = (root / "node_2_data").string();

      NodeConfig n3 = n1;
      n3.node_id = 3;
      n3.address = "127.0.0.1:" + std::to_string(base_port + 3);
      n3.peers = {
          PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
          PeerConfig{2, "127.0.0.1:" + std::to_string(base_port + 2)},
      };
      n3.data_dir = (root / "node_3_data").string();

      return {n1, n2, n3};
    }

    class MetadataCluster
    {
    public:
      explicit MetadataCluster(std::vector<NodeConfig> configs)
          : configs_(std::move(configs))
      {
      }

      ~MetadataCluster()
      {
        StopAll();
      }

      void Start()
      {
        StopAll();
        nodes_.clear();
        wait_threads_.clear();

        for (const auto &config : configs_)
        {
          nodes_.push_back(std::make_shared<RaftNode>(config));
        }
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

      void StopAll()
      {
        for (const auto &node : nodes_)
        {
          if (node != nullptr)
          {
            node->Stop();
          }
        }
        for (auto &thread : wait_threads_)
        {
          if (thread.joinable())
          {
            thread.join();
          }
        }
        wait_threads_.clear();
      }

      void StopNode(const std::size_t index)
      {
        if (index >= nodes_.size() || nodes_[index] == nullptr)
        {
          return;
        }
        nodes_[index]->Stop();
        if (index < wait_threads_.size() && wait_threads_[index].joinable())
        {
          wait_threads_[index].join();
        }
      }

      const std::vector<std::shared_ptr<RaftNode>> &Nodes() const
      {
        return nodes_;
      }

      const std::vector<NodeConfig> &Configs() const
      {
        return configs_;
      }

    private:
      std::vector<NodeConfig> configs_;
      std::vector<std::shared_ptr<RaftNode>> nodes_;
      std::vector<std::thread> wait_threads_;
    };

    bool IsExcluded(const std::size_t index,
                    const std::vector<std::size_t> &excluded)
    {
      for (const auto excluded_index : excluded)
      {
        if (index == excluded_index)
        {
          return true;
        }
      }
      return false;
    }

    std::shared_ptr<RaftNode> WaitForSingleLeader(
        const std::vector<std::shared_ptr<RaftNode>> &nodes,
        const std::chrono::milliseconds timeout,
        const std::vector<std::size_t> &excluded = {})
    {
      const auto deadline = Clock::now() + timeout;
      while (Clock::now() < deadline)
      {
        std::shared_ptr<RaftNode> leader;
        int leader_count = 0;

        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
          if (IsExcluded(i, excluded) || nodes[i] == nullptr)
          {
            continue;
          }
          if (nodes[i]->GetStatusSnapshot().role == "Leader")
          {
            leader = nodes[i];
            ++leader_count;
          }
        }

        if (leader_count == 1)
        {
          return leader;
        }

        std::this_thread::sleep_for(50ms);
      }
      return nullptr;
    }

    std::size_t FindNodeIndex(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                              const std::shared_ptr<RaftNode> &target)
    {
      for (std::size_t i = 0; i < nodes.size(); ++i)
      {
        if (nodes[i] == target)
        {
          return i;
        }
      }
      return nodes.size();
    }

    std::string DescribeCluster(const std::vector<std::shared_ptr<RaftNode>> &nodes)
    {
      std::string description;
      for (std::size_t i = 0; i < nodes.size(); ++i)
      {
        if (!description.empty())
        {
          description += "\n";
        }
        description += "node[" + std::to_string(i) + "]=";
        description += nodes[i] ? nodes[i]->Describe() : "stopped";
      }
      return description;
    }

    std::unique_ptr<raft::MetadataService::Stub> MakeMetadataStub(const std::string &address)
    {
      auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
      return raft::MetadataService::NewStub(channel);
    }

    raft::CreateBucketRequest MakeCreateBucketRequest(const std::string &request_id,
                                                      const std::string &bucket)
    {
      raft::CreateBucketRequest request;
      request.set_request_id(request_id);
      request.set_bucket(bucket);
      request.set_client_time_unix_ms(1710000000000ULL);
      return request;
    }

    raft::CreateObjectRequest MakeCreateObjectRequest(const std::string &request_id,
                                                      const std::string &bucket,
                                                      const std::string &object_key,
                                                      const std::string &object_id)
    {
      raft::CreateObjectRequest request;
      request.set_request_id(request_id);
      request.set_bucket(bucket);
      request.set_object_key(object_key);
      request.set_object_id(object_id);
      request.set_version(1);
      request.set_size(1024);
      request.set_etag("etag-" + object_id);
      request.set_client_time_unix_ms(1710000001000ULL);
      return request;
    }

    raft::CommitObjectRequest MakeCommitObjectRequest(const std::string &request_id,
                                                      const std::string &bucket,
                                                      const std::string &object_key,
                                                      const std::string &object_id)
    {
      raft::CommitObjectRequest request;
      request.set_request_id(request_id);
      request.set_bucket(bucket);
      request.set_object_key(object_key);
      request.set_object_id(object_id);
      request.set_version(1);
      request.set_size(1024);
      request.set_etag("etag-commit-" + object_id);
      request.set_client_time_unix_ms(1710000002000ULL);

      auto *chunk_a = request.add_chunks();
      chunk_a->set_chunk_id(object_id + "-chunk-a");
      chunk_a->set_offset(0);
      chunk_a->set_size(512);
      chunk_a->add_replica_nodes("node-a");
      chunk_a->add_replica_nodes("node-b");
      chunk_a->set_checksum("checksum-a");

      auto *chunk_b = request.add_chunks();
      chunk_b->set_chunk_id(object_id + "-chunk-b");
      chunk_b->set_offset(512);
      chunk_b->set_size(512);
      chunk_b->add_replica_nodes("node-b");
      chunk_b->add_replica_nodes("node-c");
      chunk_b->set_checksum("checksum-b");
      return request;
    }

    raft::HeadObjectResponse HeadViaLeader(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                                           const std::vector<NodeConfig> &configs,
                                           const std::vector<std::size_t> &excluded,
                                           const std::string &bucket,
                                           const std::string &object_key,
                                           const std::chrono::milliseconds timeout)
    {
      raft::HeadObjectResponse response;
      const auto deadline = Clock::now() + timeout;
      while (Clock::now() < deadline)
      {
        auto leader = WaitForSingleLeader(nodes, 1500ms, excluded);
        if (leader == nullptr)
        {
          std::this_thread::sleep_for(50ms);
          continue;
        }

        const auto leader_index = FindNodeIndex(nodes, leader);
        if (leader_index >= configs.size())
        {
          std::this_thread::sleep_for(50ms);
          continue;
        }

        auto stub = MakeMetadataStub(configs[leader_index].address);
        grpc::ClientContext context;
        raft::HeadObjectRequest request;
        request.set_bucket(bucket);
        request.set_object_key(object_key);
        auto status = stub->HeadObject(&context, request, &response);
        if (status.ok() &&
            response.summary().code() != raft::METADATA_STATUS_CODE_NOT_LEADER)
        {
          return response;
        }
        std::this_thread::sleep_for(50ms);
      }
      return response;
    }

    raft::ListObjectsResponse ListViaLeader(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                                            const std::vector<NodeConfig> &configs,
                                            const std::vector<std::size_t> &excluded,
                                            const std::string &bucket,
                                            const std::string &prefix,
                                            const std::chrono::milliseconds timeout)
    {
      raft::ListObjectsResponse response;
      const auto deadline = Clock::now() + timeout;
      while (Clock::now() < deadline)
      {
        auto leader = WaitForSingleLeader(nodes, 1500ms, excluded);
        if (leader == nullptr)
        {
          std::this_thread::sleep_for(50ms);
          continue;
        }

        const auto leader_index = FindNodeIndex(nodes, leader);
        if (leader_index >= configs.size())
        {
          std::this_thread::sleep_for(50ms);
          continue;
        }

        auto stub = MakeMetadataStub(configs[leader_index].address);
        grpc::ClientContext context;
        raft::ListObjectsRequest request;
        request.set_bucket(bucket);
        request.set_prefix(prefix);
        auto status = stub->ListObjects(&context, request, &response);
        if (status.ok() &&
            response.summary().code() != raft::METADATA_STATUS_CODE_NOT_LEADER)
        {
          return response;
        }
        std::this_thread::sleep_for(50ms);
      }
      return response;
    }

    class MetadataFailoverTest : public ::testing::Test
    {
    protected:
      void SetUp() override
      {
        const auto *test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        root_ = MakeTestRoot(std::string(test_info->test_suite_name()) + "_" +
                             test_info->name());
        base_port_ = PickBasePort();

        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_, ec);
        ASSERT_FALSE(ec) << "failed to create test root: " << ec.message();
      }

      void TearDown() override
      {
        std::error_code ec;
        if (!HasFailure())
        {
          std::filesystem::remove_all(root_, ec);
        }
        else
        {
          std::cout << "preserved test root: " << root_.string() << "\n";
        }
      }

      std::filesystem::path root_;
      int base_port_{0};
    };

    TEST_F(MetadataFailoverTest, NewLeaderKeepsCommittedVisibleAndPendingHidden)
    {
      const std::string bucket = "failover-bucket";
      const auto configs = BuildThreeNodeConfigs(root_, base_port_);
      MetadataCluster cluster(configs);
      cluster.Start();

      auto leader = WaitForSingleLeader(cluster.Nodes(), 8s);
      ASSERT_NE(leader, nullptr) << DescribeCluster(cluster.Nodes());

      const auto leader_index = FindNodeIndex(cluster.Nodes(), leader);
      ASSERT_LT(leader_index, cluster.Nodes().size());
      auto leader_stub = MakeMetadataStub(configs[leader_index].address);

      grpc::ClientContext bucket_context;
      raft::CreateBucketResponse bucket_response;
      ASSERT_TRUE(leader_stub->CreateBucket(&bucket_context,
                                            MakeCreateBucketRequest("create-bucket", bucket),
                                            &bucket_response)
                      .ok());
      ASSERT_EQ(bucket_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << bucket_response.summary().message();

      grpc::ClientContext create_committed_context;
      raft::CreateObjectResponse create_committed_response;
      ASSERT_TRUE(leader_stub->CreateObject(
                              &create_committed_context,
                              MakeCreateObjectRequest("create-committed", bucket,
                                                      "object/committed", "obj-committed"),
                              &create_committed_response)
                      .ok());
      ASSERT_EQ(create_committed_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << create_committed_response.summary().message();

      grpc::ClientContext commit_context;
      raft::CommitObjectResponse commit_response;
      ASSERT_TRUE(leader_stub->CommitObject(
                              &commit_context,
                              MakeCommitObjectRequest("commit-committed", bucket,
                                                      "object/committed", "obj-committed"),
                              &commit_response)
                      .ok());
      ASSERT_EQ(commit_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << commit_response.summary().message();

      grpc::ClientContext create_pending_context;
      raft::CreateObjectResponse create_pending_response;
      ASSERT_TRUE(leader_stub->CreateObject(
                              &create_pending_context,
                              MakeCreateObjectRequest("create-pending", bucket,
                                                      "object/pending", "obj-pending"),
                              &create_pending_response)
                      .ok());
      ASSERT_EQ(create_pending_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << create_pending_response.summary().message();

      const auto baseline_head =
          HeadViaLeader(cluster.Nodes(), configs, {}, bucket, "object/committed", 5s);
      ASSERT_EQ(baseline_head.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << baseline_head.summary().message();
      ASSERT_TRUE(baseline_head.found());
      EXPECT_EQ(baseline_head.object().object_key(), "object/committed");
      EXPECT_EQ(baseline_head.object().state(), raft::METADATA_OBJECT_STATE_COMMITTED);

      cluster.StopNode(leader_index);
      const std::vector<std::size_t> excluded{leader_index};

      auto new_leader = WaitForSingleLeader(cluster.Nodes(), 10s, excluded);
      ASSERT_NE(new_leader, nullptr)
          << "no new leader after stopping old leader\n"
          << DescribeCluster(cluster.Nodes());
      EXPECT_NE(new_leader, leader);

      const auto committed_head =
          HeadViaLeader(cluster.Nodes(), configs, excluded, bucket, "object/committed", 8s);
      ASSERT_EQ(committed_head.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << committed_head.summary().message();
      ASSERT_TRUE(committed_head.found());
      EXPECT_EQ(committed_head.object().object_key(), "object/committed");
      EXPECT_EQ(committed_head.object().state(), raft::METADATA_OBJECT_STATE_COMMITTED);

      const auto pending_head =
          HeadViaLeader(cluster.Nodes(), configs, excluded, bucket, "object/pending", 5s);
      EXPECT_EQ(pending_head.summary().code(), raft::METADATA_STATUS_CODE_NOT_FOUND)
          << pending_head.summary().message();
      EXPECT_FALSE(pending_head.found());

      const auto list_response =
          ListViaLeader(cluster.Nodes(), configs, excluded, bucket, "object/", 5s);
      ASSERT_EQ(list_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << list_response.summary().message();
      ASSERT_EQ(list_response.objects_size(), 1);
      EXPECT_EQ(list_response.objects(0).object_key(), "object/committed");
      EXPECT_EQ(list_response.objects(0).state(), raft::METADATA_OBJECT_STATE_COMMITTED);
    }

    TEST_F(MetadataFailoverTest, SameCommitRequestIdCanBeRetriedOnNewLeader)
    {
      const std::string bucket = "retry-bucket";
      const auto configs = BuildThreeNodeConfigs(root_, base_port_);
      MetadataCluster cluster(configs);
      cluster.Start();

      auto leader = WaitForSingleLeader(cluster.Nodes(), 8s);
      ASSERT_NE(leader, nullptr) << DescribeCluster(cluster.Nodes());

      const auto leader_index = FindNodeIndex(cluster.Nodes(), leader);
      ASSERT_LT(leader_index, cluster.Nodes().size());
      auto leader_stub = MakeMetadataStub(configs[leader_index].address);

      grpc::ClientContext bucket_context;
      raft::CreateBucketResponse bucket_response;
      ASSERT_TRUE(leader_stub->CreateBucket(&bucket_context,
                                            MakeCreateBucketRequest("retry-create-bucket", bucket),
                                            &bucket_response)
                      .ok());
      ASSERT_EQ(bucket_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << bucket_response.summary().message();

      const auto create_request =
          MakeCreateObjectRequest("create-retry", bucket, "object/retry", "obj-retry");
      const auto commit_request =
          MakeCommitObjectRequest("commit-retry", bucket, "object/retry", "obj-retry");

      grpc::ClientContext create_context;
      raft::CreateObjectResponse create_response;
      ASSERT_TRUE(leader_stub->CreateObject(&create_context, create_request, &create_response).ok());
      ASSERT_EQ(create_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << create_response.summary().message();

      grpc::ClientContext commit_context;
      raft::CommitObjectResponse commit_response;
      ASSERT_TRUE(leader_stub->CommitObject(&commit_context, commit_request, &commit_response).ok());
      ASSERT_EQ(commit_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << commit_response.summary().message();

      cluster.StopNode(leader_index);
      const std::vector<std::size_t> excluded{leader_index};

      auto new_leader = WaitForSingleLeader(cluster.Nodes(), 10s, excluded);
      ASSERT_NE(new_leader, nullptr)
          << "no new leader after failover\n"
          << DescribeCluster(cluster.Nodes());

      const auto new_leader_index = FindNodeIndex(cluster.Nodes(), new_leader);
      ASSERT_LT(new_leader_index, cluster.Nodes().size());
      auto new_leader_stub = MakeMetadataStub(configs[new_leader_index].address);

      grpc::ClientContext retry_context;
      raft::CommitObjectResponse retry_response;
      ASSERT_TRUE(new_leader_stub->CommitObject(&retry_context, commit_request, &retry_response).ok());
      EXPECT_TRUE(retry_response.summary().code() == raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY ||
                  retry_response.summary().code() == raft::METADATA_STATUS_CODE_OK)
          << retry_response.summary().message();
      EXPECT_EQ(retry_response.summary().state(), raft::METADATA_OBJECT_STATE_COMMITTED);

      const auto head_response =
          HeadViaLeader(cluster.Nodes(), configs, excluded, bucket, "object/retry", 5s);
      ASSERT_EQ(head_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << head_response.summary().message();
      ASSERT_TRUE(head_response.found());
      EXPECT_EQ(head_response.object().object_key(), "object/retry");
      EXPECT_EQ(head_response.object().chunks_size(), 2);

      const auto list_response =
          ListViaLeader(cluster.Nodes(), configs, excluded, bucket, "object/", 5s);
      ASSERT_EQ(list_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << list_response.summary().message();
      ASSERT_EQ(list_response.objects_size(), 1);
      EXPECT_EQ(list_response.objects(0).object_key(), "object/retry");
    }

  } // namespace
} // namespace raftdemo
