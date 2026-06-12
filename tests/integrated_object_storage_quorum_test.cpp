#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "metadata.grpc.pb.h"
#include "metadata_raft_test_utils.h"
#include "raft/common/config.h"
#include "raft/common/propose.h"
#include "raft/node/raft_node.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "view/view_registry.h"

namespace raftdemo
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;
        using viewdemo::GetClusterViewRequest;
        using viewdemo::MetadataMembershipObservedState;
        using viewdemo::MetadataNodeObservation;
        using viewdemo::MetadataRaftObservedRole;
        using viewdemo::NodeRegistration;
        using viewdemo::RegisterNodeRequest;
        using viewdemo::ViewNodeDiskPressure;
        using viewdemo::ViewNodeHealth;
        using viewdemo::ViewNodeRegistry;
        using viewdemo::ViewNodeType;
        using viewdemo::ViewRegistryStatusCode;

        std::string ProposeStatusName(const ProposeStatus status)
        {
            switch (status)
            {
            case ProposeStatus::kOk:
                return "Ok";
            case ProposeStatus::kNotLeader:
                return "NotLeader";
            case ProposeStatus::kInvalidCommand:
                return "InvalidCommand";
            case ProposeStatus::kNodeStopping:
                return "NodeStopping";
            case ProposeStatus::kReplicationFailed:
                return "ReplicationFailed";
            case ProposeStatus::kCommitFailed:
                return "CommitFailed";
            case ProposeStatus::kApplyFailed:
                return "ApplyFailed";
            case ProposeStatus::kTimeout:
                return "Timeout";
            case ProposeStatus::kOverloaded:
                return "Overloaded";
            }
            return "Unknown";
        }

        std::string AddLearnerProposalStatusName(
            const AddLearnerProposalStatus status)
        {
            switch (status)
            {
            case AddLearnerProposalStatus::kAcceptedPendingCommit:
                return "AcceptedPendingCommit";
            case AddLearnerProposalStatus::kDuplicate:
                return "Duplicate";
            case AddLearnerProposalStatus::kPendingMembershipChange:
                return "PendingMembershipChange";
            case AddLearnerProposalStatus::kRejected:
                return "Rejected";
            case AddLearnerProposalStatus::kNotLeader:
                return "NotLeader";
            case AddLearnerProposalStatus::kNodeStopping:
                return "NodeStopping";
            case AddLearnerProposalStatus::kInvalidArgument:
                return "InvalidArgument";
            }
            return "Unknown";
        }

        bool Contains(const std::string &text, const std::string &needle)
        {
            return text.find(needle) != std::string::npos;
        }

        bool IsLeaderNode(const std::shared_ptr<RaftNode> &node)
        {
            return node != nullptr && Contains(node->Describe(), "role=Leader");
        }

        bool IsExpectedQuorumFailure(const ProposeStatus status)
        {
            return status == ProposeStatus::kTimeout ||
                   status == ProposeStatus::kReplicationFailed ||
                   status == ProposeStatus::kCommitFailed ||
                   status == ProposeStatus::kNotLeader;
        }

        std::uint64_t NowForPath()
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
        }

        int PickBasePort()
        {
            if (const char *env = std::getenv("RAFT_TEST_BASE_PORT"))
            {
                try
                {
                    return std::stoi(env);
                }
                catch (...)
                {
                }
            }

            std::random_device rd;
            const int jitter = static_cast<int>(rd() % 12000);
            const auto tick =
                static_cast<int>(Clock::now().time_since_epoch().count() % 4000);
            return 42000 + jitter + tick;
        }

        MetadataNodeObservation MakeViewMetadataObservation(
            const std::int32_t raft_id,
            const MetadataMembershipObservedState membership_state,
            const std::uint64_t observed_term,
            const std::uint64_t membership_epoch)
        {
            MetadataNodeObservation observation;
            observation.raft_id = raft_id;
            observation.raft_role = MetadataRaftObservedRole::kFollower;
            observation.membership_state = membership_state;
            observation.observed_term = observed_term;
            observation.commit_index = observed_term * 10;
            observation.membership_epoch = membership_epoch;
            return observation;
        }

        NodeRegistration MakeMetadataRegistration(
            const std::string &cluster_id,
            const std::string &node_id,
            const std::int32_t raft_id,
            const std::uint16_t port,
            const std::uint64_t observed_at_unix_ms,
            const MetadataMembershipObservedState membership_state)
        {
            NodeRegistration registration;
            registration.cluster_id = cluster_id;
            registration.node_id = node_id;
            registration.node_type = ViewNodeType::kMetadata;
            registration.endpoint = "127.0.0.1:" + std::to_string(port);
            registration.control_plane_endpoint =
                "127.0.0.1:" + std::to_string(static_cast<std::uint32_t>(port) + 1000);
            registration.data_plane_endpoint =
                "127.0.0.1:" + std::to_string(static_cast<std::uint32_t>(port) + 2000);
            registration.data_dir_fingerprint = "fingerprint-" + node_id;
            registration.observed_at_unix_ms = observed_at_unix_ms;
            registration.failure_domain.zone = "zone-a";
            registration.failure_domain.rack = "rack-1";
            registration.health.health = ViewNodeHealth::kHealthy;
            registration.health.disk_pressure = ViewNodeDiskPressure::kLow;
            registration.load.active_reads = 1;
            registration.load.active_writes = 2;
            registration.load.queued_ops = 3;
            registration.metadata = MakeViewMetadataObservation(
                raft_id,
                membership_state,
                7,
                3);
            return registration;
        }

        RegisterNodeRequest MakeRegisterRequest(NodeRegistration registration,
                                                const std::string &request_id)
        {
            RegisterNodeRequest request;
            request.request_id = request_id;
            request.registration = std::move(registration);
            return request;
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

        AddLearnerProposalRequest MakeAddLearnerProposalRequest(
            const raft::JoinMetadataClusterRequest &request)
        {
            AddLearnerProposalRequest proposal_request;
            proposal_request.cluster_id = request.cluster_id();
            proposal_request.node_id = request.node_id();
            proposal_request.candidate_raft_id = request.candidate_raft_id();
            proposal_request.candidate_client_address =
                request.candidate_client_address();
            proposal_request.candidate_raft_address =
                request.candidate_raft_address();
            proposal_request.candidate_incarnation_id =
                request.candidate_incarnation_id();
            proposal_request.candidate_sequence = request.candidate_sequence();
            proposal_request.persistent_generation =
                request.persistent_generation();
            proposal_request.data_dir_fingerprint =
                request.data_dir_fingerprint();
            return proposal_request;
        }

        grpc::Status JoinMetadataClusterViaAddress(
            const std::string &address,
            const raft::JoinMetadataClusterRequest &request,
            raft::JoinMetadataClusterResponse *response)
        {
            auto channel =
                grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
            auto stub = raft::MetadataService::NewStub(channel);
            grpc::ClientContext context;
            return stub->JoinMetadataCluster(&context, request, response);
        }

        void RegisterNodeOrAssert(ViewNodeRegistry *registry,
                                  NodeRegistration registration,
                                  const std::string &request_id)
        {
            const auto result =
                registry->RegisterNode(MakeRegisterRequest(std::move(registration),
                                                           request_id));
            ASSERT_EQ(result.summary.status, ViewRegistryStatusCode::kOk);
            ASSERT_TRUE(result.snapshot.has_value());
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

        std::filesystem::path TestBinaryDir()
        {
#ifdef RAFT_TEST_BINARY_DIR
            return std::filesystem::path(RAFT_TEST_BINARY_DIR);
#else
            return std::filesystem::current_path();
#endif
        }

        std::filesystem::path MakeTestRoot(const std::string &test_name)
        {
            std::random_device rd;
            std::string safe_name = test_name;
            for (char &ch : safe_name)
            {
                if (ch == '/' || ch == '\\' || ch == ':' || ch == ' ')
                {
                    ch = '_';
                }
            }

#ifdef _WIN32
            const std::string name =
                "ioq_" + std::to_string(NowForPath()) + "_" + std::to_string(rd());
            return std::filesystem::temp_directory_path() / "rq_ioq" / name;
#else
            const std::string name = "integrated_object_storage_quorum_" + safe_name +
                                     "_" + std::to_string(NowForPath()) + "_" +
                                     std::to_string(rd());
            return TestBinaryDir() / "raft_test_data" / "integration" / name;
#endif
        }

        std::vector<NodeConfig> BuildNodeConfigs(
            const std::filesystem::path &data_root,
            const int base_port,
            const std::size_t voter_count)
        {
            std::vector<NodeConfig> configs;
            configs.reserve(voter_count);

            for (std::size_t node_index = 0; node_index < voter_count; ++node_index)
            {
                const int id = static_cast<int>(node_index) + 1;
                NodeConfig cfg;
                cfg.node_id = id;
                cfg.address = "127.0.0.1:" + std::to_string(base_port + id);
                cfg.election_timeout_min = 250ms;
                cfg.election_timeout_max = 500ms;
                cfg.heartbeat_interval = 80ms;
                cfg.rpc_deadline = 250ms;
                cfg.data_dir = (data_root / ("node_" + std::to_string(id))).string();

                for (std::size_t peer_index = 0; peer_index < voter_count; ++peer_index)
                {
                    const int peer_id = static_cast<int>(peer_index) + 1;
                    if (peer_id == id)
                    {
                        continue;
                    }
                    cfg.peers.push_back(
                        PeerConfig{peer_id,
                                   "127.0.0.1:" + std::to_string(base_port + peer_id)});
                }

                configs.push_back(std::move(cfg));
            }

            return configs;
        }

        snapshotConfig MakeDisabledSnapshotConfig(
            const std::filesystem::path &snapshot_root)
        {
            snapshotConfig cfg;
            cfg.enabled = false;
            cfg.snapshot_dir = snapshot_root.string();
            cfg.load_on_startup = false;
            cfg.file_prefix = "snapshot";
            return cfg;
        }

        class QuorumTestCluster
        {
        public:
            QuorumTestCluster(const std::filesystem::path &root,
                              const int base_port,
                              const std::size_t voter_count)
                : snapshot_config_(MakeDisabledSnapshotConfig(root / "raft_snapshots")),
                  voter_count_(voter_count)
            {
                const auto configs =
                    BuildNodeConfigs(root / "raft_data", base_port, voter_count_);
                nodes_.reserve(configs.size());
                for (const auto &cfg : configs)
                {
                    NodeRuntime runtime;
                    runtime.node_id = cfg.node_id;
                    runtime.node = std::make_shared<RaftNode>(cfg, snapshot_config_);
                    nodes_.push_back(std::move(runtime));
                }
            }

            ~QuorumTestCluster()
            {
                StopAll();
            }

            void StartAll()
            {
                for (std::size_t index = 0; index < nodes_.size(); ++index)
                {
                    StartNode(index);
                }
            }

            void StopAll()
            {
                for (std::size_t index = 0; index < nodes_.size(); ++index)
                {
                    StopNode(index);
                }
            }

            void StopNode(const std::size_t index)
            {
                auto &runtime = nodes_.at(index);
                if (!runtime.running)
                {
                    return;
                }

                runtime.node->Stop();
                if (runtime.thread.joinable())
                {
                    runtime.thread.join();
                }
                runtime.running = false;
            }

            std::shared_ptr<RaftNode> WaitForSingleLeader(
                const std::chrono::milliseconds timeout) const
            {
                const auto deadline = Clock::now() + timeout;
                while (Clock::now() < deadline)
                {
                    std::shared_ptr<RaftNode> leader;
                    std::size_t leader_count = 0;
                    for (const auto &runtime : nodes_)
                    {
                        if (!runtime.running)
                        {
                            continue;
                        }
                        if (IsLeaderNode(runtime.node))
                        {
                            leader = runtime.node;
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

            std::string DescribeCluster() const
            {
                std::ostringstream oss;
                bool first = true;
                for (std::size_t index = 0; index < nodes_.size(); ++index)
                {
                    if (!first)
                    {
                        oss << " | ";
                    }
                    first = false;

                    const auto &runtime = nodes_[index];
                    oss << "node[" << index << "] id=" << runtime.node_id
                        << " running=" << (runtime.running ? "true" : "false");
                    if (runtime.running && runtime.node != nullptr)
                    {
                        oss << " " << runtime.node->Describe();
                    }
                }
                return oss.str();
            }

            std::string DescribeObjectStateOnRunningNodes(
                const std::string &bucket,
                const std::string &object_key) const
            {
                std::ostringstream oss;
                bool first = true;
                for (std::size_t index = 0; index < nodes_.size(); ++index)
                {
                    if (!first)
                    {
                        oss << " | ";
                    }
                    first = false;

                    const auto &runtime = nodes_[index];
                    oss << "node[" << index << "] id=" << runtime.node_id
                        << " running=" << (runtime.running ? "true" : "false");
                    if (!runtime.running || runtime.node == nullptr)
                    {
                        continue;
                    }

                    const MetadataStateMachine *state_machine =
                        runtime.node->GetMetadataStateMachineV2();
                    if (state_machine == nullptr)
                    {
                        oss << " metadata_sm=<null>";
                        continue;
                    }

                    const auto head =
                        state_machine->HeadObject({.bucket = bucket, .object_key = object_key});
                    const auto internal = state_machine->FindObject(bucket, object_key);
                    const auto indexed = state_machine->FindIndexedObjectId(bucket, object_key);
                    const auto chunks = state_machine->FindChunkRefs(bucket, object_key);

                    oss << " head=" << static_cast<int>(head.result.code);
                    if (head.record.has_value())
                    {
                        oss << "/" << head.record->object_id
                            << "/committed=" << (head.record->IsCommitted() ? "true" : "false");
                    }
                    oss << " internal=";
                    if (internal.has_value())
                    {
                        oss << internal->object_id << "/state="
                            << static_cast<int>(internal->state);
                    }
                    else
                    {
                        oss << "<none>";
                    }
                    oss << " indexed=";
                    if (indexed.has_value())
                    {
                        oss << *indexed;
                    }
                    else
                    {
                        oss << "<none>";
                    }
                    oss << " chunks=";
                    if (chunks.has_value())
                    {
                        oss << chunks->size();
                    }
                    else
                    {
                        oss << "<none>";
                    }
                    oss << " requests=" << state_machine->RequestCount()
                        << " last_applied_index=" << state_machine->LastAppliedIndex();
                }

                return oss.str();
            }

            std::vector<std::size_t> OtherIndexes(const std::size_t index) const
            {
                std::vector<std::size_t> result;
                for (std::size_t current = 0; current < nodes_.size(); ++current)
                {
                    if (current != index)
                    {
                        result.push_back(current);
                    }
                }
                return result;
            }

            std::shared_ptr<RaftNode> Node(const std::size_t index) const
            {
                return nodes_.at(index).node;
            }

            bool IsRunning(const std::size_t index) const
            {
                return nodes_.at(index).running;
            }

            std::size_t Size() const
            {
                return nodes_.size();
            }

        private:
            void StartNode(const std::size_t index)
            {
                auto &runtime = nodes_.at(index);
                if (runtime.running)
                {
                    return;
                }

                runtime.running = true;
                const auto node = runtime.node;
                runtime.thread = std::thread([node]()
                                             {
                                                 node->Start();
                                                 node->Wait();
                                             });
            }

            struct NodeRuntime
            {
                int node_id{0};
                std::shared_ptr<RaftNode> node;
                std::thread thread;
                bool running{false};
            };

            snapshotConfig snapshot_config_;
            std::size_t voter_count_{0};
            std::vector<NodeRuntime> nodes_;
        };

        bool WaitUntilPendingObjectReplicatedOnAllRunning(
            const QuorumTestCluster &cluster,
            const std::string &bucket,
            const std::string &object_key,
            const std::string &object_id,
            const std::uint64_t expected_last_applied_index,
            const std::chrono::milliseconds timeout,
            std::string *diagnostics)
        {
            const auto deadline = Clock::now() + timeout;
            std::string last_snapshot = cluster.DescribeObjectStateOnRunningNodes(
                bucket, object_key);

            while (Clock::now() < deadline)
            {
                bool ok = true;
                for (std::size_t index = 0; index < cluster.Size(); ++index)
                {
                    if (!cluster.IsRunning(index))
                    {
                        continue;
                    }
                    const auto node = cluster.Node(index);
                    const MetadataStateMachine *state_machine =
                        node != nullptr ? node->GetMetadataStateMachineV2() : nullptr;
                    if (state_machine == nullptr)
                    {
                        ok = false;
                        break;
                    }

                    const auto head =
                        state_machine->HeadObject({.bucket = bucket, .object_key = object_key});
                    const auto internal = state_machine->FindObject(bucket, object_key);
                    if (head.result.code != MetadataStatusCode::kNotFound ||
                        head.record.has_value() || !internal.has_value() ||
                        internal->object_id != object_id ||
                        internal->state != ObjectState::PENDING ||
                        state_machine->FindChunkRefs(bucket, object_key).has_value() ||
                        state_machine->LastAppliedIndex() < expected_last_applied_index)
                    {
                        ok = false;
                        break;
                    }
                }

                last_snapshot = cluster.DescribeObjectStateOnRunningNodes(bucket, object_key);
                if (ok)
                {
                    if (diagnostics != nullptr)
                    {
                        *diagnostics = last_snapshot;
                    }
                    return true;
                }

                std::this_thread::sleep_for(50ms);
            }

            if (diagnostics != nullptr)
            {
                *diagnostics = last_snapshot;
            }
            return false;
        }

        bool WaitForPendingObjectToRemainInvisible(
            const std::shared_ptr<RaftNode> &node,
            const std::string &bucket,
            const std::string &object_key,
            const std::string &object_id,
            const std::size_t expected_request_count,
            const std::uint64_t expected_last_applied_index,
            const std::chrono::milliseconds timeout,
            std::string *diagnostics)
        {
            const auto deadline = Clock::now() + timeout;
            std::string last_snapshot;

            while (Clock::now() < deadline)
            {
                const MetadataStateMachine *state_machine =
                    node != nullptr ? node->GetMetadataStateMachineV2() : nullptr;
                if (state_machine == nullptr)
                {
                    if (diagnostics != nullptr)
                    {
                        *diagnostics = "metadata state machine unavailable";
                    }
                    return false;
                }

                const auto head =
                    state_machine->HeadObject({.bucket = bucket, .object_key = object_key});
                const auto internal = state_machine->FindObject(bucket, object_key);
                const auto indexed = state_machine->FindIndexedObjectId(bucket, object_key);
                const auto chunks = state_machine->FindChunkRefs(bucket, object_key);

                std::ostringstream oss;
                oss << "head=" << static_cast<int>(head.result.code)
                    << " head_record=" << (head.record.has_value() ? "true" : "false")
                    << " internal=";
                if (internal.has_value())
                {
                    oss << internal->object_id << "/state="
                        << static_cast<int>(internal->state);
                }
                else
                {
                    oss << "<none>";
                }
                oss << " indexed=";
                if (indexed.has_value())
                {
                    oss << *indexed;
                }
                else
                {
                    oss << "<none>";
                }
                oss << " chunks=";
                if (chunks.has_value())
                {
                    oss << chunks->size();
                }
                else
                {
                    oss << "<none>";
                }
                oss << " requests=" << state_machine->RequestCount()
                    << " last_applied_index=" << state_machine->LastAppliedIndex();
                last_snapshot = oss.str();

                if (head.result.code != MetadataStatusCode::kNotFound ||
                    head.record.has_value() || !internal.has_value() ||
                    internal->object_id != object_id ||
                    internal->state != ObjectState::PENDING ||
                    indexed.has_value() || chunks.has_value() ||
                    state_machine->RequestCount() != expected_request_count ||
                    state_machine->LastAppliedIndex() != expected_last_applied_index)
                {
                    if (diagnostics != nullptr)
                    {
                        *diagnostics = last_snapshot;
                    }
                    return false;
                }

                std::this_thread::sleep_for(50ms);
            }

            if (diagnostics != nullptr)
            {
                *diagnostics = last_snapshot;
            }
            return true;
        }

        bool WaitUntilCommittedObjectOnAllRunning(
            const QuorumTestCluster &cluster,
            const std::string &bucket,
            const std::string &object_key,
            const std::string &object_id,
            const std::uint64_t expected_min_last_applied_index,
            const std::chrono::milliseconds timeout,
            std::string *diagnostics)
        {
            const auto deadline = Clock::now() + timeout;
            std::string last_snapshot =
                cluster.DescribeObjectStateOnRunningNodes(bucket, object_key);

            while (Clock::now() < deadline)
            {
                bool ok = true;
                for (std::size_t index = 0; index < cluster.Size(); ++index)
                {
                    if (!cluster.IsRunning(index))
                    {
                        continue;
                    }
                    const auto node = cluster.Node(index);
                    const MetadataStateMachine *state_machine =
                        node != nullptr ? node->GetMetadataStateMachineV2() : nullptr;
                    if (state_machine == nullptr)
                    {
                        ok = false;
                        break;
                    }

                    const auto head =
                        state_machine->HeadObject({.bucket = bucket, .object_key = object_key});
                    const auto internal = state_machine->FindObject(bucket, object_key);
                    const auto indexed = state_machine->FindIndexedObjectId(bucket, object_key);
                    const auto chunks = state_machine->FindChunkRefs(bucket, object_key);
                    if (!head.result.Ok() || !head.record.has_value() ||
                        !head.record->IsCommitted() || !internal.has_value() ||
                        internal->object_id != object_id ||
                        internal->state != ObjectState::COMMITTED ||
                        !indexed.has_value() || *indexed != object_id ||
                        !chunks.has_value() || chunks->size() != 2U ||
                        state_machine->LastAppliedIndex() < expected_min_last_applied_index)
                    {
                        ok = false;
                        break;
                    }
                }

                last_snapshot = cluster.DescribeObjectStateOnRunningNodes(bucket, object_key);
                if (ok)
                {
                    if (diagnostics != nullptr)
                    {
                        *diagnostics = last_snapshot;
                    }
                    return true;
                }

                std::this_thread::sleep_for(50ms);
            }

            if (diagnostics != nullptr)
            {
                *diagnostics = last_snapshot;
            }
            return false;
        }

        class IntegratedObjectStorageQuorumTest : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                const auto *test_info =
                    ::testing::UnitTest::GetInstance()->current_test_info();
                const std::string test_name =
                    std::string(test_info->test_suite_name()) + "." + test_info->name();

                root_ = MakeTestRoot(test_name);
                base_port_ = PickBasePort();

                std::error_code ec;
                std::filesystem::remove_all(root_, ec);
                std::filesystem::create_directories(root_ / "raft_data", ec);
                ASSERT_FALSE(ec) << "failed to create data root: " << ec.message();
                std::filesystem::create_directories(root_ / "raft_snapshots", ec);
                ASSERT_FALSE(ec) << "failed to create snapshot root: " << ec.message();
            }

            void TearDown() override
            {
                std::error_code ec;
                if (!HasFailure())
                {
                    std::filesystem::remove_all(root_, ec);
                }
            }

            QuorumTestCluster MakeCluster(const std::size_t voter_count) const
            {
                return QuorumTestCluster(root_, base_port_, voter_count);
            }

            std::filesystem::path root_;
            int base_port_{0};
        };

        void ExpectCommittedMembershipUnchangedOnRunningNodes(
            const QuorumTestCluster &cluster,
            const std::vector<int> &expected_voter_ids,
            const std::size_t expected_quorum_size)
        {
            for (std::size_t index = 0; index < cluster.Size(); ++index)
            {
                if (!cluster.IsRunning(index))
                {
                    continue;
                }

                const auto node = cluster.Node(index);
                ASSERT_NE(node, nullptr);
                const auto summary = node->GetCommittedMembershipQuorumSummary();
                EXPECT_EQ(summary.voter_ids, expected_voter_ids)
                    << "running node[" << index
                    << "] unexpectedly changed committed voter set: "
                    << DescribeCommittedMembershipSummary(summary);
                EXPECT_TRUE(summary.learner_ids.empty())
                    << "running node[" << index
                    << "] should not expose committed learners before join membership is "
                       "implemented: "
                    << DescribeCommittedMembershipSummary(summary);
                EXPECT_EQ(summary.voter_count, expected_voter_ids.size())
                    << DescribeCommittedMembershipSummary(summary);
                EXPECT_EQ(summary.learner_count, 0U)
                    << DescribeCommittedMembershipSummary(summary);
                EXPECT_EQ(summary.quorum_size, expected_quorum_size)
                    << DescribeCommittedMembershipSummary(summary);
            }
        }

        TEST_F(IntegratedObjectStorageQuorumTest,
               ThreeVoterCommittedMembershipDoesNotShrinkQuorumWhenOnlyOneNodeRemainsLive)
        {
            constexpr const char *kBucket = "bucket-t050";
            constexpr const char *kObjectKey = "objects/quorum-insufficient.bin";
            constexpr const char *kObjectId = "obj-t050";

            auto cluster = MakeCluster(3);
            cluster.StartAll();

            const auto leader = cluster.WaitForSingleLeader(8s);
            ASSERT_NE(leader, nullptr)
                << "3-voter cluster failed to elect a single leader; cluster="
                << cluster.DescribeCluster();

            ProposeResult create_bucket_result;
            ASSERT_TRUE(test::ProposeCreateBucketWithRetry(
                            {cluster.Node(0), cluster.Node(1), cluster.Node(2)},
                            kBucket,
                            "create-bucket-t050",
                            8s,
                            &create_bucket_result))
                << "CreateBucket should succeed before quorum loss; status="
                << ProposeStatusName(create_bucket_result.status)
                << ", message=" << create_bucket_result.message
                << ", cluster=" << cluster.DescribeCluster();

            ProposeResult create_object_result;
            ASSERT_TRUE(test::ProposeMetadataCommandWithRetry(
                            {cluster.Node(0), cluster.Node(1), cluster.Node(2)},
                            test::MakeCreateObjectCommand(kBucket,
                                                          kObjectKey,
                                                          kObjectId,
                                                          "create-object-t050"),
                            8s,
                            &create_object_result))
                << "CreateObject should reach quorum before isolating voters; status="
                << ProposeStatusName(create_object_result.status)
                << ", message=" << create_object_result.message
                << ", cluster=" << cluster.DescribeCluster();

            std::string pending_replication_diagnostics;
            ASSERT_TRUE(WaitUntilPendingObjectReplicatedOnAllRunning(
                            cluster,
                            kBucket,
                            kObjectKey,
                            kObjectId,
                            create_object_result.log_index,
                            5s,
                            &pending_replication_diagnostics))
                << "PENDING object did not replicate before quorum loss; values="
                << pending_replication_diagnostics
                << ", cluster=" << cluster.DescribeCluster();

            const MetadataStateMachine *leader_state_machine =
                leader->GetMetadataStateMachineV2();
            ASSERT_NE(leader_state_machine, nullptr) << leader->Describe();
            const std::size_t baseline_request_count =
                leader_state_machine->RequestCount();
            const std::uint64_t baseline_last_applied_index =
                leader_state_machine->LastAppliedIndex();

            std::size_t leader_index = 0;
            while (leader_index < cluster.Size() &&
                   cluster.Node(leader_index) != leader)
            {
                ++leader_index;
            }
            ASSERT_LT(leader_index, cluster.Size());

            for (const auto follower_index : cluster.OtherIndexes(leader_index))
            {
                cluster.StopNode(follower_index);
            }

            const std::string isolated_snapshot = leader->Describe();
            const ProposeResult commit_result = leader->ProposeMetadata(
                SerializeMetadataCommand(
                    test::MakeCommitObjectCommand(kBucket,
                                                  kObjectKey,
                                                  kObjectId,
                                                  "commit-object-t050")));

            EXPECT_NE(commit_result.status, ProposeStatus::kOk)
                << "3-voter membership should still require quorum=2 after 2 voters stop; "
                   "single live node must not commit object. cluster="
                << cluster.DescribeCluster()
                << ", isolated_snapshot=" << isolated_snapshot;
            EXPECT_TRUE(IsExpectedQuorumFailure(commit_result.status))
                << "expected quorum-insufficient failure mode (timeout, replication failed, "
                   "commit failed, or no legal leader) but got status="
                << ProposeStatusName(commit_result.status)
                << ", message=" << commit_result.message
                << ", cluster=" << cluster.DescribeCluster()
                << ", isolated_snapshot=" << isolated_snapshot;

            std::string pending_visibility_diagnostics;
            EXPECT_TRUE(WaitForPendingObjectToRemainInvisible(
                leader,
                kBucket,
                kObjectKey,
                kObjectId,
                baseline_request_count,
                baseline_last_applied_index,
                500ms,
                &pending_visibility_diagnostics))
                << "single surviving node illegally committed or applied new object after "
                   "quorum loss; state="
                << pending_visibility_diagnostics
                << ", propose_status=" << ProposeStatusName(commit_result.status)
                << ", propose_message=" << commit_result.message
                << ", cluster=" << cluster.DescribeCluster()
                << ", isolated_snapshot=" << isolated_snapshot
                << ", running_values="
                << cluster.DescribeObjectStateOnRunningNodes(kBucket, kObjectKey);
        }

        TEST_F(IntegratedObjectStorageQuorumTest,
               FiveVoterCommittedMembershipKeepsQuorumThreeAndAllowsCommitWithThreeReachableVoters)
        {
            constexpr const char *kBucket = "bucket-t051";
            constexpr const char *kObjectKey = "objects/quorum-majority-available.bin";
            constexpr const char *kObjectId = "obj-t051";

            auto cluster = MakeCluster(5);
            cluster.StartAll();

            const auto original_leader = cluster.WaitForSingleLeader(10s);
            ASSERT_NE(original_leader, nullptr)
                << "5-voter cluster failed to elect a single leader before availability test; "
                   "cluster="
                << cluster.DescribeCluster();

            ProposeResult create_bucket_result;
            ASSERT_TRUE(test::ProposeCreateBucketWithRetry(
                            {cluster.Node(0),
                             cluster.Node(1),
                             cluster.Node(2),
                             cluster.Node(3),
                             cluster.Node(4)},
                            kBucket,
                            "create-bucket-t051",
                            10s,
                            &create_bucket_result))
                << "CreateBucket should succeed before voter loss; status="
                << ProposeStatusName(create_bucket_result.status)
                << ", message=" << create_bucket_result.message
                << ", cluster=" << cluster.DescribeCluster();

            std::size_t original_leader_index = 0;
            while (original_leader_index < cluster.Size() &&
                   cluster.Node(original_leader_index) != original_leader)
            {
                ++original_leader_index;
            }
            ASSERT_LT(original_leader_index, cluster.Size());

            const auto stopped_indexes = cluster.OtherIndexes(original_leader_index);
            ASSERT_GE(stopped_indexes.size(), 2U);

            cluster.StopNode(original_leader_index);
            cluster.StopNode(stopped_indexes[0]);

            const auto surviving_leader = cluster.WaitForSingleLeader(10s);
            ASSERT_NE(surviving_leader, nullptr)
                << "5-voter cluster should keep leader election availability with 3 reachable "
                   "voters; cluster="
                << cluster.DescribeCluster();

            ProposeResult create_object_result;
            ASSERT_TRUE(test::ProposeMetadataCommandWithRetry(
                            {cluster.Node(0),
                             cluster.Node(1),
                             cluster.Node(2),
                             cluster.Node(3),
                             cluster.Node(4)},
                            test::MakeCreateObjectCommand(kBucket,
                                                          kObjectKey,
                                                          kObjectId,
                                                          "create-object-t051"),
                            10s,
                            &create_object_result,
                            {original_leader_index, stopped_indexes[0]}))
                << "5-voter quorum should remain available with 3 reachable voters when "
                   "committed membership quorum is 3; create status="
                << ProposeStatusName(create_object_result.status)
                << ", message=" << create_object_result.message
                << ", cluster=" << cluster.DescribeCluster();

            std::string pending_replication_diagnostics;
            ASSERT_TRUE(WaitUntilPendingObjectReplicatedOnAllRunning(
                            cluster,
                            kBucket,
                            kObjectKey,
                            kObjectId,
                            create_object_result.log_index,
                            5s,
                            &pending_replication_diagnostics))
                << "PENDING object did not replicate to the surviving 3-voter majority; values="
                << pending_replication_diagnostics
                << ", cluster=" << cluster.DescribeCluster();

            ProposeResult commit_result;
            ASSERT_TRUE(test::ProposeMetadataCommandWithRetry(
                            {cluster.Node(0),
                             cluster.Node(1),
                             cluster.Node(2),
                             cluster.Node(3),
                             cluster.Node(4)},
                            test::MakeCommitObjectCommand(kBucket,
                                                          kObjectKey,
                                                          kObjectId,
                                                          "commit-object-t051"),
                            10s,
                            &commit_result,
                            {original_leader_index, stopped_indexes[0]}))
                << "5-voter committed membership should use quorum=3, so 3 reachable voters "
                   "must still commit metadata successfully; status="
                << ProposeStatusName(commit_result.status)
                << ", message=" << commit_result.message
                << ", cluster=" << cluster.DescribeCluster();

            std::string committed_replication_diagnostics;
            EXPECT_TRUE(WaitUntilCommittedObjectOnAllRunning(
                cluster,
                kBucket,
                kObjectKey,
                kObjectId,
                commit_result.log_index,
                5s,
                &committed_replication_diagnostics))
                << "surviving 3-voter majority failed to converge on committed object after "
                   "two voters became unavailable; values="
                << committed_replication_diagnostics
                << ", commit_status=" << ProposeStatusName(commit_result.status)
                << ", commit_message=" << commit_result.message
                << ", cluster=" << cluster.DescribeCluster();
        }

        TEST_F(IntegratedObjectStorageQuorumTest,
               ViewNodeRegisteredObservedVoterDoesNotExpandCommittedRaftVoterSet)
        {
            constexpr const char *kBucket = "bucket-t052";
            constexpr const char *kObjectKey = "objects/viewnode-observed-voter.bin";
            constexpr const char *kObjectId = "obj-t052";
            constexpr const char *kViewClusterId = "cluster-t052";

            auto cluster = MakeCluster(3);
            cluster.StartAll();

            const auto original_leader = cluster.WaitForSingleLeader(8s);
            ASSERT_NE(original_leader, nullptr)
                << "3-voter cluster failed to elect leader before ViewNode boundary test; "
                   "cluster="
                << cluster.DescribeCluster();

            ProposeResult create_bucket_result;
            ASSERT_TRUE(test::ProposeCreateBucketWithRetry(
                            {cluster.Node(0), cluster.Node(1), cluster.Node(2)},
                            kBucket,
                            "create-bucket-t052",
                            8s,
                            &create_bucket_result))
                << "CreateBucket should succeed before stopping one committed voter; status="
                << ProposeStatusName(create_bucket_result.status)
                << ", message=" << create_bucket_result.message
                << ", cluster=" << cluster.DescribeCluster();

            std::size_t leader_index = 0;
            while (leader_index < cluster.Size() &&
                   cluster.Node(leader_index) != original_leader)
            {
                ++leader_index;
            }
            ASSERT_LT(leader_index, cluster.Size());

            const auto follower_indexes = cluster.OtherIndexes(leader_index);
            ASSERT_FALSE(follower_indexes.empty());
            const std::size_t stopped_voter_index = follower_indexes.front();
            cluster.StopNode(stopped_voter_index);

            const auto surviving_leader = cluster.WaitForSingleLeader(8s);
            ASSERT_NE(surviving_leader, nullptr)
                << "3-voter cluster should keep commit availability with one voter stopped; "
                   "cluster="
                << cluster.DescribeCluster();

            ViewNodeRegistry registry;
            RegisterNodeOrAssert(
                &registry,
                MakeMetadataRegistration(kViewClusterId,
                                         "meta-observed-1",
                                         1,
                                         static_cast<std::uint16_t>(base_port_ + 101),
                                         100,
                                         MetadataMembershipObservedState::kVoter),
                "register-meta-observed-1");
            RegisterNodeOrAssert(
                &registry,
                MakeMetadataRegistration(kViewClusterId,
                                         "meta-observed-2",
                                         2,
                                         static_cast<std::uint16_t>(base_port_ + 102),
                                         101,
                                         MetadataMembershipObservedState::kVoter),
                "register-meta-observed-2");
            RegisterNodeOrAssert(
                &registry,
                MakeMetadataRegistration(kViewClusterId,
                                         "meta-observed-3",
                                         3,
                                         static_cast<std::uint16_t>(base_port_ + 103),
                                         102,
                                         MetadataMembershipObservedState::kVoter),
                "register-meta-observed-3");
            RegisterNodeOrAssert(
                &registry,
                MakeMetadataRegistration(kViewClusterId,
                                         "meta-observed-extra",
                                         4,
                                         static_cast<std::uint16_t>(base_port_ + 104),
                                         103,
                                         MetadataMembershipObservedState::kVoter),
                "register-meta-observed-extra");

            GetClusterViewRequest cluster_view_request;
            cluster_view_request.request_id = "cluster-view-t052";
            cluster_view_request.cluster_id = kViewClusterId;
            cluster_view_request.include_dead_nodes = true;
            cluster_view_request.include_warnings = true;

            const auto cluster_view = registry.GetClusterView(cluster_view_request, 200);
            ASSERT_EQ(cluster_view.summary.status, ViewRegistryStatusCode::kOk);
            ASSERT_EQ(cluster_view.snapshot.metadata_nodes.size(), 4U);

            bool extra_observed_voter_found = false;
            for (const auto &metadata_node : cluster_view.snapshot.metadata_nodes)
            {
                if (metadata_node.node_id != "meta-observed-extra")
                {
                    continue;
                }
                ASSERT_TRUE(metadata_node.metadata.has_value());
                EXPECT_EQ(metadata_node.metadata->membership_state,
                          MetadataMembershipObservedState::kVoter);
                EXPECT_EQ(metadata_node.metadata->raft_id, 4);
                extra_observed_voter_found = true;
            }
            ASSERT_TRUE(extra_observed_voter_found)
                << "ViewNode should expose the extra registered metadata node as an observed "
                   "VOTER for this boundary test";

            ProposeResult create_object_result;
            ASSERT_TRUE(test::ProposeMetadataCommandWithRetry(
                            {cluster.Node(0), cluster.Node(1), cluster.Node(2)},
                            test::MakeCreateObjectCommand(kBucket,
                                                          kObjectKey,
                                                          kObjectId,
                                                          "create-object-t052"),
                            8s,
                            &create_object_result,
                            {stopped_voter_index}))
                << "ViewNode observation must not enlarge committed membership from 3 voters "
                   "to 4; with one real voter stopped, the surviving 2-voter majority should "
                   "still create metadata successfully. status="
                << ProposeStatusName(create_object_result.status)
                << ", message=" << create_object_result.message
                << ", cluster=" << cluster.DescribeCluster();

            std::string pending_replication_diagnostics;
            ASSERT_TRUE(WaitUntilPendingObjectReplicatedOnAllRunning(
                            cluster,
                            kBucket,
                            kObjectKey,
                            kObjectId,
                            create_object_result.log_index,
                            5s,
                            &pending_replication_diagnostics))
                << "PENDING object did not replicate across the real 2-voter majority after "
                   "ViewNode registered an extra observed voter; values="
                << pending_replication_diagnostics
                << ", cluster=" << cluster.DescribeCluster();

            ProposeResult commit_result;
            ASSERT_TRUE(test::ProposeMetadataCommandWithRetry(
                            {cluster.Node(0), cluster.Node(1), cluster.Node(2)},
                            test::MakeCommitObjectCommand(kBucket,
                                                          kObjectKey,
                                                          kObjectId,
                                                          "commit-object-t052"),
                            8s,
                            &commit_result,
                            {stopped_voter_index}))
                << "extra metadata node registered to ViewNode as observed VOTER must not be "
                   "counted into Raft quorum. committed membership remains 3 voters, so the "
                   "surviving 2-voter majority must still commit. status="
                << ProposeStatusName(commit_result.status)
                << ", message=" << commit_result.message
                << ", cluster=" << cluster.DescribeCluster();

            std::string committed_replication_diagnostics;
            EXPECT_TRUE(WaitUntilCommittedObjectOnAllRunning(
                cluster,
                kBucket,
                kObjectKey,
                kObjectId,
                commit_result.log_index,
                5s,
                &committed_replication_diagnostics))
                << "ViewNode observation leaked into quorum calculation or object visibility; "
                   "surviving committed majority did not converge on COMMITTED object. values="
                << committed_replication_diagnostics
                << ", commit_status=" << ProposeStatusName(commit_result.status)
                << ", commit_message=" << commit_result.message
                << ", cluster=" << cluster.DescribeCluster();
        }

        TEST_F(IntegratedObjectStorageQuorumTest,
               DuplicateObservedJoinCandidateDoesNotCreateDuplicateCommittedMembershipEntry)
        {
            constexpr const char *kBucket = "bucket-t057-duplicate";
            constexpr const char *kObjectKey = "objects/duplicate-join-candidate.bin";
            constexpr const char *kObjectId = "obj-t057-duplicate";
            constexpr const char *kViewClusterId = "cluster-t057-duplicate";
            const std::vector<int> kCommittedVoters{1, 2, 3};

            auto cluster = MakeCluster(3);
            cluster.StartAll();

            const auto original_leader = cluster.WaitForSingleLeader(8s);
            ASSERT_NE(original_leader, nullptr)
                << "3-voter cluster failed to elect leader before duplicate join boundary "
                   "test; cluster="
                << cluster.DescribeCluster();

            ProposeResult create_bucket_result;
            ASSERT_TRUE(test::ProposeCreateBucketWithRetry(
                            {cluster.Node(0), cluster.Node(1), cluster.Node(2)},
                            kBucket,
                            "create-bucket-t057-duplicate",
                            8s,
                            &create_bucket_result))
                << "CreateBucket should succeed before duplicate join boundary test; status="
                << ProposeStatusName(create_bucket_result.status)
                << ", message=" << create_bucket_result.message
                << ", cluster=" << cluster.DescribeCluster();

            std::size_t leader_index = 0;
            while (leader_index < cluster.Size() &&
                   cluster.Node(leader_index) != original_leader)
            {
                ++leader_index;
            }
            ASSERT_LT(leader_index, cluster.Size());

            const auto follower_indexes = cluster.OtherIndexes(leader_index);
            ASSERT_FALSE(follower_indexes.empty());
            const std::size_t stopped_voter_index = follower_indexes.front();
            cluster.StopNode(stopped_voter_index);

            const auto surviving_leader = cluster.WaitForSingleLeader(8s);
            ASSERT_NE(surviving_leader, nullptr)
                << "3-voter cluster should keep 2-voter majority after stopping one voter; "
                   "cluster="
                << cluster.DescribeCluster();

            ViewNodeRegistry registry;
            RegisterNodeOrAssert(
                &registry,
                MakeMetadataRegistration(kViewClusterId,
                                         "meta-committed-1",
                                         1,
                                         static_cast<std::uint16_t>(base_port_ + 111),
                                         100,
                                         MetadataMembershipObservedState::kVoter),
                "register-meta-committed-1");
            RegisterNodeOrAssert(
                &registry,
                MakeMetadataRegistration(kViewClusterId,
                                         "meta-committed-2",
                                         2,
                                         static_cast<std::uint16_t>(base_port_ + 112),
                                         101,
                                         MetadataMembershipObservedState::kVoter),
                "register-meta-committed-2");
            RegisterNodeOrAssert(
                &registry,
                MakeMetadataRegistration(kViewClusterId,
                                         "meta-committed-3",
                                         3,
                                         static_cast<std::uint16_t>(base_port_ + 113),
                                         102,
                                         MetadataMembershipObservedState::kVoter),
                "register-meta-committed-3");
            RegisterNodeOrAssert(
                &registry,
                MakeMetadataRegistration(kViewClusterId,
                                         "meta-join-candidate-a",
                                         41,
                                         static_cast<std::uint16_t>(base_port_ + 141),
                                         200,
                                         MetadataMembershipObservedState::kJoining),
                "register-meta-join-candidate-a");
            const auto duplicate_candidate_result = registry.RegisterNode(
                MakeRegisterRequest(
                    MakeMetadataRegistration(kViewClusterId,
                                             "meta-join-candidate-a",
                                             41,
                                             static_cast<std::uint16_t>(base_port_ + 141),
                                             260,
                                             MetadataMembershipObservedState::kJoining),
                    "register-meta-join-candidate-a-replay"));
            ASSERT_TRUE(duplicate_candidate_result.summary.ok());
            EXPECT_EQ(duplicate_candidate_result.summary.status,
                      ViewRegistryStatusCode::kIdempotentReplay)
                << "same candidate duplicate join should currently be treated as explicit "
                   "idempotent replay instead of creating a second membership entry";

            GetClusterViewRequest cluster_view_request;
            cluster_view_request.request_id = "cluster-view-t057-duplicate";
            cluster_view_request.cluster_id = kViewClusterId;
            cluster_view_request.include_dead_nodes = true;
            cluster_view_request.include_warnings = true;

            const auto cluster_view = registry.GetClusterView(cluster_view_request, 300);
            ASSERT_EQ(cluster_view.summary.status, ViewRegistryStatusCode::kOk);

            std::size_t candidate_count = 0;
            for (const auto &metadata_node : cluster_view.snapshot.metadata_nodes)
            {
                if (metadata_node.node_id == "meta-join-candidate-a")
                {
                    ++candidate_count;
                    ASSERT_TRUE(metadata_node.metadata.has_value());
                    EXPECT_EQ(metadata_node.metadata->membership_state,
                              MetadataMembershipObservedState::kJoining);
                    EXPECT_EQ(metadata_node.metadata->raft_id, 41);
                }
            }
            EXPECT_EQ(candidate_count, 1U)
                << "duplicate join candidate replay should remain a single observed candidate "
                   "record instead of creating duplicate membership entries";
            EXPECT_EQ(cluster_view.snapshot.metadata_nodes.size(), 4U);

            ExpectCommittedMembershipUnchangedOnRunningNodes(cluster,
                                                            kCommittedVoters,
                                                            2U);

            ProposeResult create_object_result;
            ASSERT_TRUE(test::ProposeMetadataCommandWithRetry(
                            {cluster.Node(0), cluster.Node(1), cluster.Node(2)},
                            test::MakeCreateObjectCommand(
                                kBucket,
                                kObjectKey,
                                kObjectId,
                                "create-object-t057-duplicate"),
                            8s,
                            &create_object_result,
                            {stopped_voter_index}))
                << "duplicate join candidate replay must not enlarge committed membership or "
                   "quorum; surviving 2-voter majority should still create metadata. status="
                << ProposeStatusName(create_object_result.status)
                << ", message=" << create_object_result.message
                << ", cluster=" << cluster.DescribeCluster();

            std::string pending_replication_diagnostics;
            ASSERT_TRUE(WaitUntilPendingObjectReplicatedOnAllRunning(
                            cluster,
                            kBucket,
                            kObjectKey,
                            kObjectId,
                            create_object_result.log_index,
                            5s,
                            &pending_replication_diagnostics))
                << "PENDING object did not replicate across surviving committed majority after "
                   "duplicate join candidate replay; values="
                << pending_replication_diagnostics
                << ", cluster=" << cluster.DescribeCluster();

            ProposeResult commit_result;
            ASSERT_TRUE(test::ProposeMetadataCommandWithRetry(
                            {cluster.Node(0), cluster.Node(1), cluster.Node(2)},
                            test::MakeCommitObjectCommand(
                                kBucket,
                                kObjectKey,
                                kObjectId,
                                "commit-object-t057-duplicate"),
                            8s,
                            &commit_result,
                            {stopped_voter_index}))
                << "duplicate join candidate replay must not pollute committed membership; "
                   "candidate cannot become voter without committed Raft membership change. "
                   "status="
                << ProposeStatusName(commit_result.status)
                << ", message=" << commit_result.message
                << ", cluster=" << cluster.DescribeCluster();

            std::string committed_replication_diagnostics;
            EXPECT_TRUE(WaitUntilCommittedObjectOnAllRunning(
                cluster,
                kBucket,
                kObjectKey,
                kObjectId,
                commit_result.log_index,
                5s,
                &committed_replication_diagnostics))
                << "duplicate join candidate replay leaked into committed membership or quorum; "
                   "surviving committed majority did not converge on COMMITTED object. values="
                << committed_replication_diagnostics
                << ", commit_status=" << ProposeStatusName(commit_result.status)
                << ", commit_message=" << commit_result.message
                << ", cluster=" << cluster.DescribeCluster();
        }

        TEST_F(IntegratedObjectStorageQuorumTest,
               PendingObservedJoinCandidatesDoNotPolluteCommittedMembershipOrQuorum)
        {
            constexpr const char *kBucket = "bucket-t057-pending";
            constexpr const char *kObjectKey = "objects/pending-join-candidates.bin";
            constexpr const char *kObjectId = "obj-t057-pending";
            constexpr const char *kViewClusterId = "cluster-t057-pending";
            const std::vector<int> kCommittedVoters{1, 2, 3};

            auto cluster = MakeCluster(3);
            cluster.StartAll();

            const auto original_leader = cluster.WaitForSingleLeader(8s);
            ASSERT_NE(original_leader, nullptr)
                << "3-voter cluster failed to elect leader before pending join boundary "
                   "test; cluster="
                << cluster.DescribeCluster();

            ProposeResult create_bucket_result;
            ASSERT_TRUE(test::ProposeCreateBucketWithRetry(
                            {cluster.Node(0), cluster.Node(1), cluster.Node(2)},
                            kBucket,
                            "create-bucket-t057-pending",
                            8s,
                            &create_bucket_result))
                << "CreateBucket should succeed before pending join boundary test; status="
                << ProposeStatusName(create_bucket_result.status)
                << ", message=" << create_bucket_result.message
                << ", cluster=" << cluster.DescribeCluster();

            std::size_t leader_index = 0;
            while (leader_index < cluster.Size() &&
                   cluster.Node(leader_index) != original_leader)
            {
                ++leader_index;
            }
            ASSERT_LT(leader_index, cluster.Size());

            const auto follower_indexes = cluster.OtherIndexes(leader_index);
            ASSERT_FALSE(follower_indexes.empty());
            const std::size_t stopped_voter_index = follower_indexes.front();
            cluster.StopNode(stopped_voter_index);

            const auto surviving_leader = cluster.WaitForSingleLeader(8s);
            ASSERT_NE(surviving_leader, nullptr)
                << "3-voter cluster should keep a legal leader with one stopped voter; "
                   "cluster="
                << cluster.DescribeCluster();

            ViewNodeRegistry registry;
            RegisterNodeOrAssert(
                &registry,
                MakeMetadataRegistration(kViewClusterId,
                                         "meta-committed-1",
                                         1,
                                         static_cast<std::uint16_t>(base_port_ + 121),
                                         100,
                                         MetadataMembershipObservedState::kVoter),
                "register-meta-committed-1");
            RegisterNodeOrAssert(
                &registry,
                MakeMetadataRegistration(kViewClusterId,
                                         "meta-committed-2",
                                         2,
                                         static_cast<std::uint16_t>(base_port_ + 122),
                                         101,
                                         MetadataMembershipObservedState::kVoter),
                "register-meta-committed-2");
            RegisterNodeOrAssert(
                &registry,
                MakeMetadataRegistration(kViewClusterId,
                                         "meta-committed-3",
                                         3,
                                         static_cast<std::uint16_t>(base_port_ + 123),
                                         102,
                                         MetadataMembershipObservedState::kVoter),
                "register-meta-committed-3");
            RegisterNodeOrAssert(
                &registry,
                MakeMetadataRegistration(kViewClusterId,
                                         "meta-join-candidate-a",
                                         51,
                                         static_cast<std::uint16_t>(base_port_ + 151),
                                         200,
                                         MetadataMembershipObservedState::kJoining),
                "register-meta-join-candidate-a");
            RegisterNodeOrAssert(
                &registry,
                MakeMetadataRegistration(kViewClusterId,
                                         "meta-join-candidate-b",
                                         52,
                                         static_cast<std::uint16_t>(base_port_ + 152),
                                         210,
                                         MetadataMembershipObservedState::kJoining),
                "register-meta-join-candidate-b");

            GetClusterViewRequest cluster_view_request;
            cluster_view_request.request_id = "cluster-view-t057-pending";
            cluster_view_request.cluster_id = kViewClusterId;
            cluster_view_request.include_dead_nodes = true;
            cluster_view_request.include_warnings = true;

            const auto cluster_view = registry.GetClusterView(cluster_view_request, 300);
            ASSERT_EQ(cluster_view.summary.status, ViewRegistryStatusCode::kOk);
            ASSERT_EQ(cluster_view.snapshot.metadata_nodes.size(), 5U);

            std::size_t joining_candidate_count = 0;
            for (const auto &metadata_node : cluster_view.snapshot.metadata_nodes)
            {
                if (metadata_node.node_id != "meta-join-candidate-a" &&
                    metadata_node.node_id != "meta-join-candidate-b")
                {
                    continue;
                }

                ++joining_candidate_count;
                ASSERT_TRUE(metadata_node.metadata.has_value());
                EXPECT_EQ(metadata_node.metadata->membership_state,
                          MetadataMembershipObservedState::kJoining);
                EXPECT_NE(metadata_node.metadata->raft_id, 1);
                EXPECT_NE(metadata_node.metadata->raft_id, 2);
                EXPECT_NE(metadata_node.metadata->raft_id, 3);
            }
            EXPECT_EQ(joining_candidate_count, 2U)
                << "pending join candidates should remain observed JOINING records only";

            ExpectCommittedMembershipUnchangedOnRunningNodes(cluster,
                                                            kCommittedVoters,
                                                            2U);

            ProposeResult create_object_result;
            ASSERT_TRUE(test::ProposeMetadataCommandWithRetry(
                            {cluster.Node(0), cluster.Node(1), cluster.Node(2)},
                            test::MakeCreateObjectCommand(
                                kBucket,
                                kObjectKey,
                                kObjectId,
                                "create-object-t057-pending"),
                            8s,
                            &create_object_result,
                            {stopped_voter_index}))
                << "pending join candidates must not raise committed quorum or become voters; "
                   "surviving 2-voter majority should still create metadata. status="
                << ProposeStatusName(create_object_result.status)
                << ", message=" << create_object_result.message
                << ", cluster=" << cluster.DescribeCluster();

            std::string pending_replication_diagnostics;
            ASSERT_TRUE(WaitUntilPendingObjectReplicatedOnAllRunning(
                            cluster,
                            kBucket,
                            kObjectKey,
                            kObjectId,
                            create_object_result.log_index,
                            5s,
                            &pending_replication_diagnostics))
                << "PENDING object did not replicate across surviving committed majority after "
                   "observing pending join candidates; values="
                << pending_replication_diagnostics
                << ", cluster=" << cluster.DescribeCluster();

            ProposeResult commit_result;
            ASSERT_TRUE(test::ProposeMetadataCommandWithRetry(
                            {cluster.Node(0), cluster.Node(1), cluster.Node(2)},
                            test::MakeCommitObjectCommand(
                                kBucket,
                                kObjectKey,
                                kObjectId,
                                "commit-object-t057-pending"),
                            8s,
                            &commit_result,
                            {stopped_voter_index}))
                << "pending join candidates must not pollute committed membership while "
                   "membership change is unimplemented; status="
                << ProposeStatusName(commit_result.status)
                << ", message=" << commit_result.message
                << ", cluster=" << cluster.DescribeCluster();

            std::string committed_replication_diagnostics;
            EXPECT_TRUE(WaitUntilCommittedObjectOnAllRunning(
                cluster,
                kBucket,
                kObjectKey,
                kObjectId,
                commit_result.log_index,
                5s,
                &committed_replication_diagnostics))
                << "pending join observations leaked into committed membership or quorum; "
                   "surviving committed majority did not converge on COMMITTED object. values="
                << committed_replication_diagnostics
                << ", commit_status=" << ProposeStatusName(commit_result.status)
                << ", commit_message=" << commit_result.message
                << ", cluster=" << cluster.DescribeCluster();
        }

        TEST_F(IntegratedObjectStorageQuorumTest,
               AddLearnerProposalPathRejectsFollowerAndPreservesDuplicatePendingBoundary)
        {
            constexpr const char *kClusterId = "cluster-t063-node";
            const std::vector<int> kCommittedVoters{1, 2, 3};

            auto cluster = MakeCluster(3);
            cluster.StartAll();

            const auto leader = cluster.WaitForSingleLeader(8s);
            ASSERT_NE(leader, nullptr)
                << "3-voter cluster failed to elect leader before AddLearner proposal "
                   "path test; cluster="
                << cluster.DescribeCluster();

            std::size_t leader_index = 0;
            while (leader_index < cluster.Size() &&
                   cluster.Node(leader_index) != leader)
            {
                ++leader_index;
            }
            ASSERT_LT(leader_index, cluster.Size());

            const auto follower_indexes = cluster.OtherIndexes(leader_index);
            ASSERT_FALSE(follower_indexes.empty());
            const auto follower = cluster.Node(follower_indexes.front());
            ASSERT_NE(follower, nullptr);

            const auto accepted_request = MakeAddLearnerProposalRequest(
                MakeJoinMetadataClusterRequest("req-add-learner-accepted",
                                               kClusterId,
                                               "meta-join-candidate-a",
                                               61,
                                               static_cast<std::uint16_t>(base_port_ + 1610),
                                               static_cast<std::uint16_t>(base_port_ + 2610)));
            const auto accepted_result = leader->ProposeAddLearner(accepted_request);
            EXPECT_EQ(accepted_result.status,
                      AddLearnerProposalStatus::kAcceptedPendingCommit)
                << accepted_result.message;
            EXPECT_FALSE(accepted_result.committed_membership_changed);
            EXPECT_EQ(accepted_result.canonical_node_id, accepted_request.node_id);
            EXPECT_EQ(accepted_result.assigned_raft_id,
                      accepted_request.candidate_raft_id);
            EXPECT_TRUE(Contains(accepted_result.message,
                                 "committed membership log proposal"))
                << accepted_result.message;

            const auto duplicate_result = leader->ProposeAddLearner(accepted_request);
            EXPECT_EQ(duplicate_result.status,
                      AddLearnerProposalStatus::kDuplicate)
                << duplicate_result.message;
            EXPECT_FALSE(duplicate_result.committed_membership_changed);

            auto conflicting_request = accepted_request;
            conflicting_request.candidate_client_address =
                "127.0.0.1:" + std::to_string(static_cast<std::uint32_t>(base_port_) + 1620);
            conflicting_request.candidate_raft_address =
                "127.0.0.1:" + std::to_string(static_cast<std::uint32_t>(base_port_) + 2620);
            conflicting_request.candidate_incarnation_id =
                "meta-join-candidate-a:boot:1710000001";
            const auto conflicting_result = leader->ProposeAddLearner(conflicting_request);
            EXPECT_EQ(conflicting_result.status,
                      AddLearnerProposalStatus::kRejected)
                << conflicting_result.message;
            EXPECT_FALSE(conflicting_result.committed_membership_changed);

            const auto pending_request = MakeAddLearnerProposalRequest(
                MakeJoinMetadataClusterRequest("req-add-learner-pending",
                                               kClusterId,
                                               "meta-join-candidate-b",
                                               62,
                                               static_cast<std::uint16_t>(base_port_ + 1630),
                                               static_cast<std::uint16_t>(base_port_ + 2630)));
            const auto pending_result = leader->ProposeAddLearner(pending_request);
            EXPECT_EQ(pending_result.status,
                      AddLearnerProposalStatus::kPendingMembershipChange)
                << pending_result.message;
            EXPECT_FALSE(pending_result.committed_membership_changed);

            const auto follower_result = follower->ProposeAddLearner(accepted_request);
            EXPECT_EQ(follower_result.status,
                      AddLearnerProposalStatus::kNotLeader)
                << follower_result.message;
            EXPECT_FALSE(follower_result.committed_membership_changed);

            ExpectCommittedMembershipUnchangedOnRunningNodes(cluster,
                                                            kCommittedVoters,
                                                            2U);
        }

        TEST_F(IntegratedObjectStorageQuorumTest,
               JoinMetadataClusterFollowerRejectsAuthorityAndReturnsLeaderHint)
        {
            constexpr const char *kClusterId = "cluster-t060-follower";
            const std::vector<int> kCommittedVoters{1, 2, 3};

            auto cluster = MakeCluster(3);
            cluster.StartAll();

            const auto leader = cluster.WaitForSingleLeader(8s);
            ASSERT_NE(leader, nullptr)
                << "3-voter cluster failed to elect leader before JoinMetadataCluster "
                   "follower validation test; cluster="
                << cluster.DescribeCluster();

            std::size_t leader_index = 0;
            while (leader_index < cluster.Size() &&
                   cluster.Node(leader_index) != leader)
            {
                ++leader_index;
            }
            ASSERT_LT(leader_index, cluster.Size());

            const auto follower_indexes = cluster.OtherIndexes(leader_index);
            ASSERT_FALSE(follower_indexes.empty());
            const std::size_t follower_index = follower_indexes.front();

            const NodeStatusSnapshot leader_status = leader->GetStatusSnapshot();
            const NodeStatusSnapshot follower_status =
                cluster.Node(follower_index)->GetStatusSnapshot();

            raft::JoinMetadataClusterRequest request =
                MakeJoinMetadataClusterRequest("req-join-follower-authority",
                                               kClusterId,
                                               "meta-join-candidate-follower",
                                               41,
                                               static_cast<std::uint16_t>(base_port_ + 1410),
                                               static_cast<std::uint16_t>(base_port_ + 2410));
            raft::JoinMetadataClusterResponse response;
            const grpc::Status rpc_status =
                JoinMetadataClusterViaAddress(follower_status.address, request, &response);

            ASSERT_TRUE(rpc_status.ok()) << rpc_status.error_message();
            EXPECT_EQ(response.summary().code(), raft::METADATA_STATUS_CODE_NOT_LEADER);
            EXPECT_EQ(response.disposition(),
                      raft::JOIN_METADATA_CLUSTER_DISPOSITION_NOT_LEADER);
            EXPECT_EQ(response.requested_membership(),
                      raft::JOIN_METADATA_TARGET_MEMBERSHIP_LEARNER);
            EXPECT_FALSE(response.committed_membership_changed());
            EXPECT_EQ(response.summary().request_id(), request.request_id());
            EXPECT_EQ(response.summary().leader_hint().leader_id(), leader_status.node_id);
            EXPECT_EQ(response.summary().leader_hint().leader_address(),
                      leader_status.address);

            ExpectCommittedMembershipUnchangedOnRunningNodes(cluster,
                                                            kCommittedVoters,
                                                            2U);
        }

        TEST_F(IntegratedObjectStorageQuorumTest,
               JoinMetadataClusterLeaderValidatesInvalidDuplicateAndPendingWithoutChangingCommittedMembership)
        {
            constexpr const char *kClusterId = "cluster-t060-leader";
            const std::vector<int> kCommittedVoters{1, 2, 3};

            auto cluster = MakeCluster(3);
            cluster.StartAll();

            const auto leader = cluster.WaitForSingleLeader(8s);
            ASSERT_NE(leader, nullptr)
                << "3-voter cluster failed to elect leader before JoinMetadataCluster "
                   "leader validation test; cluster="
                << cluster.DescribeCluster();

            const NodeStatusSnapshot leader_status = leader->GetStatusSnapshot();

            raft::JoinMetadataClusterRequest invalid_request =
                MakeJoinMetadataClusterRequest("req-join-invalid",
                                               kClusterId,
                                               "meta-join-invalid",
                                               0,
                                               static_cast<std::uint16_t>(base_port_ + 1510),
                                               static_cast<std::uint16_t>(base_port_ + 2510));
            invalid_request.set_candidate_raft_id(0);
            raft::JoinMetadataClusterResponse invalid_response;
            grpc::Status rpc_status =
                JoinMetadataClusterViaAddress(leader_status.address,
                                             invalid_request,
                                             &invalid_response);
            ASSERT_TRUE(rpc_status.ok()) << rpc_status.error_message();
            EXPECT_EQ(invalid_response.summary().code(),
                      raft::METADATA_STATUS_CODE_INVALID_ARGUMENT);
            EXPECT_EQ(invalid_response.disposition(),
                      raft::JOIN_METADATA_CLUSTER_DISPOSITION_INVALID_CANDIDATE);
            EXPECT_FALSE(invalid_response.committed_membership_changed());

            raft::JoinMetadataClusterRequest accepted_request =
                MakeJoinMetadataClusterRequest("req-join-accepted",
                                               kClusterId,
                                               "meta-join-candidate-a",
                                               51,
                                               static_cast<std::uint16_t>(base_port_ + 1520),
                                               static_cast<std::uint16_t>(base_port_ + 2520));
            raft::JoinMetadataClusterResponse accepted_response;
            rpc_status = JoinMetadataClusterViaAddress(leader_status.address,
                                                       accepted_request,
                                                       &accepted_response);
            ASSERT_TRUE(rpc_status.ok()) << rpc_status.error_message();
            EXPECT_EQ(accepted_response.summary().code(), raft::METADATA_STATUS_CODE_OK);
            EXPECT_EQ(accepted_response.disposition(),
                      raft::JOIN_METADATA_CLUSTER_DISPOSITION_ACCEPTED_PENDING_COMMIT);
            EXPECT_EQ(accepted_response.requested_membership(),
                      raft::JOIN_METADATA_TARGET_MEMBERSHIP_LEARNER);
            EXPECT_FALSE(accepted_response.committed_membership_changed());
            EXPECT_EQ(accepted_response.canonical_node_id(),
                      accepted_request.node_id());
            EXPECT_EQ(accepted_response.assigned_raft_id(),
                      accepted_request.candidate_raft_id());

            raft::JoinMetadataClusterResponse duplicate_response;
            rpc_status = JoinMetadataClusterViaAddress(leader_status.address,
                                                       accepted_request,
                                                       &duplicate_response);
            ASSERT_TRUE(rpc_status.ok()) << rpc_status.error_message();
            EXPECT_EQ(duplicate_response.summary().code(),
                      raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY);
            EXPECT_EQ(duplicate_response.disposition(),
                      raft::JOIN_METADATA_CLUSTER_DISPOSITION_DUPLICATE);
            EXPECT_FALSE(duplicate_response.committed_membership_changed());

            raft::JoinMetadataClusterRequest pending_request =
                MakeJoinMetadataClusterRequest("req-join-pending",
                                               kClusterId,
                                               "meta-join-candidate-b",
                                               52,
                                               static_cast<std::uint16_t>(base_port_ + 1530),
                                               static_cast<std::uint16_t>(base_port_ + 2530));
            raft::JoinMetadataClusterResponse pending_response;
            rpc_status = JoinMetadataClusterViaAddress(leader_status.address,
                                                       pending_request,
                                                       &pending_response);
            ASSERT_TRUE(rpc_status.ok()) << rpc_status.error_message();
            EXPECT_EQ(pending_response.summary().code(),
                      raft::METADATA_STATUS_CODE_STATE_CONFLICT);
            EXPECT_EQ(pending_response.disposition(),
                      raft::JOIN_METADATA_CLUSTER_DISPOSITION_PENDING_MEMBERSHIP_CHANGE);
            EXPECT_FALSE(pending_response.committed_membership_changed());

            raft::JoinMetadataClusterRequest conflicting_request =
                MakeJoinMetadataClusterRequest("req-join-conflict",
                                               kClusterId,
                                               accepted_request.node_id(),
                                               accepted_request.candidate_raft_id(),
                                               static_cast<std::uint16_t>(base_port_ + 1540),
                                               static_cast<std::uint16_t>(base_port_ + 2540));
            raft::JoinMetadataClusterResponse conflicting_response;
            rpc_status = JoinMetadataClusterViaAddress(leader_status.address,
                                                       conflicting_request,
                                                       &conflicting_response);
            ASSERT_TRUE(rpc_status.ok()) << rpc_status.error_message();
            EXPECT_EQ(conflicting_response.summary().code(),
                      raft::METADATA_STATUS_CODE_STATE_CONFLICT);
            EXPECT_EQ(conflicting_response.disposition(),
                      raft::JOIN_METADATA_CLUSTER_DISPOSITION_REJECTED);
            EXPECT_FALSE(conflicting_response.committed_membership_changed());

            ExpectCommittedMembershipUnchangedOnRunningNodes(cluster,
                                                            kCommittedVoters,
                                                            2U);
        }
    } // namespace
} // namespace raftdemo
