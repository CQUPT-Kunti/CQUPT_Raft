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
      n1.rpc_deadline = std::chrono::milliseconds(500);
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

      void RestartNode(const std::size_t index)
      {
        if (index >= configs_.size())
        {
          return;
        }

        StopNode(index);
        nodes_[index] = std::make_shared<RaftNode>(configs_[index]);
        nodes_[index]->Start();
        if (index >= wait_threads_.size())
        {
          wait_threads_.resize(index + 1);
        }
        wait_threads_[index] = std::thread([node = nodes_[index]]()
                                           { node->Wait(); });
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

    template <typename Request, typename Response, typename Invoke>
    Response InvokeWriteViaCurrentLeader(
        const std::vector<std::shared_ptr<RaftNode>> &nodes,
        const std::vector<NodeConfig> &configs,
        const Request &request,
        const std::chrono::milliseconds timeout,
        Invoke &&invoke)
    {
      Response response;
      const auto deadline = Clock::now() + timeout;
      while (Clock::now() < deadline)
      {
        auto leader = WaitForSingleLeader(nodes, 1500ms);
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
        const auto status = invoke(stub.get(), &context, request, &response);
        if (status.ok() &&
            response.summary().code() != raft::METADATA_STATUS_CODE_NOT_LEADER &&
            response.summary().code() != raft::METADATA_STATUS_CODE_TIMEOUT)
        {
          return response;
        }

        std::this_thread::sleep_for(50ms);
      }

      return response;
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
      const auto bucket_response = InvokeWriteViaCurrentLeader<
          raft::CreateBucketRequest, raft::CreateBucketResponse>(
          cluster.Nodes(), configs, MakeCreateBucketRequest("create-bucket", bucket), 5s,
          [](raft::MetadataService::Stub *stub,
             grpc::ClientContext *context,
             const raft::CreateBucketRequest &request,
             raft::CreateBucketResponse *response)
          {
            return stub->CreateBucket(context, request, response);
          });
      ASSERT_EQ(bucket_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << bucket_response.summary().message();

      const auto create_committed_response = InvokeWriteViaCurrentLeader<
          raft::CreateObjectRequest, raft::CreateObjectResponse>(
          cluster.Nodes(),
          configs,
          MakeCreateObjectRequest("create-committed", bucket,
                                  "object/committed", "obj-committed"),
          5s,
          [](raft::MetadataService::Stub *stub,
             grpc::ClientContext *context,
             const raft::CreateObjectRequest &request,
             raft::CreateObjectResponse *response)
          {
            return stub->CreateObject(context, request, response);
          });
      ASSERT_EQ(create_committed_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << create_committed_response.summary().message();

      const auto commit_response = InvokeWriteViaCurrentLeader<
          raft::CommitObjectRequest, raft::CommitObjectResponse>(
          cluster.Nodes(),
          configs,
          MakeCommitObjectRequest("commit-committed", bucket,
                                  "object/committed", "obj-committed"),
          5s,
          [](raft::MetadataService::Stub *stub,
             grpc::ClientContext *context,
             const raft::CommitObjectRequest &request,
             raft::CommitObjectResponse *response)
          {
            return stub->CommitObject(context, request, response);
          });
      ASSERT_EQ(commit_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << commit_response.summary().message();

      const auto create_pending_response = InvokeWriteViaCurrentLeader<
          raft::CreateObjectRequest, raft::CreateObjectResponse>(
          cluster.Nodes(),
          configs,
          MakeCreateObjectRequest("create-pending", bucket,
                                  "object/pending", "obj-pending"),
          5s,
          [](raft::MetadataService::Stub *stub,
             grpc::ClientContext *context,
             const raft::CreateObjectRequest &request,
             raft::CreateObjectResponse *response)
          {
            return stub->CreateObject(context, request, response);
          });
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
      auto configs = BuildThreeNodeConfigs(root_, base_port_);
      for (auto &config : configs)
      {
        config.rpc_deadline = 500ms;
      }
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

    TEST_F(MetadataFailoverTest, FollowerWriteReturnsNotLeader)
    {
      const std::string bucket = "not-leader-bucket";
      const auto configs = BuildThreeNodeConfigs(root_, base_port_);
      MetadataCluster cluster(configs);
      cluster.Start();

      auto leader = WaitForSingleLeader(cluster.Nodes(), 8s);
      ASSERT_NE(leader, nullptr) << DescribeCluster(cluster.Nodes());
      std::this_thread::sleep_for(250ms);

      const auto leader_index = FindNodeIndex(cluster.Nodes(), leader);
      ASSERT_LT(leader_index, cluster.Nodes().size());

      std::size_t follower_index = cluster.Nodes().size();
      for (std::size_t i = 0; i < cluster.Nodes().size(); ++i)
      {
        if (i != leader_index)
        {
          follower_index = i;
          break;
        }
      }
      ASSERT_LT(follower_index, cluster.Nodes().size());

      auto follower_stub = MakeMetadataStub(configs[follower_index].address);
      grpc::ClientContext context;
      raft::CreateBucketResponse response;
      ASSERT_TRUE(follower_stub->CreateBucket(
                                  &context,
                                  MakeCreateBucketRequest("write-on-follower", bucket),
                                  &response)
                      .ok());
      EXPECT_EQ(response.summary().code(), raft::METADATA_STATUS_CODE_NOT_LEADER)
          << response.summary().message();
      EXPECT_EQ(response.summary().request_id(), "write-on-follower");
    }

    TEST_F(MetadataFailoverTest, LeaderWriteTimeoutReturnsTimeoutAndSameRequestIdCanRetry)
    {
      const std::string bucket = "timeout-bucket";
      auto configs = BuildThreeNodeConfigs(root_, base_port_);
      for (auto &config : configs)
      {
        config.rpc_deadline = 400ms;
      }

      MetadataCluster cluster(configs);
      cluster.Start();

      auto leader = WaitForSingleLeader(cluster.Nodes(), 8s);
      ASSERT_NE(leader, nullptr) << DescribeCluster(cluster.Nodes());

      const auto leader_index = FindNodeIndex(cluster.Nodes(), leader);
      ASSERT_LT(leader_index, cluster.Nodes().size());
      auto leader_stub = MakeMetadataStub(configs[leader_index].address);

      const auto bucket_response = InvokeWriteViaCurrentLeader<
          raft::CreateBucketRequest, raft::CreateBucketResponse>(
          cluster.Nodes(),
          configs,
          MakeCreateBucketRequest("timeout-create-bucket", bucket),
          5s,
          [](raft::MetadataService::Stub *stub,
             grpc::ClientContext *context,
             const raft::CreateBucketRequest &request,
             raft::CreateBucketResponse *response)
          {
            return stub->CreateBucket(context, request, response);
          });
      ASSERT_EQ(bucket_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << bucket_response.summary().message();

      std::vector<std::size_t> follower_indexes;
      for (std::size_t i = 0; i < cluster.Nodes().size(); ++i)
      {
        if (i != leader_index)
        {
          follower_indexes.push_back(i);
          cluster.StopNode(i);
        }
      }

      const auto create_request =
          MakeCreateObjectRequest("timeout-create", bucket, "object/timeout", "obj-timeout");
      grpc::ClientContext timeout_context;
      raft::CreateObjectResponse timeout_response;
      ASSERT_TRUE(leader_stub->CreateObject(&timeout_context, create_request, &timeout_response).ok());
      EXPECT_EQ(timeout_response.summary().code(), raft::METADATA_STATUS_CODE_TIMEOUT)
          << timeout_response.summary().message();
      EXPECT_EQ(timeout_response.summary().request_id(), "timeout-create");

      for (const auto index : follower_indexes)
      {
        cluster.RestartNode(index);
      }

      raft::CreateObjectResponse retry_response;
      const auto retry_deadline = Clock::now() + 8s;
      while (Clock::now() < retry_deadline)
      {
        auto current_leader = WaitForSingleLeader(cluster.Nodes(), 1500ms);
        if (current_leader == nullptr)
        {
          std::this_thread::sleep_for(100ms);
          continue;
        }
        const auto current_leader_index = FindNodeIndex(cluster.Nodes(), current_leader);
        if (current_leader_index >= configs.size())
        {
          std::this_thread::sleep_for(100ms);
          continue;
        }
        auto current_leader_stub = MakeMetadataStub(configs[current_leader_index].address);
        grpc::ClientContext retry_context;
        const auto retry_status = current_leader_stub->CreateObject(
            &retry_context, create_request, &retry_response);
        if (!retry_status.ok())
        {
          std::this_thread::sleep_for(100ms);
          continue;
        }
        if (retry_response.summary().code() == raft::METADATA_STATUS_CODE_OK ||
            retry_response.summary().code() == raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY)
        {
          break;
        }
        std::this_thread::sleep_for(100ms);
      }

      EXPECT_TRUE(retry_response.summary().code() == raft::METADATA_STATUS_CODE_OK ||
                  retry_response.summary().code() == raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY)
          << retry_response.summary().message();
      EXPECT_EQ(retry_response.summary().request_id(), "timeout-create");

      auto current_leader = WaitForSingleLeader(cluster.Nodes(), 8s);
      ASSERT_NE(current_leader, nullptr) << DescribeCluster(cluster.Nodes());
      const auto current_leader_index = FindNodeIndex(cluster.Nodes(), current_leader);
      ASSERT_LT(current_leader_index, configs.size());
      auto current_leader_stub = MakeMetadataStub(configs[current_leader_index].address);

      grpc::ClientContext commit_context;
      raft::CommitObjectResponse commit_response;
      ASSERT_TRUE(current_leader_stub->CommitObject(
                                      &commit_context,
                                      MakeCommitObjectRequest("timeout-commit", bucket,
                                                              "object/timeout", "obj-timeout"),
                                      &commit_response)
                      .ok());
      ASSERT_EQ(commit_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << commit_response.summary().message();

      const auto head_response =
          HeadViaLeader(cluster.Nodes(), configs, {}, bucket, "object/timeout", 5s);
      ASSERT_EQ(head_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << head_response.summary().message();
      ASSERT_TRUE(head_response.found());
      EXPECT_EQ(head_response.object().object_id(), "obj-timeout");
    }

    TEST_F(MetadataFailoverTest, ConcurrentDuplicateCreateObjectRequestsShareSameLogIndex)
    {
      const std::string bucket = "coalesce-bucket";
      auto configs = BuildThreeNodeConfigs(root_, base_port_);
      for (auto &config : configs)
      {
        config.rpc_deadline = 500ms;
      }

      MetadataCluster cluster(configs);
      cluster.Start();

      auto leader = WaitForSingleLeader(cluster.Nodes(), 8s);
      ASSERT_NE(leader, nullptr) << DescribeCluster(cluster.Nodes());

      const auto leader_index = FindNodeIndex(cluster.Nodes(), leader);
      ASSERT_LT(leader_index, cluster.Nodes().size());
      const std::string leader_address = configs[leader_index].address;
      auto leader_stub = MakeMetadataStub(leader_address);

      grpc::ClientContext bucket_context;
      raft::CreateBucketResponse bucket_response;
      ASSERT_TRUE(leader_stub->CreateBucket(&bucket_context,
                                            MakeCreateBucketRequest("coalesce-create-bucket", bucket),
                                            &bucket_response)
                      .ok());
      ASSERT_EQ(bucket_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << bucket_response.summary().message();

      const auto request =
          MakeCreateObjectRequest("coalesce-create", bucket, "object/coalesce", "obj-coalesce");
      raft::CreateObjectResponse response_a;
      raft::CreateObjectResponse response_b;

      std::thread thread_a([&]()
                           {
                             auto stub = MakeMetadataStub(leader_address);
                             grpc::ClientContext context;
                             EXPECT_TRUE(stub->CreateObject(&context, request, &response_a).ok());
                           });
      std::thread thread_b([&]()
                           {
                             auto stub = MakeMetadataStub(leader_address);
                             grpc::ClientContext context;
                             EXPECT_TRUE(stub->CreateObject(&context, request, &response_b).ok());
                           });
      thread_a.join();
      thread_b.join();

      ASSERT_EQ(response_a.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << response_a.summary().message();
      ASSERT_EQ(response_b.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << response_b.summary().message();
      EXPECT_EQ(response_a.summary().request_id(), "coalesce-create");
      EXPECT_EQ(response_b.summary().request_id(), "coalesce-create");
      EXPECT_EQ(response_a.summary().log_index(), response_b.summary().log_index());
      EXPECT_GT(response_a.summary().log_index(), 0U);
    }

    TEST_F(MetadataFailoverTest, DifferentFingerprintForSameRequestIdReturnsIdempotencyConflict)
    {
      const std::string bucket = "fingerprint-conflict-bucket";
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
      ASSERT_TRUE(leader_stub->CreateBucket(
                                  &bucket_context,
                                  MakeCreateBucketRequest("fingerprint-bucket", bucket),
                                  &bucket_response)
                      .ok());
      ASSERT_EQ(bucket_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << bucket_response.summary().message();

      auto first_request =
          MakeCreateObjectRequest("fingerprint-create", bucket, "object/conflict-a", "obj-conflict-a");
      grpc::ClientContext first_context;
      raft::CreateObjectResponse first_response;
      ASSERT_TRUE(leader_stub->CreateObject(&first_context, first_request, &first_response).ok());
      ASSERT_EQ(first_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << first_response.summary().message();

      auto second_request =
          MakeCreateObjectRequest("fingerprint-create", bucket, "object/conflict-b", "obj-conflict-b");
      second_request.set_size(2048);
      grpc::ClientContext second_context;
      raft::CreateObjectResponse second_response;
      ASSERT_TRUE(leader_stub->CreateObject(&second_context, second_request, &second_response).ok());
      EXPECT_EQ(second_response.summary().code(), raft::METADATA_STATUS_CODE_IDEMPOTENCY_CONFLICT)
          << second_response.summary().message();
      EXPECT_EQ(second_response.summary().request_id(), "fingerprint-create");
    }

  } // namespace
} // namespace raftdemo
