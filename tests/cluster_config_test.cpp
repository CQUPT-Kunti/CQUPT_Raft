#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cluster/cluster_config.h"

namespace clusterdemo
{
    namespace
    {
        constexpr std::uint64_t kDefaultStorageCapacityBytes = 64ULL * 1024ULL * 1024ULL;

        ClusterTimeoutConfig MakeValidTimeoutConfig()
        {
            return ClusterTimeoutConfig{
                .discovery_rpc_timeout = std::chrono::milliseconds(500),
                .metadata_rpc_timeout = std::chrono::milliseconds(800),
                .storage_rpc_timeout = std::chrono::milliseconds(1200),
                .heartbeat_interval = std::chrono::milliseconds(1000),
                .registration_timeout = std::chrono::milliseconds(3000),
                .commit_deadline = std::chrono::milliseconds(5000),
                .liveness_stale_timeout = std::chrono::milliseconds(4000),
                .liveness_dead_timeout = std::chrono::milliseconds(9000),
            };
        }

        ChunkPolicyConfig MakeValidChunkPolicy()
        {
            return ChunkPolicyConfig{
                .chunk_size_bytes = 4ULL * 1024ULL * 1024ULL,
                .replica_count = 3,
                .minimum_successful_writes = 2,
                .checksum_algorithm = ClusterChecksumAlgorithm::kSha256,
            };
        }

        ClusterConfigGenerationRequest MakeGenerationRequest(
            const std::size_t metadata_voter_count)
        {
            return ClusterConfigGenerationRequest{
                .cluster_id = "cluster-config-test-" + std::to_string(metadata_voter_count),
                .base_dir = std::filesystem::temp_directory_path() /
                            "cqupt_raft_cluster_config_test" /
                            ("layout-" + std::to_string(metadata_voter_count)),
                .bind_host = "127.0.0.1",
                .advertise_host = "",
                .view_node_count = 1,
                .metadata_node_count = metadata_voter_count,
                .metadata_voter_count = metadata_voter_count,
                .storage_node_count = 3,
                .view_port_base = static_cast<std::uint16_t>(21000 + metadata_voter_count * 100),
                .metadata_port_base = static_cast<std::uint16_t>(22000 + metadata_voter_count * 100),
                .storage_port_base = static_cast<std::uint16_t>(23000 + metadata_voter_count * 100),
                .default_storage_capacity_bytes = kDefaultStorageCapacityBytes,
                .chunk_policy = MakeValidChunkPolicy(),
                .timeouts = MakeValidTimeoutConfig(),
                .fixed_view_node_ids = {},
                .fixed_metadata_node_ids = {},
                .fixed_metadata_raft_ids = {},
                .fixed_storage_node_ids = {},
                .storage_capacity_overrides_bytes = {
                    kDefaultStorageCapacityBytes * 2ULL,
                    kDefaultStorageCapacityBytes * 3ULL,
                },
                .generation_seed = 1000 + metadata_voter_count,
            };
        }

        bool ContainsIssue(const ClusterConfigValidationResult &validation,
                           const ClusterConfigIssueCode code)
        {
            for (const ClusterConfigValidationIssue &issue : validation.issues)
            {
                if (issue.code == code)
                {
                    return true;
                }
            }
            return false;
        }

        std::string BuildConfigSignature(const ClusterConfig &config)
        {
            std::ostringstream oss;
            oss << config.cluster_id << '\n'
                << config.base_dir.generic_string() << '\n'
                << config.initial_raft_membership.membership_epoch << '\n';

            for (const ViewNodeConfig &node : config.view_nodes)
            {
                oss << "view|"
                    << node.node_id.value_or("") << '|'
                    << node.endpoint << '|'
                    << node.data_dir.generic_string() << '\n';
            }

            for (const MetadataNodeConfig &node : config.metadata_nodes)
            {
                oss << "meta|"
                    << node.node_id << '|'
                    << node.raft_id << '|'
                    << node.endpoint << '|'
                    << node.data_dir.generic_string() << '|'
                    << node.snapshot_dir.generic_string() << '|'
                    << ToString(node.initial_role) << '\n';
            }

            for (const StorageNodeConfig &node : config.storage_nodes)
            {
                oss << "store|"
                    << node.node_id.value_or("") << '|'
                    << node.endpoint << '|'
                    << node.data_dir.generic_string() << '|'
                    << node.capacity_bytes << '\n';
            }

            oss << "voters:";
            for (const std::int32_t raft_id : config.initial_raft_membership.voter_raft_ids)
            {
                oss << raft_id << ',';
            }
            oss << "\nlearners:";
            for (const std::int32_t raft_id : config.initial_raft_membership.learner_raft_ids)
            {
                oss << raft_id << ',';
            }

            return oss.str();
        }

        void ExpectUniqueAndNonEmpty(const std::vector<std::string> &values)
        {
            std::set<std::string> unique_values;
            for (const std::string &value : values)
            {
                EXPECT_FALSE(value.empty());
                unique_values.insert(value);
            }
            EXPECT_EQ(unique_values.size(), values.size());
        }

        void ExpectValidGeneratedTopology(const ClusterConfigGenerationRequest &request,
                                          const ClusterConfigGenerationResult &result)
        {
            ASSERT_TRUE(result.ok()) << result.error_detail;
            EXPECT_TRUE(result.validation.ok()) << result.error_detail;
            EXPECT_EQ(result.status, ClusterConfigStatusCode::kOk);
            EXPECT_EQ(result.config.cluster_id, request.cluster_id);
            EXPECT_EQ(result.config.base_dir, request.base_dir);
            EXPECT_EQ(result.config.view_nodes.size(), request.view_node_count);
            EXPECT_EQ(result.config.metadata_nodes.size(), request.metadata_node_count);
            EXPECT_EQ(result.config.storage_nodes.size(), request.storage_node_count);
            EXPECT_EQ(result.config.initial_raft_membership.membership_epoch,
                      request.generation_seed.value_or(1));
            EXPECT_EQ(result.config.initial_raft_membership.voter_raft_ids.size(),
                      request.metadata_voter_count);
            EXPECT_TRUE(result.config.initial_raft_membership.learner_raft_ids.empty());

            std::vector<std::string> node_ids;
            std::vector<std::string> endpoints;
            std::vector<std::string> paths;
            std::vector<std::int32_t> metadata_raft_ids;

            for (const ViewNodeConfig &node : result.config.view_nodes)
            {
                ASSERT_TRUE(node.node_id.has_value());
                node_ids.push_back(*node.node_id);
                endpoints.push_back(node.endpoint);
                paths.push_back(node.data_dir.generic_string());
            }

            for (std::size_t index = 0; index < result.config.metadata_nodes.size(); ++index)
            {
                const MetadataNodeConfig &node = result.config.metadata_nodes[index];
                node_ids.push_back(node.node_id);
                endpoints.push_back(node.endpoint);
                paths.push_back(node.data_dir.generic_string());
                paths.push_back(node.snapshot_dir.generic_string());
                metadata_raft_ids.push_back(node.raft_id);

                EXPECT_GT(node.raft_id, 0);
                EXPECT_EQ(node.initial_role, MetadataNodeInitialRole::kVoter);
                EXPECT_EQ(node.endpoint,
                          request.bind_host + ":" +
                              std::to_string(request.metadata_port_base + index));
            }

            for (std::size_t index = 0; index < result.config.storage_nodes.size(); ++index)
            {
                const StorageNodeConfig &node = result.config.storage_nodes[index];
                ASSERT_TRUE(node.node_id.has_value());
                node_ids.push_back(*node.node_id);
                endpoints.push_back(node.endpoint);
                paths.push_back(node.data_dir.generic_string());

                const std::uint64_t expected_capacity =
                    index < request.storage_capacity_overrides_bytes.size()
                        ? request.storage_capacity_overrides_bytes[index]
                        : request.default_storage_capacity_bytes;
                EXPECT_EQ(node.capacity_bytes, expected_capacity);
                EXPECT_GT(node.capacity_bytes, 0U);
            }

            ExpectUniqueAndNonEmpty(node_ids);
            ExpectUniqueAndNonEmpty(endpoints);
            ExpectUniqueAndNonEmpty(paths);

            std::set<std::int32_t> unique_raft_ids(metadata_raft_ids.begin(),
                                                   metadata_raft_ids.end());
            EXPECT_EQ(unique_raft_ids.size(), metadata_raft_ids.size());
            EXPECT_EQ(result.config.initial_raft_membership.voter_raft_ids,
                      metadata_raft_ids);
        }

        TEST(cluster_config_generation_test,
             supports_1_3_5_7_voter_layouts_with_valid_generated_membership)
        {
            for (const std::size_t voter_count : std::array<std::size_t, 4>{1, 3, 5, 7})
            {
                SCOPED_TRACE("voter_count=" + std::to_string(voter_count));
                const ClusterConfigGenerationRequest request =
                    MakeGenerationRequest(voter_count);

                const ClusterConfigGenerationResult generated =
                    GenerateDeterministicClusterConfig(request);

                ExpectValidGeneratedTopology(request, generated);
            }
        }

        TEST(cluster_config_generation_test,
             same_request_generates_reproducible_config_without_hardcoded_demo_topology)
        {
            ClusterConfigGenerationRequest request = MakeGenerationRequest(5);
            request.fixed_view_node_ids = {"view-fixed-a"};
            request.fixed_metadata_node_ids = {
                "meta-fixed-a",
                "meta-fixed-b",
                "meta-fixed-c",
                "meta-fixed-d",
                "meta-fixed-e",
            };
            request.fixed_metadata_raft_ids = {41, 43};
            request.fixed_storage_node_ids = {
                "store-fixed-a",
                "store-fixed-b",
                "store-fixed-c",
            };
            request.generation_seed = 4242;

            const ClusterConfigGenerationResult first =
                GenerateDeterministicClusterConfig(request);
            const ClusterConfigGenerationResult second =
                GenerateDeterministicClusterConfig(request);

            ExpectValidGeneratedTopology(request, first);
            ExpectValidGeneratedTopology(request, second);
            EXPECT_EQ(BuildConfigSignature(first.config),
                      BuildConfigSignature(second.config));
            EXPECT_EQ(first.config.initial_raft_membership.membership_epoch, 4242U);
        }

        TEST(cluster_config_validation_test,
             rejects_zero_storage_capacity_in_generated_config)
        {
            ClusterConfigGenerationRequest request = MakeGenerationRequest(3);
            request.default_storage_capacity_bytes = 0;
            request.storage_capacity_overrides_bytes.clear();

            const ClusterConfigGenerationResult generated =
                GenerateDeterministicClusterConfig(request);

            EXPECT_FALSE(generated.ok());
            EXPECT_EQ(generated.status, ClusterConfigStatusCode::kInvalidArgument);
            EXPECT_FALSE(generated.validation.ok());
            EXPECT_TRUE(ContainsIssue(generated.validation,
                                      ClusterConfigIssueCode::kInvalidCapacity));
        }
    } // namespace
} // namespace clusterdemo
