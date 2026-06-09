#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
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

        const ClusterConfigValidationIssue *FindIssue(
            const ClusterConfigValidationResult &validation,
            const ClusterConfigIssueCode code)
        {
            for (const ClusterConfigValidationIssue &issue : validation.issues)
            {
                if (issue.code == code)
                {
                    return &issue;
                }
            }
            return nullptr;
        }

        std::string BuildEndpointAllocationSignature(
            const ClusterEndpointAllocationResult &allocation)
        {
            std::ostringstream oss;
            for (const ClusterEndpointAssignment &assignment : allocation.assignments)
            {
                oss << ToString(assignment.node_type) << '|'
                    << assignment.node_id << '|'
                    << assignment.ordinal << '|'
                    << assignment.endpoint << '\n';
            }
            return oss.str();
        }

        std::size_t CountAssignments(const ClusterEndpointAllocationResult &allocation,
                                     const ClusterNodeType node_type)
        {
            std::size_t count = 0;
            for (const ClusterEndpointAssignment &assignment : allocation.assignments)
            {
                if (assignment.node_type == node_type)
                {
                    ++count;
                }
            }
            return count;
        }

        std::filesystem::path MakeTempConfigPath(const std::size_t voter_count)
        {
            const auto now_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            return std::filesystem::temp_directory_path() /
                   "cqupt_raft_cluster_config_test" /
                   ("t070-" + std::to_string(voter_count) + "-" +
                    std::to_string(now_ns) + ".json");
        }

        void WriteConfigJson(const std::filesystem::path &path,
                             const std::string &content)
        {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            ASSERT_FALSE(ec) << ec.message();

            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(output.is_open()) << path.string();
            output << content;
            output.close();
            ASSERT_TRUE(output.good()) << path.string();
        }

        std::vector<std::string> MakeFixedMetadataNodeIds(const std::size_t count)
        {
            static constexpr std::array<const char *, 7> kMetadataNodeIds{
                "meta-zulu",
                "meta-alpha",
                "meta-gamma",
                "meta-beta",
                "meta-theta",
                "meta-delta",
                "meta-omega",
            };

            return std::vector<std::string>(kMetadataNodeIds.begin(),
                                            kMetadataNodeIds.begin() + count);
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

        TEST(cluster_config_generation_test,
             T070MetadataRaftIdsStayStableDistinctAndMatchInitialMembership)
        {
            for (const std::size_t voter_count : std::array<std::size_t, 4>{1, 3, 5, 7})
            {
                SCOPED_TRACE("voter_count=" + std::to_string(voter_count));
                ClusterConfigGenerationRequest request =
                    MakeGenerationRequest(voter_count);
                request.fixed_metadata_node_ids =
                    MakeFixedMetadataNodeIds(voter_count);

                const ClusterConfigGenerationResult first =
                    GenerateDeterministicClusterConfig(request);
                const ClusterConfigGenerationResult second =
                    GenerateDeterministicClusterConfig(request);

                ASSERT_TRUE(first.ok()) << first.error_detail;
                ASSERT_TRUE(second.ok()) << second.error_detail;
                ASSERT_EQ(first.config.metadata_nodes.size(), voter_count);
                ASSERT_EQ(second.config.metadata_nodes.size(), voter_count);

                std::vector<std::int32_t> first_raft_ids;
                std::vector<std::int32_t> second_raft_ids;
                first_raft_ids.reserve(voter_count);
                second_raft_ids.reserve(voter_count);

                std::set<std::int32_t> unique_raft_ids;
                for (std::size_t index = 0; index < voter_count; ++index)
                {
                    const auto &first_node = first.config.metadata_nodes[index];
                    const auto &second_node = second.config.metadata_nodes[index];
                    EXPECT_EQ(first_node.node_id,
                              request.fixed_metadata_node_ids[index]);
                    EXPECT_FALSE(first_node.node_id.empty());
                    EXPECT_GT(first_node.raft_id, 0);
                    EXPECT_EQ(first_node.raft_id,
                              static_cast<std::int32_t>(index + 1));
                    EXPECT_EQ(first_node.raft_id, second_node.raft_id);
                    EXPECT_EQ(first_node.node_id, second_node.node_id);
                    EXPECT_NE(first_node.node_id, std::to_string(first_node.raft_id));
                    EXPECT_EQ(first_node.initial_role,
                              MetadataNodeInitialRole::kVoter);

                    first_raft_ids.push_back(first_node.raft_id);
                    second_raft_ids.push_back(second_node.raft_id);
                    unique_raft_ids.insert(first_node.raft_id);

                    const auto resolved = ResolveClusterNodeConfig(
                        first.config,
                        ClusterNodeType::kMetadata,
                        first_node.node_id);
                    ASSERT_TRUE(resolved.ok()) << resolved.error_detail;
                    ASSERT_TRUE(resolved.resolved.has_value());
                    EXPECT_EQ(resolved.resolved->node_id, first_node.node_id);
                    ASSERT_TRUE(resolved.resolved->raft_id.has_value());
                    EXPECT_EQ(*resolved.resolved->raft_id, first_node.raft_id);
                }

                EXPECT_EQ(unique_raft_ids.size(), voter_count);
                EXPECT_EQ(first.config.initial_raft_membership.voter_raft_ids,
                          first_raft_ids);
                EXPECT_EQ(second.config.initial_raft_membership.voter_raft_ids,
                          second_raft_ids);

                const std::filesystem::path json_path =
                    MakeTempConfigPath(voter_count);
                WriteConfigJson(json_path,
                                SerializeClusterConfigToJson(first.config));
                const auto loaded = LoadClusterConfigFromJsonFile(json_path);
                ASSERT_TRUE(loaded.ok()) << loaded.error_detail;
                ASSERT_TRUE(loaded.config.has_value());
                EXPECT_EQ(loaded.config->initial_raft_membership.voter_raft_ids,
                          first_raft_ids);
                ASSERT_EQ(loaded.config->metadata_nodes.size(), voter_count);
                for (std::size_t index = 0; index < voter_count; ++index)
                {
                    EXPECT_EQ(loaded.config->metadata_nodes[index].node_id,
                              first.config.metadata_nodes[index].node_id);
                    EXPECT_EQ(loaded.config->metadata_nodes[index].raft_id,
                              first.config.metadata_nodes[index].raft_id);
                }

                std::error_code remove_ec;
                std::filesystem::remove(json_path, remove_ec);
            }
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

        TEST(cluster_config_validation_test,
             T070RejectsDuplicateFixedMetadataRaftIdsWithClearDiagnostics)
        {
            ClusterConfigGenerationRequest request = MakeGenerationRequest(3);
            request.fixed_metadata_node_ids = MakeFixedMetadataNodeIds(3);
            request.fixed_metadata_raft_ids = {17, 17, 19};

            const ClusterConfigGenerationResult generated =
                GenerateDeterministicClusterConfig(request);

            EXPECT_FALSE(generated.ok());
            EXPECT_EQ(generated.status, ClusterConfigStatusCode::kConflict);
            EXPECT_FALSE(generated.validation.ok());

            const ClusterConfigValidationIssue *issue =
                FindIssue(generated.validation,
                          ClusterConfigIssueCode::kDuplicateRaftId);
            ASSERT_NE(issue, nullptr);
            EXPECT_EQ(issue->field_path, "metadata_nodes[1].raft_id");
            EXPECT_EQ(issue->node_type, ClusterNodeType::kMetadata);
            EXPECT_EQ(issue->node_id, request.fixed_metadata_node_ids[1]);
        }

        TEST(cluster_config_endpoint_allocation_test,
             allocates_stable_role_specific_endpoints_from_request)
        {
            ClusterConfigGenerationRequest request = MakeGenerationRequest(3);
            request.fixed_view_node_ids = {"view-fixed-a"};
            request.fixed_metadata_node_ids = {
                "meta-fixed-a",
                "meta-fixed-b",
                "meta-fixed-c",
            };
            request.fixed_storage_node_ids = {
                "store-fixed-a",
                "store-fixed-b",
                "store-fixed-c",
            };

            const ClusterEndpointAllocationResult first =
                AllocateClusterEndpoints(request);
            const ClusterEndpointAllocationResult second =
                AllocateClusterEndpoints(request);

            ASSERT_TRUE(first.ok()) << first.error_detail;
            ASSERT_TRUE(second.ok()) << second.error_detail;
            EXPECT_EQ(CountAssignments(first, ClusterNodeType::kView), 1U);
            EXPECT_EQ(CountAssignments(first, ClusterNodeType::kMetadata), 3U);
            EXPECT_EQ(CountAssignments(first, ClusterNodeType::kStorage), 3U);
            EXPECT_EQ(BuildEndpointAllocationSignature(first),
                      BuildEndpointAllocationSignature(second));
        }

        TEST(cluster_config_endpoint_allocation_test,
             reports_duplicate_endpoint_conflicts_from_overlapping_port_ranges)
        {
            ClusterConfigGenerationRequest request = MakeGenerationRequest(1);
            request.view_port_base = 25000;
            request.metadata_port_base = 25000;
            request.storage_port_base = 25000;

            const ClusterEndpointAllocationResult allocation =
                AllocateClusterEndpoints(request);

            EXPECT_FALSE(allocation.ok());
            EXPECT_EQ(allocation.status, ClusterConfigStatusCode::kConflict);
            EXPECT_TRUE(ContainsIssue(allocation.validation,
                                      ClusterConfigIssueCode::kDuplicateEndpoint));
        }

        TEST(cluster_config_resolution_test,
             resolves_view_metadata_and_storage_nodes_by_role_and_node_id)
        {
            ClusterConfigGenerationRequest request = MakeGenerationRequest(3);
            request.fixed_view_node_ids = {"view-fixed-a"};
            request.fixed_metadata_node_ids = {
                "meta-fixed-a",
                "meta-fixed-b",
                "meta-fixed-c",
            };
            request.fixed_storage_node_ids = {
                "store-fixed-a",
                "store-fixed-b",
                "store-fixed-c",
            };

            const ClusterConfigGenerationResult generated =
                GenerateDeterministicClusterConfig(request);
            ASSERT_TRUE(generated.ok()) << generated.error_detail;

            const ClusterNodeResolutionResult view_result =
                ResolveClusterNodeConfig(generated.config,
                                         ClusterNodeType::kView,
                                         "view-fixed-a");
            ASSERT_TRUE(view_result.ok()) << view_result.error_detail;
            ASSERT_TRUE(view_result.resolved.has_value());
            EXPECT_EQ(view_result.resolved->node_type, ClusterNodeType::kView);
            EXPECT_EQ(view_result.resolved->endpoint, "127.0.0.1:21300");
            EXPECT_EQ(view_result.resolved->data_dir,
                      request.base_dir / "view" / "view-fixed-a");
            EXPECT_FALSE(view_result.resolved->snapshot_dir.has_value());
            EXPECT_FALSE(view_result.resolved->raft_id.has_value());

            const ClusterNodeResolutionResult metadata_result =
                ResolveClusterNodeConfig(generated.config,
                                         ClusterNodeType::kMetadata,
                                         "meta-fixed-b");
            ASSERT_TRUE(metadata_result.ok()) << metadata_result.error_detail;
            ASSERT_TRUE(metadata_result.resolved.has_value());
            EXPECT_EQ(metadata_result.resolved->node_type,
                      ClusterNodeType::kMetadata);
            EXPECT_EQ(metadata_result.resolved->endpoint, "127.0.0.1:22301");
            EXPECT_EQ(metadata_result.resolved->raft_id, 2);
            EXPECT_EQ(metadata_result.resolved->metadata_initial_role,
                      MetadataNodeInitialRole::kVoter);
            EXPECT_TRUE(metadata_result.resolved->snapshot_dir.has_value());

            const ClusterNodeResolutionResult storage_result =
                ResolveClusterNodeConfig(generated.config,
                                         ClusterNodeType::kStorage,
                                         "store-fixed-c");
            ASSERT_TRUE(storage_result.ok()) << storage_result.error_detail;
            ASSERT_TRUE(storage_result.resolved.has_value());
            EXPECT_EQ(storage_result.resolved->node_type, ClusterNodeType::kStorage);
            EXPECT_EQ(storage_result.resolved->endpoint, "127.0.0.1:23302");
            EXPECT_EQ(storage_result.resolved->capacity_bytes,
                      kDefaultStorageCapacityBytes);
            EXPECT_FALSE(storage_result.resolved->snapshot_dir.has_value());
            EXPECT_FALSE(storage_result.resolved->raft_id.has_value());
        }

        TEST(cluster_config_resolution_test,
             rejects_missing_node_and_role_mismatch_without_fallback)
        {
            ClusterConfigGenerationRequest request = MakeGenerationRequest(3);
            request.fixed_metadata_node_ids = {
                "meta-fixed-a",
                "meta-fixed-b",
                "meta-fixed-c",
            };
            request.fixed_storage_node_ids = {
                "store-fixed-a",
                "store-fixed-b",
                "store-fixed-c",
            };

            const ClusterConfigGenerationResult generated =
                GenerateDeterministicClusterConfig(request);
            ASSERT_TRUE(generated.ok()) << generated.error_detail;

            const ClusterNodeResolutionResult role_mismatch =
                ResolveClusterNodeConfig(generated.config,
                                         ClusterNodeType::kMetadata,
                                         "store-fixed-a");
            EXPECT_FALSE(role_mismatch.ok());
            EXPECT_EQ(role_mismatch.status, ClusterConfigStatusCode::kInvalidArgument);
            EXPECT_FALSE(role_mismatch.resolved.has_value());
            EXPECT_TRUE(ContainsIssue(role_mismatch.validation,
                                      ClusterConfigIssueCode::kInvalidNodeType));

            const ClusterNodeResolutionResult missing_node =
                ResolveClusterNodeConfig(generated.config,
                                         ClusterNodeType::kStorage,
                                         "store-missing");
            EXPECT_FALSE(missing_node.ok());
            EXPECT_EQ(missing_node.status, ClusterConfigStatusCode::kInvalidArgument);
            EXPECT_FALSE(missing_node.resolved.has_value());
            EXPECT_TRUE(ContainsIssue(missing_node.validation,
                                      ClusterConfigIssueCode::kInvalidNodeId));
        }

        TEST(cluster_config_quorum_helper_test,
             computes_majority_quorum_for_1_3_5_7_initial_voters)
        {
            for (const auto &[voter_count, expected_quorum] :
                 std::array<std::pair<std::size_t, std::size_t>, 4>{
                     std::pair<std::size_t, std::size_t>{1, 1},
                     {3, 2},
                     {5, 3},
                     {7, 4},
                 })
            {
                SCOPED_TRACE("voter_count=" + std::to_string(voter_count));
                const ClusterConfigGenerationResult generated =
                    GenerateDeterministicClusterConfig(
                        MakeGenerationRequest(voter_count));
                ASSERT_TRUE(generated.ok()) << generated.error_detail;

                const InitialRaftQuorumComputationResult from_config =
                    ComputeInitialRaftQuorum(generated.config);
                ASSERT_TRUE(from_config.ok()) << from_config.error_detail;
                ASSERT_TRUE(from_config.summary.has_value());
                EXPECT_EQ(from_config.summary->voter_count, voter_count);
                EXPECT_EQ(from_config.summary->election_quorum, expected_quorum);
                EXPECT_EQ(from_config.summary->commit_quorum, expected_quorum);
                EXPECT_EQ(from_config.summary->voter_raft_ids,
                          generated.config.initial_raft_membership.voter_raft_ids);

                EXPECT_EQ(
                    ComputeInitialRaftQuorumSize(generated.config.initial_raft_membership),
                    expected_quorum);
            }
        }

        TEST(cluster_config_quorum_helper_test,
             rejects_empty_or_learner_only_membership_with_diagnostics)
        {
            const InitialRaftMembershipConfig empty_membership{
                .voter_raft_ids = {},
                .learner_raft_ids = {},
                .membership_epoch = 1,
            };
            const InitialRaftQuorumComputationResult empty_result =
                ComputeInitialRaftQuorum(empty_membership);
            EXPECT_FALSE(empty_result.ok());
            EXPECT_EQ(empty_result.status, ClusterConfigStatusCode::kInvalidArgument);
            EXPECT_FALSE(empty_result.summary.has_value());
            EXPECT_TRUE(ContainsIssue(empty_result.validation,
                                      ClusterConfigIssueCode::kInvalidRaftVoterCount));

            const InitialRaftMembershipConfig learner_only_membership{
                .voter_raft_ids = {},
                .learner_raft_ids = {11, 13},
                .membership_epoch = 2,
            };
            const InitialRaftQuorumComputationResult learner_only_result =
                ComputeInitialRaftQuorum(learner_only_membership);
            EXPECT_FALSE(learner_only_result.ok());
            EXPECT_EQ(learner_only_result.status, ClusterConfigStatusCode::kInvalidArgument);
            EXPECT_FALSE(learner_only_result.summary.has_value());
            EXPECT_TRUE(ContainsIssue(learner_only_result.validation,
                                      ClusterConfigIssueCode::kInvalidRaftVoterCount));
        }

        TEST(cluster_config_quorum_helper_test,
             rejects_duplicate_and_overlapping_membership_entries)
        {
            const InitialRaftMembershipConfig duplicate_voters{
                .voter_raft_ids = {21, 21, 23},
                .learner_raft_ids = {},
                .membership_epoch = 3,
            };
            const InitialRaftQuorumComputationResult duplicate_result =
                ComputeInitialRaftQuorum(duplicate_voters);
            EXPECT_FALSE(duplicate_result.ok());
            EXPECT_EQ(duplicate_result.status, ClusterConfigStatusCode::kInvalidArgument);
            EXPECT_TRUE(ContainsIssue(duplicate_result.validation,
                                      ClusterConfigIssueCode::kInvalidInitialMembership));

            const InitialRaftMembershipConfig overlapping_membership{
                .voter_raft_ids = {31, 33, 35},
                .learner_raft_ids = {35, 37},
                .membership_epoch = 4,
            };
            const InitialRaftQuorumComputationResult overlapping_result =
                ComputeInitialRaftQuorum(overlapping_membership);
            EXPECT_FALSE(overlapping_result.ok());
            EXPECT_EQ(overlapping_result.status, ClusterConfigStatusCode::kInvalidArgument);
            EXPECT_TRUE(ContainsIssue(overlapping_result.validation,
                                      ClusterConfigIssueCode::kInvalidInitialMembership));
        }
    } // namespace
} // namespace clusterdemo
