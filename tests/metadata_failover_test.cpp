#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "metadata.grpc.pb.h"
#include "metadata_raft_test_utils.h"
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

    std::optional<std::uint64_t> ExtractUnsignedDiagnosticValue(
        const std::string &text,
        const std::string &key)
    {
      const std::size_t begin = text.find(key);
      if (begin == std::string::npos)
      {
        return std::nullopt;
      }

      const std::size_t value_begin = begin + key.size();
      std::size_t value_end = value_begin;
      while (value_end < text.size() &&
             std::isdigit(static_cast<unsigned char>(text[value_end])) != 0)
      {
        ++value_end;
      }
      if (value_end == value_begin)
      {
        return std::nullopt;
      }

      try
      {
        return static_cast<std::uint64_t>(
            std::stoull(text.substr(value_begin, value_end - value_begin)));
      }
      catch (...)
      {
        return std::nullopt;
      }
    }

    std::optional<std::size_t> ExtractBracketListEntryCount(
        const std::string &text,
        const std::string &key)
    {
      const std::size_t begin = text.find(key);
      if (begin == std::string::npos)
      {
        return std::nullopt;
      }

      const std::size_t list_begin = begin + key.size();
      const std::size_t list_end = text.find(']', list_begin);
      if (list_end == std::string::npos)
      {
        return std::nullopt;
      }
      if (list_end == list_begin)
      {
        return 0U;
      }

      std::size_t count = 1U;
      for (std::size_t index = list_begin; index < list_end; ++index)
      {
        if (text[index] == ',')
        {
          ++count;
        }
      }
      return count;
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

    NodeConfig BuildDetachedLearnerLikeConfig(const std::filesystem::path &root,
                                              const int learner_id,
                                              const int learner_port)
    {
      NodeConfig learner;
      learner.node_id = learner_id;
      learner.address = "127.0.0.1:" + std::to_string(learner_port);
      learner.election_timeout_min = 300ms;
      learner.election_timeout_max = 600ms;
      learner.heartbeat_interval = 80ms;
      learner.rpc_deadline = 500ms;
      learner.data_dir = (root / ("learner_" + std::to_string(learner_id))).string();
      return learner;
    }

    void WriteStructuredLearnerIdentity(const NodeConfig &learner)
    {
      std::error_code ec;
      std::filesystem::create_directories(learner.data_dir, ec);
      ASSERT_FALSE(ec) << ec.message();

      std::ofstream out(std::filesystem::path(learner.data_dir) / "node.identity",
                        std::ios::trunc);
      ASSERT_TRUE(out.is_open());
      out << "membership_state=learner\n";
      out.flush();
      ASSERT_TRUE(static_cast<bool>(out));
    }

    std::string DescribeCommittedMembershipSummary(
        const CommittedMembershipQuorumSummary &summary)
    {
      std::ostringstream oss;
      oss << "commit_index=" << summary.committed_log_index
          << ", term=" << summary.committed_term
          << ", voters=[";
      for (std::size_t index = 0; index < summary.voter_ids.size(); ++index)
      {
        if (index != 0)
        {
          oss << ",";
        }
        oss << summary.voter_ids[index];
      }
      oss << "], learners=[";
      for (std::size_t index = 0; index < summary.learner_ids.size(); ++index)
      {
        if (index != 0)
        {
          oss << ",";
        }
        oss << summary.learner_ids[index];
      }
      oss << "], voter_count=" << summary.voter_count
          << ", learner_count=" << summary.learner_count
          << ", quorum=" << summary.quorum_size
          << ", local_role=" << static_cast<int>(summary.local_role);
      return oss.str();
    }

    std::string DescribeRuntimeMembershipSummary(const RuntimeMembershipSummary &summary)
    {
      std::ostringstream oss;
      oss << "commit_index=" << summary.committed_log_index
          << ", term=" << summary.committed_term
          << ", voters=[";
      for (std::size_t index = 0; index < summary.voter_ids.size(); ++index)
      {
        if (index != 0)
        {
          oss << ",";
        }
        oss << summary.voter_ids[index];
      }
      oss << "], learners=[";
      for (std::size_t index = 0; index < summary.learner_ids.size(); ++index)
      {
        if (index != 0)
        {
          oss << ",";
        }
        oss << summary.learner_ids[index];
      }
      oss << "], voter_count=" << summary.voter_count
          << ", learner_count=" << summary.learner_count
          << ", committed_voter_quorum=" << summary.committed_voter_quorum_size
          << ", local_role=" << static_cast<int>(summary.local_role);
      return oss.str();
    }

    void ExpectNoCommittedFourVoterDiagnostic(const std::string &diagnostic,
                                              const std::string &context)
    {
      if (diagnostic.empty())
      {
        return;
      }

      if (const auto voter_count = ExtractUnsignedDiagnosticValue(
              diagnostic,
              "committed_voter_count=");
          voter_count.has_value())
      {
        EXPECT_NE(*voter_count, 4U)
            << context << "; diagnostic=" << diagnostic;
      }

      if (const auto voter_ids = ExtractBracketListEntryCount(
              diagnostic,
              "committed_voter_ids=[");
          voter_ids.has_value())
      {
        EXPECT_NE(*voter_ids, 4U)
            << context << "; diagnostic=" << diagnostic;
      }
    }

    void ExpectCommittedThreeVoterBoundary(const std::shared_ptr<RaftNode> &node,
                                           const std::string &context)
    {
      ASSERT_NE(node, nullptr);
      const auto summary = node->GetCommittedMembershipQuorumSummary();
      EXPECT_EQ(summary.voter_ids, std::vector<int>({1, 2, 3}))
          << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
      EXPECT_TRUE(summary.learner_ids.empty())
          << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
      EXPECT_EQ(summary.voter_count, 3U)
          << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
      EXPECT_EQ(summary.learner_count, 0U)
          << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
      EXPECT_EQ(summary.quorum_size, 2U)
          << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
      EXPECT_NE(summary.voter_count, 4U)
          << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
    }

    void ExpectRuntimeStillTreatsLearnersAsNonVoters(
        const RuntimeMembershipSummary &summary,
        const std::vector<int> &candidate_learner_ids,
        const std::string &context)
    {
      EXPECT_EQ(summary.voter_ids, std::vector<int>({1, 2, 3}))
          << context << "; runtime=" << DescribeRuntimeMembershipSummary(summary);
      EXPECT_EQ(summary.voter_count, 3U)
          << context << "; runtime=" << DescribeRuntimeMembershipSummary(summary);
      EXPECT_EQ(summary.committed_voter_quorum_size, 2U)
          << context << "; runtime=" << DescribeRuntimeMembershipSummary(summary);
      EXPECT_NE(summary.voter_count, 4U)
          << context << "; runtime=" << DescribeRuntimeMembershipSummary(summary);
      for (const int learner_id : candidate_learner_ids)
      {
        EXPECT_TRUE(std::find(summary.voter_ids.begin(),
                              summary.voter_ids.end(),
                              learner_id) == summary.voter_ids.end())
            << context << "; learner_id=" << learner_id
            << " must not be restored as voter; runtime="
            << DescribeRuntimeMembershipSummary(summary);
      }
    }

    raft::JoinMetadataClusterRequest MakeJoinMetadataClusterRequest(
        const std::string &request_id,
        const std::string &cluster_id,
        const std::string &node_id,
        const std::int32_t candidate_raft_id,
        const std::uint16_t candidate_client_port,
        const std::uint16_t candidate_raft_port)
    {
      raft::JoinMetadataClusterRequest request;
      request.set_request_id(request_id);
      request.set_cluster_id(cluster_id);
      request.set_node_id(node_id);
      request.set_candidate_raft_id(candidate_raft_id);
      request.set_candidate_client_address("127.0.0.1:" +
                                           std::to_string(candidate_client_port));
      request.set_candidate_raft_address("127.0.0.1:" +
                                         std::to_string(candidate_raft_port));
      request.set_candidate_incarnation_id(node_id + ":boot:1710000000");
      request.set_candidate_sequence(1);
      request.set_persistent_generation(1);
      request.set_data_dir_fingerprint("fingerprint-" + node_id);
      request.set_local_state_hint(
          raft::JOIN_METADATA_CANDIDATE_STATE_HINT_CANDIDATE);
      request.set_observed_view_node_id("view-1");
      request.set_observed_time_unix_ms(1710000000123ULL);
      request.set_observed_metadata_endpoint("127.0.0.1:" +
                                             std::to_string(candidate_client_port));
      return request;
    }

    grpc::Status JoinMetadataClusterViaAddress(
        const std::string &address,
        const raft::JoinMetadataClusterRequest &request,
        raft::JoinMetadataClusterResponse *response)
    {
      auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
      auto stub = raft::MetadataService::NewStub(channel);
      grpc::ClientContext context;
      return stub->JoinMetadataCluster(&context, request, response);
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

    class StandaloneNodeRunner
    {
    public:
      explicit StandaloneNodeRunner(std::shared_ptr<RaftNode> node)
          : node_(std::move(node))
      {
      }

      ~StandaloneNodeRunner()
      {
        Stop();
      }

      void Start()
      {
        if (!node_ || thread_.joinable())
        {
          return;
        }
        thread_ = std::thread([node = node_]()
                              {
                                node->Start();
                                node->Wait();
                              });
      }

      void Stop()
      {
        if (node_ != nullptr)
        {
          node_->Stop();
        }
        if (thread_.joinable())
        {
          thread_.join();
        }
      }

      const std::shared_ptr<RaftNode> &Node() const
      {
        return node_;
      }

    private:
      std::shared_ptr<RaftNode> node_;
      std::thread thread_;
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

    bool WaitForLearnerReplicationProgress(const std::shared_ptr<RaftNode> &leader,
                                           const int learner_raft_id,
                                           const std::uint64_t minimum_match_index,
                                           const std::chrono::milliseconds timeout,
                                           RuntimeMembershipEntry *learner_entry,
                                           std::string *diagnostics)
    {
      const auto deadline = Clock::now() + timeout;
      std::string last_snapshot;

      while (Clock::now() < deadline)
      {
        const auto summary =
            leader != nullptr ? leader->GetRuntimeMembershipSummary()
                              : RuntimeMembershipSummary{};
        last_snapshot = DescribeRuntimeMembershipSummary(summary);
        for (const auto &entry : summary.learner_entries)
        {
          if (entry.raft_id != learner_raft_id)
          {
            continue;
          }
          if (entry.match_index >= minimum_match_index &&
              entry.next_index >= entry.match_index)
          {
            if (learner_entry != nullptr)
            {
              *learner_entry = entry;
            }
            if (diagnostics != nullptr)
            {
              *diagnostics = last_snapshot;
            }
            return true;
          }
        }

        std::this_thread::sleep_for(50ms);
      }

      if (diagnostics != nullptr)
      {
        *diagnostics = last_snapshot;
      }
      return false;
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

      const auto create_response = InvokeWriteViaCurrentLeader<
          raft::CreateObjectRequest, raft::CreateObjectResponse>(
          cluster.Nodes(), configs, create_request, 5s,
          [](raft::MetadataService::Stub *stub,
             grpc::ClientContext *context,
             const raft::CreateObjectRequest &request,
             raft::CreateObjectResponse *response)
          {
            return stub->CreateObject(context, request, response);
          });
      ASSERT_EQ(create_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << create_response.summary().message();

      const auto commit_response = InvokeWriteViaCurrentLeader<
          raft::CommitObjectRequest, raft::CommitObjectResponse>(
          cluster.Nodes(), configs, commit_request, 5s,
          [](raft::MetadataService::Stub *stub,
             grpc::ClientContext *context,
             const raft::CommitObjectRequest &request,
             raft::CommitObjectResponse *response)
          {
            return stub->CommitObject(context, request, response);
          });
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

    TEST_F(MetadataFailoverTest, FollowerHeadAndListReturnNotLeader)
    {
      const std::string bucket = "follower-read-bucket";
      const auto configs = BuildThreeNodeConfigs(root_, base_port_);
      MetadataCluster cluster(configs);
      cluster.Start();

      auto leader = WaitForSingleLeader(cluster.Nodes(), 8s);
      ASSERT_NE(leader, nullptr) << DescribeCluster(cluster.Nodes());

      const auto bucket_response = InvokeWriteViaCurrentLeader<
          raft::CreateBucketRequest, raft::CreateBucketResponse>(
          cluster.Nodes(), configs, MakeCreateBucketRequest("follower-read-bucket-create", bucket), 5s,
          [](raft::MetadataService::Stub *stub,
             grpc::ClientContext *context,
             const raft::CreateBucketRequest &request,
             raft::CreateBucketResponse *response)
          {
            return stub->CreateBucket(context, request, response);
          });
      ASSERT_EQ(bucket_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << bucket_response.summary().message();

      const auto create_response = InvokeWriteViaCurrentLeader<
          raft::CreateObjectRequest, raft::CreateObjectResponse>(
          cluster.Nodes(),
          configs,
          MakeCreateObjectRequest("follower-read-create", bucket,
                                  "object/readable", "obj-readable"),
          5s,
          [](raft::MetadataService::Stub *stub,
             grpc::ClientContext *context,
             const raft::CreateObjectRequest &request,
             raft::CreateObjectResponse *response)
          {
            return stub->CreateObject(context, request, response);
          });
      ASSERT_EQ(create_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << create_response.summary().message();

      const auto commit_response = InvokeWriteViaCurrentLeader<
          raft::CommitObjectRequest, raft::CommitObjectResponse>(
          cluster.Nodes(),
          configs,
          MakeCommitObjectRequest("follower-read-commit", bucket,
                                  "object/readable", "obj-readable"),
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
      grpc::ClientContext head_context;
      raft::HeadObjectRequest head_request;
      head_request.set_bucket(bucket);
      head_request.set_object_key("object/readable");
      raft::HeadObjectResponse head_response;
      ASSERT_TRUE(follower_stub->HeadObject(&head_context, head_request, &head_response).ok());
      EXPECT_EQ(head_response.summary().code(), raft::METADATA_STATUS_CODE_NOT_LEADER)
          << head_response.summary().message();
      EXPECT_FALSE(head_response.found());

      grpc::ClientContext list_context;
      raft::ListObjectsRequest list_request;
      list_request.set_bucket(bucket);
      list_request.set_prefix("object/");
      raft::ListObjectsResponse list_response;
      ASSERT_TRUE(follower_stub->ListObjects(&list_context, list_request, &list_response).ok());
      EXPECT_EQ(list_response.summary().code(), raft::METADATA_STATUS_CODE_NOT_LEADER)
          << list_response.summary().message();
      EXPECT_EQ(list_response.objects_size(), 0);
    }

    TEST_F(MetadataFailoverTest, LeaderHeadAndListInvalidRequestReturnInvalidArgument)
    {
      const auto configs = BuildThreeNodeConfigs(root_, base_port_);
      MetadataCluster cluster(configs);
      cluster.Start();

      auto leader = WaitForSingleLeader(cluster.Nodes(), 8s);
      ASSERT_NE(leader, nullptr) << DescribeCluster(cluster.Nodes());

      const auto leader_index = FindNodeIndex(cluster.Nodes(), leader);
      ASSERT_LT(leader_index, cluster.Nodes().size());
      auto leader_stub = MakeMetadataStub(configs[leader_index].address);

      grpc::ClientContext invalid_head_context;
      raft::HeadObjectRequest invalid_head_request;
      invalid_head_request.set_bucket("");
      invalid_head_request.set_object_key("");
      raft::HeadObjectResponse invalid_head_response;
      ASSERT_TRUE(leader_stub->HeadObject(
                              &invalid_head_context,
                              invalid_head_request,
                              &invalid_head_response)
                      .ok());
      EXPECT_EQ(invalid_head_response.summary().code(),
                raft::METADATA_STATUS_CODE_INVALID_ARGUMENT)
          << invalid_head_response.summary().message();
      EXPECT_FALSE(invalid_head_response.found());

      grpc::ClientContext invalid_list_context;
      raft::ListObjectsRequest invalid_list_request;
      invalid_list_request.set_bucket("");
      raft::ListObjectsResponse invalid_list_response;
      ASSERT_TRUE(leader_stub->ListObjects(
                              &invalid_list_context,
                              invalid_list_request,
                              &invalid_list_response)
                      .ok());
      EXPECT_EQ(invalid_list_response.summary().code(),
                raft::METADATA_STATUS_CODE_INVALID_ARGUMENT)
          << invalid_list_response.summary().message();
      EXPECT_EQ(invalid_list_response.objects_size(), 0);
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

      leader = WaitForSingleLeader(cluster.Nodes(), 2s);
      ASSERT_NE(leader, nullptr) << DescribeCluster(cluster.Nodes());
      const auto current_leader_index = FindNodeIndex(cluster.Nodes(), leader);
      ASSERT_LT(current_leader_index, cluster.Nodes().size());
      leader_stub = MakeMetadataStub(configs[current_leader_index].address);

      std::vector<std::size_t> follower_indexes;
      for (std::size_t i = 0; i < cluster.Nodes().size(); ++i)
      {
        if (i != current_leader_index)
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
      const auto retry_leader_index = FindNodeIndex(cluster.Nodes(), current_leader);
      ASSERT_LT(retry_leader_index, configs.size());
      auto current_leader_stub = MakeMetadataStub(configs[retry_leader_index].address);

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

      raftdemo::test::MetadataRecoveryExpectation expectation;
      expectation.bucket = bucket;
      expectation.objects = {{
          "object/timeout",
          "obj-timeout",
          2U,
          false,
      }};
      expectation.visible_keys = {"object/timeout"};
      expectation.expected_request_count = 3U;
      expectation.expected_tombstone_count = 0U;
      expectation.expected_last_applied_index = commit_response.summary().log_index();
      ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
          cluster.Nodes(), expectation, 10s));
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

      const auto commit_response = InvokeWriteViaCurrentLeader<
          raft::CommitObjectRequest, raft::CommitObjectResponse>(
          cluster.Nodes(),
          configs,
          MakeCommitObjectRequest("coalesce-commit", bucket,
                                  "object/coalesce", "obj-coalesce"),
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

      raftdemo::test::MetadataRecoveryExpectation expectation;
      expectation.bucket = bucket;
      expectation.objects = {{
          "object/coalesce",
          "obj-coalesce",
          2U,
          false,
      }};
      expectation.visible_keys = {"object/coalesce"};
      expectation.expected_request_count = 3U;
      expectation.expected_tombstone_count = 0U;
      expectation.expected_last_applied_index = commit_response.summary().log_index();
      ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
          cluster.Nodes(), expectation, 10s));

      const auto head_response =
          HeadViaLeader(cluster.Nodes(), configs, {}, bucket, "object/coalesce", 5s);
      ASSERT_EQ(head_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << head_response.summary().message();
      ASSERT_TRUE(head_response.found());
      EXPECT_EQ(head_response.object().object_id(), "obj-coalesce");

      const auto list_response =
          ListViaLeader(cluster.Nodes(), configs, {}, bucket, "object/", 5s);
      ASSERT_EQ(list_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << list_response.summary().message();
      ASSERT_EQ(list_response.objects_size(), 1);
      EXPECT_EQ(list_response.objects(0).object_key(), "object/coalesce");
    }

    TEST_F(MetadataFailoverTest,
           ConcurrentConflictingCreateObjectRequestsReturnConflictAndKeepCommittedStateConsistent)
    {
      const std::string bucket = "fingerprint-concurrent-bucket";
      const auto configs = BuildThreeNodeConfigs(root_, base_port_);
      MetadataCluster cluster(configs);
      cluster.Start();

      auto leader = WaitForSingleLeader(cluster.Nodes(), 8s);
      ASSERT_NE(leader, nullptr) << DescribeCluster(cluster.Nodes());

      const auto leader_index = FindNodeIndex(cluster.Nodes(), leader);
      ASSERT_LT(leader_index, cluster.Nodes().size());
      const std::string leader_address = configs[leader_index].address;

      const auto bucket_response = InvokeWriteViaCurrentLeader<
          raft::CreateBucketRequest, raft::CreateBucketResponse>(
          cluster.Nodes(),
          configs,
          MakeCreateBucketRequest("fingerprint-concurrent-bucket-create", bucket),
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

      auto request_a = MakeCreateObjectRequest(
          "fingerprint-concurrent-create", bucket, "object/conflict", "obj-conflict");
      auto request_b = request_a;
      request_b.set_size(2048);
      request_b.set_etag("etag-conflict-overwrite");

      raft::CreateObjectResponse response_a;
      raft::CreateObjectResponse response_b;
      std::thread thread_a([&]()
                           {
                             auto stub = MakeMetadataStub(leader_address);
                             grpc::ClientContext context;
                             EXPECT_TRUE(stub->CreateObject(&context, request_a, &response_a).ok());
                           });
      std::thread thread_b([&]()
                           {
                             auto stub = MakeMetadataStub(leader_address);
                             grpc::ClientContext context;
                             EXPECT_TRUE(stub->CreateObject(&context, request_b, &response_b).ok());
                           });
      thread_a.join();
      thread_b.join();

      int ok_count = 0;
      int conflict_count = 0;
      for (const auto *response : {&response_a, &response_b})
      {
        if (response->summary().code() == raft::METADATA_STATUS_CODE_OK)
        {
          ++ok_count;
          continue;
        }
        if (response->summary().code() == raft::METADATA_STATUS_CODE_IDEMPOTENCY_CONFLICT)
        {
          ++conflict_count;
          continue;
        }
        FAIL() << "unexpected create response code=" << response->summary().code()
               << " message=" << response->summary().message();
      }
      EXPECT_EQ(ok_count, 1);
      EXPECT_EQ(conflict_count, 1);

      const auto commit_response = InvokeWriteViaCurrentLeader<
          raft::CommitObjectRequest, raft::CommitObjectResponse>(
          cluster.Nodes(),
          configs,
          MakeCommitObjectRequest("fingerprint-concurrent-commit", bucket,
                                  "object/conflict", "obj-conflict"),
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

      raftdemo::test::MetadataRecoveryExpectation expectation;
      expectation.bucket = bucket;
      expectation.objects = {{
          "object/conflict",
          "obj-conflict",
          2U,
          false,
      }};
      expectation.visible_keys = {"object/conflict"};
      expectation.expected_request_count = 3U;
      expectation.expected_tombstone_count = 0U;
      expectation.expected_last_applied_index = commit_response.summary().log_index();
      ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
          cluster.Nodes(), expectation, 10s));

      const auto head_response =
          HeadViaLeader(cluster.Nodes(), configs, {}, bucket, "object/conflict", 5s);
      ASSERT_EQ(head_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << head_response.summary().message();
      ASSERT_TRUE(head_response.found());
      EXPECT_EQ(head_response.object().object_id(), "obj-conflict");

      const auto list_response =
          ListViaLeader(cluster.Nodes(), configs, {}, bucket, "object/", 5s);
      ASSERT_EQ(list_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << list_response.summary().message();
      ASSERT_EQ(list_response.objects_size(), 1);
      EXPECT_EQ(list_response.objects(0).object_key(), "object/conflict");
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

    TEST_F(MetadataFailoverTest,
           LeaderFailureDuringIncompleteBatchPromoteDoesNotLeavePartialCommittedMembership)
    {
      constexpr const char *kBucket = "failover-batch-promote-bucket";
      constexpr const char *kObjectKey = "object/failover-batch-promote";
      constexpr const char *kObjectId = "obj-failover-batch-promote";
      constexpr const char *kClusterId = "cluster-t080-batch-promote-failover";
      constexpr const char *kFirstLearnerNodeId = "meta-failover-learner-a-t080";
      constexpr const char *kSecondLearnerNodeId = "meta-failover-learner-b-t080";
      constexpr std::int32_t kFirstLearnerRaftId = 280;
      constexpr std::int32_t kSecondLearnerRaftId = 281;

      const auto configs = BuildThreeNodeConfigs(root_, base_port_);
      MetadataCluster cluster(configs);
      cluster.Start();

      auto leader = WaitForSingleLeader(cluster.Nodes(), 8s);
      ASSERT_NE(leader, nullptr) << DescribeCluster(cluster.Nodes());

      const auto bucket_response = InvokeWriteViaCurrentLeader<
          raft::CreateBucketRequest, raft::CreateBucketResponse>(
          cluster.Nodes(),
          configs,
          MakeCreateBucketRequest("t080-create-bucket", kBucket),
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

      const auto create_response = InvokeWriteViaCurrentLeader<
          raft::CreateObjectRequest, raft::CreateObjectResponse>(
          cluster.Nodes(),
          configs,
          MakeCreateObjectRequest("t080-create-object",
                                  kBucket,
                                  kObjectKey,
                                  kObjectId),
          5s,
          [](raft::MetadataService::Stub *stub,
             grpc::ClientContext *context,
             const raft::CreateObjectRequest &request,
             raft::CreateObjectResponse *response)
          {
            return stub->CreateObject(context, request, response);
          });
      ASSERT_EQ(create_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << create_response.summary().message();

      const auto commit_response = InvokeWriteViaCurrentLeader<
          raft::CommitObjectRequest, raft::CommitObjectResponse>(
          cluster.Nodes(),
          configs,
          MakeCommitObjectRequest("t080-commit-object",
                                  kBucket,
                                  kObjectKey,
                                  kObjectId),
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
      ASSERT_GT(commit_response.summary().log_index(), 0U);

      for (const auto &node : cluster.Nodes())
      {
        ExpectCommittedThreeVoterBoundary(node, "initial 3-voter committed state");
      }

      const auto learners_root = root_ / "t080_detached_learners";
      const auto first_learner_config = BuildDetachedLearnerLikeConfig(
          learners_root,
          kFirstLearnerRaftId,
          base_port_ + 280);
      const auto second_learner_config = BuildDetachedLearnerLikeConfig(
          learners_root,
          kSecondLearnerRaftId,
          base_port_ + 281);
      WriteStructuredLearnerIdentity(first_learner_config);
      WriteStructuredLearnerIdentity(second_learner_config);

      StandaloneNodeRunner first_learner_runner(
          std::make_shared<RaftNode>(first_learner_config));
      StandaloneNodeRunner second_learner_runner(
          std::make_shared<RaftNode>(second_learner_config));

      std::vector<std::string> observed_diagnostics;
      leader = WaitForSingleLeader(cluster.Nodes(), 8s);
      ASSERT_NE(leader, nullptr) << DescribeCluster(cluster.Nodes());

      raft::JoinMetadataClusterRequest first_join_request =
          MakeJoinMetadataClusterRequest("req-join-t080-learner-a",
                                         kClusterId,
                                         kFirstLearnerNodeId,
                                         kFirstLearnerRaftId,
                                         static_cast<std::uint16_t>(base_port_ + 1280),
                                         static_cast<std::uint16_t>(base_port_ + 280));
      raft::JoinMetadataClusterResponse first_join_response;
      ASSERT_TRUE(JoinMetadataClusterViaAddress(leader->GetStatusSnapshot().address,
                                                first_join_request,
                                                &first_join_response)
                      .ok());
      ASSERT_EQ(first_join_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << first_join_response.summary().message();
      ASSERT_EQ(first_join_response.disposition(),
                raft::JOIN_METADATA_CLUSTER_DISPOSITION_ACCEPTED_PENDING_COMMIT)
          << first_join_response.summary().message();
      ASSERT_FALSE(first_join_response.committed_membership_changed());
      EXPECT_TRUE(first_join_response.requested_membership() ==
                  raft::JOIN_METADATA_TARGET_MEMBERSHIP_LEARNER);
      EXPECT_TRUE(first_join_response.summary().message().find("learner_status=pending") !=
                  std::string::npos)
          << first_join_response.summary().message();
      EXPECT_TRUE(first_join_response.summary().message().find("committed_quorum_size=2") !=
                  std::string::npos)
          << first_join_response.summary().message();
      observed_diagnostics.push_back(first_join_response.summary().message());
      ExpectNoCommittedFourVoterDiagnostic(first_join_response.summary().message(),
                                           "first learner accepted pending commit");

      first_learner_runner.Start();

      RuntimeMembershipEntry first_learner_progress;
      std::string first_learner_progress_diagnostics;
      ASSERT_TRUE(WaitForLearnerReplicationProgress(leader,
                                                    kFirstLearnerRaftId,
                                                    commit_response.summary().log_index(),
                                                    8s,
                                                    &first_learner_progress,
                                                    &first_learner_progress_diagnostics))
          << first_learner_progress_diagnostics << "\n"
          << DescribeCluster(cluster.Nodes());

      auto current_leader = WaitForSingleLeader(cluster.Nodes(), 8s);
      ASSERT_NE(current_leader, nullptr) << DescribeCluster(cluster.Nodes());

      raft::JoinMetadataClusterResponse first_ready_response;
      ASSERT_TRUE(JoinMetadataClusterViaAddress(current_leader->GetStatusSnapshot().address,
                                                first_join_request,
                                                &first_ready_response)
                      .ok());
      EXPECT_EQ(first_ready_response.summary().code(),
                raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY)
          << first_ready_response.summary().message();
      EXPECT_EQ(first_ready_response.disposition(),
                raft::JOIN_METADATA_CLUSTER_DISPOSITION_DUPLICATE)
          << first_ready_response.summary().message();
      EXPECT_FALSE(first_ready_response.committed_membership_changed());
      EXPECT_TRUE(first_ready_response.summary().message().find(
                      "learner_status=ready_to_promote") != std::string::npos)
          << first_ready_response.summary().message();
      EXPECT_TRUE(first_ready_response.summary().message().find(
                      "promotion_status=waiting_for_pair") != std::string::npos)
          << first_ready_response.summary().message();
      EXPECT_TRUE(first_ready_response.summary().message().find(
                      "promotion_block_reason=even_voter_count") != std::string::npos)
          << first_ready_response.summary().message();
      observed_diagnostics.push_back(first_ready_response.summary().message());
      ExpectNoCommittedFourVoterDiagnostic(first_ready_response.summary().message(),
                                           "first learner ready waiting_for_pair");

      const auto ready_runtime = current_leader->GetRuntimeMembershipSummary();
      ExpectRuntimeStillTreatsLearnersAsNonVoters(
          ready_runtime,
          {kFirstLearnerRaftId, kSecondLearnerRaftId},
          "single ready learner before failover");
      EXPECT_EQ(ready_runtime.learner_ids, std::vector<int>({kFirstLearnerRaftId}))
          << DescribeRuntimeMembershipSummary(ready_runtime);
      EXPECT_EQ(ready_runtime.learner_count, 1U)
          << DescribeRuntimeMembershipSummary(ready_runtime);

      raft::JoinMetadataClusterRequest second_join_request =
          MakeJoinMetadataClusterRequest("req-join-t080-learner-b",
                                         kClusterId,
                                         kSecondLearnerNodeId,
                                         kSecondLearnerRaftId,
                                         static_cast<std::uint16_t>(base_port_ + 1281),
                                         static_cast<std::uint16_t>(base_port_ + 281));
      raft::JoinMetadataClusterResponse second_join_response;
      ASSERT_TRUE(JoinMetadataClusterViaAddress(current_leader->GetStatusSnapshot().address,
                                                second_join_request,
                                                &second_join_response)
                      .ok());
      EXPECT_FALSE(second_join_response.committed_membership_changed());
      EXPECT_EQ(second_join_response.requested_membership(),
                raft::JOIN_METADATA_TARGET_MEMBERSHIP_LEARNER);
      observed_diagnostics.push_back(second_join_response.summary().message());
      ExpectNoCommittedFourVoterDiagnostic(second_join_response.summary().message(),
                                           "second learner join attempt before failover");

      const auto leader_index = FindNodeIndex(cluster.Nodes(), current_leader);
      ASSERT_LT(leader_index, cluster.Nodes().size());
      cluster.StopNode(leader_index);

      const std::vector<std::size_t> excluded{leader_index};
      auto new_leader = WaitForSingleLeader(cluster.Nodes(), 10s, excluded);
      ASSERT_NE(new_leader, nullptr)
          << "no new leader after stopping old leader during incomplete batch promote\n"
          << DescribeCluster(cluster.Nodes());

      for (std::size_t index = 0; index < cluster.Nodes().size(); ++index)
      {
        if (std::find(excluded.begin(), excluded.end(), index) != excluded.end())
        {
          continue;
        }
        ExpectCommittedThreeVoterBoundary(
            cluster.Nodes()[index],
            "committed membership after leader failure during incomplete promote");
      }

      const auto failover_runtime = new_leader->GetRuntimeMembershipSummary();
      ExpectRuntimeStillTreatsLearnersAsNonVoters(
          failover_runtime,
          {kFirstLearnerRaftId, kSecondLearnerRaftId},
          "new leader after incomplete batch promote failover");
      EXPECT_LE(failover_runtime.learner_count, 1U)
          << DescribeRuntimeMembershipSummary(failover_runtime);

      raft::JoinMetadataClusterResponse retry_first_on_new_leader;
      ASSERT_TRUE(JoinMetadataClusterViaAddress(new_leader->GetStatusSnapshot().address,
                                                first_join_request,
                                                &retry_first_on_new_leader)
                      .ok());
      EXPECT_FALSE(retry_first_on_new_leader.committed_membership_changed());
      EXPECT_TRUE(retry_first_on_new_leader.summary().code() ==
                      raft::METADATA_STATUS_CODE_OK ||
                  retry_first_on_new_leader.summary().code() ==
                      raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY)
          << retry_first_on_new_leader.summary().message();
      EXPECT_EQ(retry_first_on_new_leader.requested_membership(),
                raft::JOIN_METADATA_TARGET_MEMBERSHIP_LEARNER);
      observed_diagnostics.push_back(retry_first_on_new_leader.summary().message());
      ExpectNoCommittedFourVoterDiagnostic(
          retry_first_on_new_leader.summary().message(),
          "retry first learner on new leader after failover");

      const auto retry_runtime = new_leader->GetRuntimeMembershipSummary();
      ExpectRuntimeStillTreatsLearnersAsNonVoters(
          retry_runtime,
          {kFirstLearnerRaftId, kSecondLearnerRaftId},
          "runtime after retrying first learner on new leader");
      EXPECT_LE(retry_runtime.learner_count, 1U)
          << DescribeRuntimeMembershipSummary(retry_runtime);
      if (!retry_runtime.learner_ids.empty())
      {
        EXPECT_EQ(retry_runtime.learner_ids.front(), kFirstLearnerRaftId)
            << DescribeRuntimeMembershipSummary(retry_runtime);
      }

      raft::JoinMetadataClusterResponse duplicate_first_on_new_leader;
      ASSERT_TRUE(JoinMetadataClusterViaAddress(new_leader->GetStatusSnapshot().address,
                                                first_join_request,
                                                &duplicate_first_on_new_leader)
                      .ok());
      EXPECT_FALSE(duplicate_first_on_new_leader.committed_membership_changed());
      observed_diagnostics.push_back(duplicate_first_on_new_leader.summary().message());
      ExpectNoCommittedFourVoterDiagnostic(
          duplicate_first_on_new_leader.summary().message(),
          "duplicate first learner retry on new leader");

      const auto duplicate_runtime = new_leader->GetRuntimeMembershipSummary();
      ExpectRuntimeStillTreatsLearnersAsNonVoters(
          duplicate_runtime,
          {kFirstLearnerRaftId, kSecondLearnerRaftId},
          "runtime after duplicate retry on new leader");
      EXPECT_LE(duplicate_runtime.learner_count, 1U)
          << DescribeRuntimeMembershipSummary(duplicate_runtime);

      raft::JoinMetadataClusterResponse retry_second_on_new_leader;
      ASSERT_TRUE(JoinMetadataClusterViaAddress(new_leader->GetStatusSnapshot().address,
                                                second_join_request,
                                                &retry_second_on_new_leader)
                      .ok());
      EXPECT_FALSE(retry_second_on_new_leader.committed_membership_changed());
      EXPECT_EQ(retry_second_on_new_leader.requested_membership(),
                raft::JOIN_METADATA_TARGET_MEMBERSHIP_LEARNER);
      observed_diagnostics.push_back(retry_second_on_new_leader.summary().message());
      ExpectNoCommittedFourVoterDiagnostic(
          retry_second_on_new_leader.summary().message(),
          "retry second learner on new leader after failover");

      for (std::size_t index = 0; index < cluster.Nodes().size(); ++index)
      {
        if (std::find(excluded.begin(), excluded.end(), index) != excluded.end())
        {
          continue;
        }
        ExpectCommittedThreeVoterBoundary(
            cluster.Nodes()[index],
            "final committed boundary after failover retries");
      }

      EXPECT_EQ(second_join_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
          << second_join_response.summary().message();
      EXPECT_EQ(second_join_response.disposition(),
                raft::JOIN_METADATA_CLUSTER_DISPOSITION_ACCEPTED_PENDING_COMMIT)
          << second_join_response.summary().message();
      if (second_join_response.summary().code() != raft::METADATA_STATUS_CODE_OK ||
          second_join_response.disposition() !=
              raft::JOIN_METADATA_CLUSTER_DISPOSITION_ACCEPTED_PENDING_COMMIT)
      {
        ADD_FAILURE()
            << "T080 requires a real promote-in-progress boundary with 3 committed voters "
               "+ 2 ready learners before leader failure. current runtime still blocks the "
               "second learner while the first learner waits_for_pair, so this test can "
               "only lock failover safety for the incomplete/blocked path: no partial "
               "committed membership, no committed 4-voter state, no learner restored as "
               "voter, and no quorum shrink. actual_code="
            << second_join_response.summary().code()
            << ", actual_disposition=" << second_join_response.disposition()
            << ", actual_message=" << second_join_response.summary().message()
            << "\ncluster:\n"
            << DescribeCluster(cluster.Nodes());
      }
    }

  } // namespace
} // namespace raftdemo
