#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "raft/common/config.h"
#include "raft/common/metadata_command.h"
#include "raft/runtime/logging.h"
#include "raft/common/propose.h"
#include "raft/node/raft_node.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "metadata_raft_test_utils.h"

namespace raftdemo
{
    namespace
    {

        using namespace std::chrono_literals;

        struct RunningCluster
        {
            RunningCluster() = default;
            RunningCluster(const RunningCluster &) = delete;
            RunningCluster &operator=(const RunningCluster &) = delete;

            RunningCluster(RunningCluster &&other) noexcept
                : nodes(std::move(other.nodes)), threads(std::move(other.threads)) {}

            RunningCluster &operator=(RunningCluster &&other) noexcept
            {
                if (this != &other)
                {
                    Stop();
                    nodes = std::move(other.nodes);
                    threads = std::move(other.threads);
                }
                return *this;
            }

            ~RunningCluster() { Stop(); }

            void Stop()
            {
                for (auto &node : nodes)
                {
                    if (node)
                    {
                        node->Stop();
                    }
                }
                for (auto &t : threads)
                {
                    if (t.joinable())
                    {
                        t.join();
                    }
                }
                threads.clear();
                nodes.clear();
            }

            std::vector<std::shared_ptr<RaftNode>> nodes;
            std::vector<std::thread> threads;
        };

        std::string GetEnvOrDefault(const char *key, const std::string &fallback)
        {
            const char *value = std::getenv(key);
            if (value == nullptr || *value == '\0')
            {
                return fallback;
            }
            return value;
        }

        std::filesystem::path TestBinaryDir()
        {
#ifdef RAFT_TEST_BINARY_DIR
            return std::filesystem::path(RAFT_TEST_BINARY_DIR);
#else
            return std::filesystem::current_path();
#endif
        }

        std::uint64_t NowForPath()
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
        }

        std::string UniqueRoot(const std::string &suffix)
        {
            const auto base = TestBinaryDir() / "raft_test_data" / "snapshot_recovery";
            std::filesystem::create_directories(base);
            return (base / (suffix + "_" + std::to_string(NowForPath()))).string();
        }

        std::vector<NodeConfig> BuildThreeNodeConfigs(const std::string &data_root, int base_port)
        {
            NodeConfig n1;
            n1.node_id = 1;
            n1.address = "127.0.0.1:" + std::to_string(base_port + 1);
            n1.peers = {
                PeerConfig{2, "127.0.0.1:" + std::to_string(base_port + 2)},
                PeerConfig{3, "127.0.0.1:" + std::to_string(base_port + 3)},
            };
            n1.election_timeout_min = 300ms;
            n1.election_timeout_max = 600ms;
            n1.heartbeat_interval = 80ms;
            n1.rpc_deadline = 500ms;
            n1.data_dir = (std::filesystem::path(data_root) / "node_1").string();

            NodeConfig n2;
            n2.node_id = 2;
            n2.address = "127.0.0.1:" + std::to_string(base_port + 2);
            n2.peers = {
                PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
                PeerConfig{3, "127.0.0.1:" + std::to_string(base_port + 3)},
            };
            n2.election_timeout_min = 300ms;
            n2.election_timeout_max = 600ms;
            n2.heartbeat_interval = 80ms;
            n2.rpc_deadline = 500ms;
            n2.data_dir = (std::filesystem::path(data_root) / "node_2").string();

            NodeConfig n3;
            n3.node_id = 3;
            n3.address = "127.0.0.1:" + std::to_string(base_port + 3);
            n3.peers = {
                PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
                PeerConfig{2, "127.0.0.1:" + std::to_string(base_port + 2)},
            };
            n3.election_timeout_min = 300ms;
            n3.election_timeout_max = 600ms;
            n3.heartbeat_interval = 80ms;
            n3.rpc_deadline = 500ms;
            n3.data_dir = (std::filesystem::path(data_root) / "node_3").string();

            return {n1, n2, n3};
        }

        std::vector<snapshotConfig> BuildSnapshotConfigs(const std::string &snapshot_root)
        {
            snapshotConfig s1;
            s1.enabled = true;
            s1.snapshot_dir = (std::filesystem::path(snapshot_root) / "node_1").string();
            s1.log_threshold = 20;
            s1.snapshot_interval = 10min;
            s1.max_snapshot_count = 5;
            s1.load_on_startup = true;
            s1.file_prefix = "snapshot";

            snapshotConfig s2 = s1;
            s2.snapshot_dir = (std::filesystem::path(snapshot_root) / "node_2").string();

            snapshotConfig s3 = s1;
            s3.snapshot_dir = (std::filesystem::path(snapshot_root) / "node_3").string();

            return {s1, s2, s3};
        }

        RunningCluster StartCluster(const std::vector<NodeConfig> &node_configs,
                                    const std::vector<snapshotConfig> &snapshot_configs)
        {
            RunningCluster cluster;
            cluster.nodes.reserve(node_configs.size());
            cluster.threads.reserve(node_configs.size());

            for (std::size_t i = 0; i < node_configs.size(); ++i)
            {
                cluster.nodes.push_back(std::make_shared<RaftNode>(node_configs[i], snapshot_configs[i]));
            }

            for (const auto &node : cluster.nodes)
            {
                cluster.threads.emplace_back([node]()
                                             {
      node->Start();
      node->Wait(); });
            }

            return cluster;
        }

        void StopCluster(RunningCluster *cluster)
        {
            if (cluster != nullptr)
            {
                cluster->Stop();
            }
        }

        bool IsLeaderNode(const std::shared_ptr<RaftNode> &node)
        {
            return node != nullptr && node->Describe().find("role=Leader") != std::string::npos;
        }

        std::string DescribeCluster(const std::vector<std::shared_ptr<RaftNode>> &nodes)
        {
            std::ostringstream oss;
            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                if (i != 0)
                {
                    oss << " | ";
                }
                oss << "node[" << i << "]=";
                if (!nodes[i])
                {
                    oss << "null";
                }
                else
                {
                    oss << nodes[i]->Describe();
                }
            }
            return oss.str();
        }

        struct StableLeaderObservation
        {
            std::shared_ptr<RaftNode> leader;
            std::size_t leader_index{0};
            int stable_observations{0};
            std::string diagnostics;
        };

        std::optional<StableLeaderObservation> WaitForStableLeader(
            const std::vector<std::shared_ptr<RaftNode>> &nodes,
            std::chrono::milliseconds timeout,
            int required_observations = 3)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            std::size_t last_leader_index = nodes.size();
            int stable_observations = 0;
            int last_leader_count = 0;
            std::string last_cluster_state;

            while (std::chrono::steady_clock::now() < deadline)
            {
                std::shared_ptr<RaftNode> leader;
                std::size_t leader_index = nodes.size();
                int leader_count = 0;

                for (std::size_t i = 0; i < nodes.size(); ++i)
                {
                    if (IsLeaderNode(nodes[i]))
                    {
                        leader = nodes[i];
                        leader_index = i;
                        ++leader_count;
                    }
                }

                last_leader_count = leader_count;
                last_cluster_state = DescribeCluster(nodes);

                if (leader_count == 1)
                {
                    if (leader_index == last_leader_index)
                    {
                        ++stable_observations;
                    }
                    else
                    {
                        last_leader_index = leader_index;
                        stable_observations = 1;
                    }

                    if (stable_observations >= required_observations)
                    {
                        StableLeaderObservation observation;
                        observation.leader = leader;
                        observation.leader_index = leader_index;
                        observation.stable_observations = stable_observations;
                        observation.diagnostics =
                            "stable leader_index=" + std::to_string(leader_index) +
                            ", observations=" + std::to_string(stable_observations) +
                            ", leader=" + leader->Describe() +
                            ", cluster=" + last_cluster_state;
                        return observation;
                    }
                }
                else
                {
                    last_leader_index = nodes.size();
                    stable_observations = 0;
                }

                std::this_thread::sleep_for(50ms);
            }

            return std::nullopt;
        }

        std::shared_ptr<RaftNode> WaitForLeader(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                                                std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                for (const auto &node : nodes)
                {
                    if (IsLeaderNode(node))
                    {
                        return node;
                    }
                }
                std::this_thread::sleep_for(50ms);
            }
            return nullptr;
        }

        bool HasDirectorySnapshot(const snapshotConfig &cfg)
        {
            std::error_code ec;
            if (!std::filesystem::exists(cfg.snapshot_dir, ec))
            {
                return false;
            }

            for (const auto &entry : std::filesystem::directory_iterator(cfg.snapshot_dir, ec))
            {
                if (ec)
                {
                    return false;
                }
                const auto snapshot_dir = entry.path();
                const auto name = snapshot_dir.filename().string();
                if (!entry.is_directory(ec) || name.rfind(cfg.file_prefix + "_", 0) != 0)
                {
                    continue;
                }
                if (std::filesystem::exists(snapshot_dir / "data.bin", ec) &&
                    std::filesystem::exists(snapshot_dir / "__raft_snapshot_meta", ec))
                {
                    return true;
                }
            }
            return false;
        }

        bool WaitForSnapshots(const std::vector<snapshotConfig> &snapshot_configs,
                              std::chrono::milliseconds timeout,
                              std::string *diagnostics = nullptr)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            const std::size_t required = snapshot_configs.size() / 2 + 1;
            std::size_t last_ready = 0;
            while (std::chrono::steady_clock::now() < deadline)
            {
                std::size_t ready = 0;
                for (const auto &cfg : snapshot_configs)
                {
                    if (HasDirectorySnapshot(cfg))
                    {
                        ++ready;
                    }
                }
                last_ready = ready;
                if (ready >= required)
                {
                    if (diagnostics != nullptr)
                    {
                        *diagnostics = "snapshot majority reached, ready=" +
                                       std::to_string(ready) +
                                       ", required=" + std::to_string(required);
                    }
                    return true;
                }
                std::this_thread::sleep_for(100ms);
            }
            if (diagnostics != nullptr)
            {
                *diagnostics = "snapshot majority not reached, ready=" +
                               std::to_string(last_ready) +
                               ", required=" + std::to_string(required);
            }
            return false;
        }

        bool ProposeWithStableLeaderRetry(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                                          const Command &command,
                                          std::chrono::milliseconds timeout,
                                          ProposeResult *final_result,
                                          std::string *diagnostics = nullptr)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            ProposeResult last_result;
            std::size_t last_leader_index = nodes.size();
            std::string last_leader_describe = "none";
            std::string last_cluster_state = DescribeCluster(nodes);
            int attempts = 0;
            int no_stable_leader_rounds = 0;

            while (std::chrono::steady_clock::now() < deadline)
            {
                auto stable_leader = WaitForStableLeader(nodes, 1500ms);
                if (!stable_leader.has_value())
                {
                    ++no_stable_leader_rounds;
                    last_cluster_state = DescribeCluster(nodes);
                    std::this_thread::sleep_for(50ms);
                    continue;
                }

                ++attempts;
                last_leader_index = stable_leader->leader_index;
                last_leader_describe = stable_leader->leader->Describe();
                last_cluster_state = DescribeCluster(nodes);

                last_result = stable_leader->leader->Propose(command);
                if (last_result.Ok())
                {
                    if (final_result != nullptr)
                    {
                        *final_result = last_result;
                    }
                    if (diagnostics != nullptr)
                    {
                        *diagnostics = "proposal committed, attempts=" + std::to_string(attempts) +
                                       ", leader_index=" + std::to_string(last_leader_index) +
                                       ", leader=" + last_leader_describe +
                                       ", cluster=" + last_cluster_state;
                    }
                    return true;
                }

                if (last_result.status == ProposeStatus::kInvalidCommand ||
                    last_result.status == ProposeStatus::kApplyFailed ||
                    last_result.status == ProposeStatus::kCommitFailed)
                {
                    break;
                }

                std::this_thread::sleep_for(100ms);
            }

            if (final_result != nullptr)
            {
                *final_result = last_result;
            }
            if (diagnostics != nullptr)
            {
                std::string category = "proposal_failure";
                if (no_stable_leader_rounds > 0 && attempts == 0)
                {
                    category = "leader_not_stable_before_propose";
                }
                else if (last_result.status == ProposeStatus::kNotLeader)
                {
                    category = "leadership_churn_during_propose";
                }
                else if (last_result.status == ProposeStatus::kReplicationFailed ||
                         last_result.status == ProposeStatus::kTimeout ||
                         last_result.status == ProposeStatus::kCommitFailed)
                {
                    category = "proposal_failed_before_majority_or_commit";
                }
                else if (last_result.status == ProposeStatus::kApplyFailed)
                {
                    category = "proposal_committed_but_apply_failed";
                }

                *diagnostics = "category=" + category +
                               ", attempts=" + std::to_string(attempts) +
                               ", no_stable_leader_rounds=" + std::to_string(no_stable_leader_rounds) +
                               ", last_leader_index=" + std::to_string(last_leader_index) +
                               ", last_leader=" + last_leader_describe +
                               ", last_status=" + std::to_string(static_cast<int>(last_result.status)) +
                               ", last_message=" + last_result.message +
                               ", cluster=" + last_cluster_state;
            }
            return false;
        }

        void LogClusterState(const std::string &title,
                             const std::vector<std::shared_ptr<RaftNode>> &nodes)
        {
            Log("snapshot_recovery_gtest", title);
            for (const auto &node : nodes)
            {
                Log("snapshot_recovery_gtest", node->Describe());
            }
        }

        void LogSnapshotFiles(const std::vector<snapshotConfig> &snapshot_configs)
        {
            for (const auto &cfg : snapshot_configs)
            {
                std::error_code ec;
                Log("snapshot_recovery_gtest", "snapshot dir=", cfg.snapshot_dir);
                if (!std::filesystem::exists(cfg.snapshot_dir, ec))
                {
                    Log("snapshot_recovery_gtest", "  (not exists)");
                    continue;
                }
                for (const auto &entry : std::filesystem::recursive_directory_iterator(cfg.snapshot_dir, ec))
                {
                    if (ec)
                    {
                        break;
                    }
                    Log("snapshot_recovery_gtest", "  path=", entry.path().string());
                }
            }
        }

        TEST(RaftSnapshotRecoveryTest, SavesSnapshotAndRestoresAfterRestart)
        {
            const std::string data_root = GetEnvOrDefault(
                "RAFT_TEST_DATA_ROOT", UniqueRoot("snapshot_recovery_data"));
            const std::string snapshot_root = GetEnvOrDefault(
                "RAFT_TEST_SNAPSHOT_ROOT", UniqueRoot("snapshot_recovery_snapshots"));

            constexpr int kBasePort = 56050;

            std::filesystem::remove_all(data_root);
            std::filesystem::remove_all(snapshot_root);

            const auto node_configs = BuildThreeNodeConfigs(data_root, kBasePort);
            const auto snapshot_configs = BuildSnapshotConfigs(snapshot_root);

            Log("snapshot_recovery_gtest", "data_root=", data_root);
            Log("snapshot_recovery_gtest", "snapshot_root=", snapshot_root);
            Log("snapshot_recovery_gtest", "phase-1 start cluster and write logs");

            auto cluster = StartCluster(node_configs, snapshot_configs);
            auto stable_leader = WaitForStableLeader(cluster.nodes, 6s);
            ASSERT_TRUE(stable_leader.has_value())
                << "leader not stable in phase-1, cluster=" << DescribeCluster(cluster.nodes);

            LogClusterState("phase-1 leader elected", cluster.nodes);

            const std::string bucket = "snapshot-recovery-bucket";
            ProposeResult result;
            ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                cluster.nodes,
                raftdemo::test::MakeCreateBucketCommand(
                    bucket, "snapshot-recovery-create-bucket-1"),
                10s,
                &result));

            std::vector<raftdemo::test::ExpectedRecoveredMetadataObject> expected_objects;
            std::vector<std::string> visible_keys;
            for (int i = 0; i < 25; ++i)
            {
                const std::string suffix = (i < 10 ? "0" : "") + std::to_string(i);
                const std::string key = "k" + suffix;
                const std::string object_id = "obj-" + key;
                ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                    cluster.nodes,
                    raftdemo::test::MakeCreateObjectCommand(
                        bucket, key, object_id,
                        "snapshot-recovery-create-" + suffix),
                    10s,
                    &result));
                ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                    cluster.nodes,
                    raftdemo::test::MakeCommitObjectCommand(
                        bucket, key, object_id,
                        "snapshot-recovery-commit-" + suffix),
                    10s,
                    &result));
                expected_objects.push_back({key, object_id, 2U, false});
                visible_keys.push_back(key);
            }

            ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                cluster.nodes,
                raftdemo::test::MakeCreateObjectCommand(
                    bucket, "deleted_anchor", "obj-deleted-anchor",
                    "snapshot-recovery-create-deleted-anchor"),
                10s,
                &result));
            ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                cluster.nodes,
                raftdemo::test::MakeCommitObjectCommand(
                    bucket, "deleted_anchor", "obj-deleted-anchor",
                    "snapshot-recovery-commit-deleted-anchor"),
                10s,
                &result));
            ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                cluster.nodes,
                raftdemo::test::MakeDeleteObjectCommand(
                    bucket, "deleted_anchor", "obj-deleted-anchor",
                    "snapshot-recovery-delete-deleted-anchor"),
                10s,
                &result));

            expected_objects.push_back(
                {"deleted_anchor", "obj-deleted-anchor", 0U, true});
            std::sort(visible_keys.begin(), visible_keys.end());

            const raftdemo::test::MetadataRecoveryExpectation expected_before_restart{
                .bucket = bucket,
                .objects = expected_objects,
                .visible_keys = visible_keys,
                .expected_request_count = 54U,
                .expected_tombstone_count = 1U,
                .expected_last_applied_index = result.log_index,
                .expected_min_last_applied_term = result.term,
            };
            ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
                cluster.nodes, expected_before_restart, 8s));

            std::string snapshot_diagnostics;
            ASSERT_TRUE(WaitForSnapshots(snapshot_configs, 10s, &snapshot_diagnostics))
                << "snapshot files were not created for a majority of nodes, diagnostics="
                << snapshot_diagnostics;

            LogClusterState("phase-1 after 25 commands", cluster.nodes);
            LogSnapshotFiles(snapshot_configs);

            StopCluster(&cluster);

            Log("snapshot_recovery_gtest", "phase-2 restart cluster and verify recovery");

            auto restarted = StartCluster(node_configs, snapshot_configs);
            auto restarted_stable_leader = WaitForStableLeader(restarted.nodes, 6s);
            ASSERT_TRUE(restarted_stable_leader.has_value())
                << "leader not stable after restart, cluster=" << DescribeCluster(restarted.nodes);

            LogClusterState("phase-2 after restart before new propose", restarted.nodes);

            ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
                restarted.nodes, expected_before_restart, 8s));
            for (const auto &node : restarted.nodes)
            {
                const MetadataStateMachine *state_machine = node->GetMetadataStateMachineV2();
                ASSERT_NE(state_machine, nullptr);
                EXPECT_EQ(state_machine->RequestCount(), 54U);
                EXPECT_EQ(state_machine->TombstoneCount(), 1U);
                EXPECT_GE(state_machine->LastAppliedTerm(), result.term);
                EXPECT_GE(state_machine->LastAppliedIndex(), expected_before_restart.expected_last_applied_index);
            }

            ProposeResult after_restart_result;
            ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                restarted.nodes,
                raftdemo::test::MakeCreateObjectCommand(
                    bucket, "after_restart", "obj-after-restart",
                    "snapshot-recovery-create-after-restart"),
                10s,
                &after_restart_result));
            ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                restarted.nodes,
                raftdemo::test::MakeCommitObjectCommand(
                    bucket, "after_restart", "obj-after-restart",
                    "snapshot-recovery-commit-after-restart"),
                10s,
                &after_restart_result));

            auto expected_after_restart = expected_before_restart;
            expected_after_restart.objects.push_back(
                {"after_restart", "obj-after-restart", 2U, false});
            expected_after_restart.visible_keys.push_back("after_restart");
            std::sort(expected_after_restart.visible_keys.begin(),
                      expected_after_restart.visible_keys.end());
            expected_after_restart.expected_request_count = 56U;
            expected_after_restart.expected_last_applied_index = after_restart_result.log_index;
            expected_after_restart.expected_min_last_applied_term = after_restart_result.term;

            ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
                restarted.nodes, expected_after_restart, 8s));

            LogClusterState("phase-2 after recovery and new propose", restarted.nodes);
            LogSnapshotFiles(snapshot_configs);

            StopCluster(&restarted);
        }

    } // namespace
} // namespace raftdemo
