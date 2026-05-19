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

#include "raft.grpc.pb.h"
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

            const NodeConfig &Config(const std::size_t index) const
            {
                return configs_.at(index);
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

        raft::CreateMetadataRecordRequest MakeCreateRequest(const std::string &request_id,
                                                            const std::string &object_key)
        {
            raft::CreateMetadataRecordRequest request;
            request.set_request_id(request_id);
            request.set_object_key(object_key);
            request.set_payload("payload-" + object_key);
            request.mutable_manifest()->set_object_size(1024);
            request.mutable_manifest()->set_chunk_size(256);
            request.mutable_manifest()->set_chunk_count(4);
            request.mutable_manifest()->set_checksum("checksum-" + object_key);
            request.mutable_manifest()->add_mock_locations("node-a");
            request.mutable_manifest()->add_mock_locations("node-b");
            return request;
        }

        raft::CommitMetadataRecordRequest MakeCommitRequest(const std::string &request_id,
                                                            const std::string &object_key,
                                                            const std::string &create_request_id)
        {
            raft::CommitMetadataRecordRequest request;
            request.set_request_id(request_id);
            request.set_object_key(object_key);
            request.set_expected_create_request_id(create_request_id);
            request.set_commit_info("commit-info-" + object_key);
            return request;
        }

        raft::HeadMetadataRecordResponse HeadViaLeader(
            const std::vector<std::shared_ptr<RaftNode>> &nodes,
            const std::vector<NodeConfig> &configs,
            const std::vector<std::size_t> &excluded,
            const std::string &object_key,
            const std::chrono::milliseconds timeout)
        {
            raft::HeadMetadataRecordResponse response;
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
                raft::HeadMetadataRecordRequest request;
                request.set_object_key(object_key);
                auto status = stub->HeadMetadataRecord(&context, request, &response);
                if (status.ok() &&
                    response.summary().code() != raft::METADATA_STATUS_CODE_NOT_LEADER)
                {
                    return response;
                }
                std::this_thread::sleep_for(50ms);
            }
            return response;
        }

        raft::ListMetadataRecordsResponse ListViaLeader(
            const std::vector<std::shared_ptr<RaftNode>> &nodes,
            const std::vector<NodeConfig> &configs,
            const std::vector<std::size_t> &excluded,
            const std::string &prefix,
            const std::chrono::milliseconds timeout)
        {
            raft::ListMetadataRecordsResponse response;
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
                raft::ListMetadataRecordsRequest request;
                request.set_prefix(prefix);
                auto status = stub->ListMetadataRecords(&context, request, &response);
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
            const auto configs = BuildThreeNodeConfigs(root_, base_port_);
            MetadataCluster cluster(configs);
            cluster.Start();

            auto leader = WaitForSingleLeader(cluster.Nodes(), 8s);
            ASSERT_NE(leader, nullptr) << DescribeCluster(cluster.Nodes());

            const auto leader_index = FindNodeIndex(cluster.Nodes(), leader);
            ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader";
            auto leader_stub = MakeMetadataStub(configs[leader_index].address);

            const std::string committed_key = "object/committed";
            const std::string pending_key = "object/pending";

            {
                grpc::ClientContext context;
                raft::CreateMetadataRecordResponse response;
                const auto request = MakeCreateRequest("create-committed", committed_key);
                ASSERT_TRUE(leader_stub->CreateMetadataRecord(&context, request, &response).ok());
                ASSERT_EQ(response.summary().code(), raft::METADATA_STATUS_CODE_OK)
                    << response.summary().message();
            }

            {
                grpc::ClientContext context;
                raft::CommitMetadataRecordResponse response;
                const auto request =
                    MakeCommitRequest("commit-committed", committed_key, "create-committed");
                ASSERT_TRUE(leader_stub->CommitMetadataRecord(&context, request, &response).ok());
                ASSERT_EQ(response.summary().code(), raft::METADATA_STATUS_CODE_OK)
                    << response.summary().message();
            }

            {
                grpc::ClientContext context;
                raft::CreateMetadataRecordResponse response;
                const auto request = MakeCreateRequest("create-pending", pending_key);
                ASSERT_TRUE(leader_stub->CreateMetadataRecord(&context, request, &response).ok());
                ASSERT_EQ(response.summary().code(), raft::METADATA_STATUS_CODE_OK)
                    << response.summary().message();
            }

            const auto baseline_head =
                HeadViaLeader(cluster.Nodes(), configs, {}, committed_key, 5s);
            ASSERT_EQ(baseline_head.summary().code(), raft::METADATA_STATUS_CODE_OK)
                << baseline_head.summary().message();
            ASSERT_TRUE(baseline_head.found());
            EXPECT_EQ(baseline_head.record().object_key(), committed_key);
            EXPECT_EQ(baseline_head.record().state(),
                      raft::METADATA_RECORD_STATE_COMMITTED);

            cluster.StopNode(leader_index);
            const std::vector<std::size_t> excluded{leader_index};

            auto new_leader = WaitForSingleLeader(cluster.Nodes(), 10s, excluded);
            ASSERT_NE(new_leader, nullptr)
                << "no new leader after stopping old leader\n"
                << DescribeCluster(cluster.Nodes());
            EXPECT_NE(new_leader, leader);

            const auto committed_head =
                HeadViaLeader(cluster.Nodes(), configs, excluded, committed_key, 8s);
            ASSERT_EQ(committed_head.summary().code(), raft::METADATA_STATUS_CODE_OK)
                << "head after failover failed, message=" << committed_head.summary().message()
                << "\ncluster=\n"
                << DescribeCluster(cluster.Nodes());
            ASSERT_TRUE(committed_head.found());
            EXPECT_EQ(committed_head.record().object_key(), committed_key);
            EXPECT_EQ(committed_head.record().state(),
                      raft::METADATA_RECORD_STATE_COMMITTED);

            const auto pending_head =
                HeadViaLeader(cluster.Nodes(), configs, excluded, pending_key, 5s);
            EXPECT_EQ(pending_head.summary().code(), raft::METADATA_STATUS_CODE_NOT_FOUND)
                << pending_head.summary().message();
            EXPECT_FALSE(pending_head.found());

            const auto list_response =
                ListViaLeader(cluster.Nodes(), configs, excluded, "object/", 5s);
            ASSERT_EQ(list_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
                << list_response.summary().message();
            ASSERT_EQ(list_response.records_size(), 1);
            EXPECT_EQ(list_response.records(0).object_key(), committed_key);
            EXPECT_EQ(list_response.records(0).state(),
                      raft::METADATA_RECORD_STATE_COMMITTED);
        }

        TEST_F(MetadataFailoverTest, SameCommitRequestIdCanBeRetriedOnNewLeader)
        {
            const auto configs = BuildThreeNodeConfigs(root_, base_port_);
            MetadataCluster cluster(configs);
            cluster.Start();

            auto leader = WaitForSingleLeader(cluster.Nodes(), 8s);
            ASSERT_NE(leader, nullptr) << DescribeCluster(cluster.Nodes());

            const auto leader_index = FindNodeIndex(cluster.Nodes(), leader);
            ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader";
            auto leader_stub = MakeMetadataStub(configs[leader_index].address);

            const std::string object_key = "object/retry";
            const auto create_request = MakeCreateRequest("create-retry", object_key);
            const auto commit_request =
                MakeCommitRequest("commit-retry", object_key, "create-retry");

            {
                grpc::ClientContext context;
                raft::CreateMetadataRecordResponse response;
                ASSERT_TRUE(leader_stub->CreateMetadataRecord(&context, create_request, &response).ok());
                ASSERT_EQ(response.summary().code(), raft::METADATA_STATUS_CODE_OK)
                    << response.summary().message();
            }

            {
                grpc::ClientContext context;
                raft::CommitMetadataRecordResponse response;
                ASSERT_TRUE(leader_stub->CommitMetadataRecord(&context, commit_request, &response).ok());
                ASSERT_EQ(response.summary().code(), raft::METADATA_STATUS_CODE_OK)
                    << response.summary().message();
            }

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
            raft::CommitMetadataRecordResponse retry_response;
            ASSERT_TRUE(new_leader_stub->CommitMetadataRecord(
                            &retry_context, commit_request, &retry_response)
                            .ok());
            EXPECT_TRUE(retry_response.summary().code() == raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY ||
                        retry_response.summary().code() == raft::METADATA_STATUS_CODE_OK)
                << retry_response.summary().message();
            EXPECT_EQ(retry_response.summary().state(),
                      raft::METADATA_RECORD_STATE_COMMITTED);

            const auto head_response =
                HeadViaLeader(cluster.Nodes(), configs, excluded, object_key, 5s);
            ASSERT_EQ(head_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
                << head_response.summary().message();
            ASSERT_TRUE(head_response.found());
            EXPECT_EQ(head_response.record().object_key(), object_key);

            const auto list_response =
                ListViaLeader(cluster.Nodes(), configs, excluded, "object/", 5s);
            ASSERT_EQ(list_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
                << list_response.summary().message();
            ASSERT_EQ(list_response.records_size(), 1);
            EXPECT_EQ(list_response.records(0).object_key(), object_key);
        }

    } // namespace
} // namespace raftdemo
