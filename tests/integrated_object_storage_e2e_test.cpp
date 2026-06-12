#include "raft/common/metadata_command.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "support/metadata_test_utils.h"
#include "cluster/cluster_config.h"
#include "store/placement/placement_manager.h"
#include "view/view_registry.h"

#include "metadata.pb.h"
#include "store/common/store_types.h"
#include "storage_node.pb.h"

#include <google/protobuf/descriptor.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using raftdemo::test::ApplyMetadataCommand;
    using raftdemo::test::MakeCreateBucketCommand;
    using raftdemo::test::MakeSnapshotPath;

    struct HappyPathE2EScaffoldWorkspace
    {
        std::filesystem::path root;
        std::filesystem::path source_path;
        std::filesystem::path download_path;

        ~HappyPathE2EScaffoldWorkspace()
        {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    };

    bool DescriptorHasBytesField(const google::protobuf::Descriptor &descriptor)
    {
        for (int index = 0; index < descriptor.field_count(); ++index)
        {
            if (descriptor.field(index)->type() ==
                google::protobuf::FieldDescriptor::TYPE_BYTES)
            {
                return true;
            }
        }

        return false;
    }

    std::vector<char> ReadBinaryFile(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
        {
            throw std::runtime_error("failed to open snapshot file: " + path.string());
        }

        return std::vector<char>(std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>());
    }

    std::string MakeHappyPathFixturePayload()
    {
        std::string payload;
        payload.reserve(64 * 1024);
        for (std::size_t index = 0; index < 64 * 1024; ++index)
        {
            payload.push_back(static_cast<char>((index * 31 + 17) % 251));
        }
        return payload;
    }

    void WriteBinaryFileOrThrow(const std::filesystem::path &path,
                                const std::string &content)
    {
        std::error_code create_ec;
        std::filesystem::create_directories(path.parent_path(), create_ec);
        if (create_ec)
        {
            throw std::runtime_error("failed to create directories for test file: " +
                                     path.string() + ": " + create_ec.message());
        }

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            throw std::runtime_error("failed to open output file: " + path.string());
        }

        output.write(content.data(),
                     static_cast<std::streamsize>(content.size()));
        output.close();
        if (!output.good())
        {
            throw std::runtime_error("failed to write output file: " + path.string());
        }
    }

    std::string ReadBinaryFileToStringOrThrow(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
        {
            throw std::runtime_error("failed to open binary test file: " +
                                     path.string());
        }

        return std::string(std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>());
    }

    std::string ComputeFileSha256OrThrow(const std::filesystem::path &path)
    {
        storedemo::ChunkChecksum checksum;
        std::string error_detail;
        const std::string payload = ReadBinaryFileToStringOrThrow(path);
        const auto status =
            storedemo::ComputeChunkChecksum(payload, &checksum, &error_detail);
        if (status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to compute scaffold SHA-256 for " +
                                     path.string() + ": " + error_detail);
        }

        return checksum.value;
    }

    std::string ComputePayloadSha256OrThrow(const std::string &payload)
    {
        storedemo::ChunkChecksum checksum;
        std::string error_detail;
        const auto status =
            storedemo::ComputeChunkChecksum(payload, &checksum, &error_detail);
        if (status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to compute scaffold SHA-256: " +
                                     error_detail);
        }

        return checksum.value;
    }

    std::string MakeCorruptedPayloadCopy(const std::string &payload,
                                         const std::size_t offset)
    {
        if (payload.empty())
        {
            throw std::runtime_error("cannot corrupt empty payload");
        }

        std::string corrupted = payload;
        const std::size_t index = offset % corrupted.size();
        corrupted[index] = static_cast<char>(corrupted[index] ^ 0x5A);
        if (corrupted[index] == payload[index])
        {
            corrupted[index] = static_cast<char>(corrupted[index] ^ 0x01);
        }
        return corrupted;
    }

    HappyPathE2EScaffoldWorkspace MakeHappyPathE2EScaffoldWorkspace()
    {
        const auto now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        HappyPathE2EScaffoldWorkspace workspace;
        workspace.root = std::filesystem::temp_directory_path() /
                         "cqupt_integrated_object_storage_e2e" /
                         ("t026-" + std::to_string(now_ns));
        workspace.source_path = workspace.root / "input" / "fixture.bin";
        workspace.download_path = workspace.root / "output" / "fixture.download.bin";
        return workspace;
    }

    std::vector<raftdemo::ChunkRef> MakeAuditChunks()
    {
        return {
            raftdemo::ChunkRef{
                "chunk-t022-0", 0, 4096, {"store-a", "store-b"}, "sha256:chunk-t022-0"},
            raftdemo::ChunkRef{
                "chunk-t022-1", 4096, 2048, {"store-b"}, "sha256:chunk-t022-1"}};
    }

    raftdemo::MetadataCommand MakeCreateObjectAuditCommand()
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCreateObject;
        command.request_id = "create-object-t022";
        command.create_object = raftdemo::CreateObjectCommandPayload{
            raftdemo::ObjectRecord{"bucket-t022",
                                   "objects/boundary-audit.bin",
                                   "obj-t022",
                                   3,
                                   6144,
                                   "sha256:object-t022",
                                   raftdemo::ObjectState::PENDING,
                                   {},
                                   1717555200001ULL,
                                   std::nullopt,
                                   std::nullopt}};
        command.request_context = raftdemo::RequestRecord{
            "create-object-t022",
            raftdemo::MetadataRequestType::kCreateObject,
            "bucket-t022",
            "objects/boundary-audit.bin",
            "accepted",
            0,
            1717555200001ULL,
            std::nullopt};
        return command;
    }

    raftdemo::MetadataCommand MakeCommitObjectAuditCommand()
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCommitObject;
        command.request_id = "commit-object-t022";
        command.commit_object = raftdemo::CommitObjectCommandPayload{
            "bucket-t022",
            "objects/boundary-audit.bin",
            "obj-t022",
            3,
            6144,
            "sha256:object-t022",
            MakeAuditChunks(),
            1717555200999ULL};
        command.request_context = raftdemo::RequestRecord{
            "commit-object-t022",
            raftdemo::MetadataRequestType::kCommitObject,
            "bucket-t022",
            "objects/boundary-audit.bin",
            "accepted",
            0,
            1717555200001ULL,
            1717555200999ULL};
        return command;
    }

    std::vector<raftdemo::ChunkRef> MakeChecksumMismatchChunks(
        const std::string &payload)
    {
        const std::size_t first_chunk_size = payload.size() / 2;
        const std::size_t second_chunk_size = payload.size() - first_chunk_size;
        return {
            raftdemo::ChunkRef{"chunk-t028-0",
                               0,
                               static_cast<std::uint64_t>(first_chunk_size),
                               {"store-a", "store-b"},
                               ComputePayloadSha256OrThrow(
                                   payload.substr(0, first_chunk_size))},
            raftdemo::ChunkRef{"chunk-t028-1",
                               static_cast<std::uint64_t>(first_chunk_size),
                               static_cast<std::uint64_t>(second_chunk_size),
                               {"store-b"},
                               ComputePayloadSha256OrThrow(
                                   payload.substr(first_chunk_size))}};
    }

    raftdemo::MetadataCommand MakeChecksumMismatchCommitCommand(
        const std::vector<raftdemo::ChunkRef> &chunks,
        const std::uint64_t object_size,
        const std::string &object_checksum)
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCommitObject;
        command.request_id = "commit-object-t028";
        command.commit_object = raftdemo::CommitObjectCommandPayload{
            "bucket-t028",
            "objects/checksum-mismatch.bin",
            "obj-t028",
            1,
            object_size,
            object_checksum,
            chunks,
            1717555300999ULL};
        command.request_context = raftdemo::RequestRecord{
            "commit-object-t028",
            raftdemo::MetadataRequestType::kCommitObject,
            "bucket-t028",
            "objects/checksum-mismatch.bin",
            "accepted",
            0,
            1717555300001ULL,
            1717555300999ULL};
        return command;
    }

    std::vector<raftdemo::ChunkRef> MakeDynamicStoragePlacementLegacyChunks()
    {
        return {
            raftdemo::ChunkRef{"chunk-t048-0",
                               0,
                               256,
                               {"store-a", "store-b"},
                               "sha256:t048-chunk-0"},
            raftdemo::ChunkRef{"chunk-t048-1",
                               256,
                               256,
                               {"store-b"},
                               "sha256:t048-chunk-1"}};
    }

    raftdemo::MetadataCommand MakeDynamicStoragePlacementCommitCommand()
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCommitObject;
        command.request_id = "commit-object-t048-old";
        command.commit_object = raftdemo::CommitObjectCommandPayload{
            "bucket-t048",
            "objects/legacy-before-join.bin",
            "obj-t048-old",
            1,
            512,
            "sha256:obj-t048-old",
            MakeDynamicStoragePlacementLegacyChunks(),
            1717555400999ULL};
        command.request_context = raftdemo::RequestRecord{
            "commit-object-t048-old",
            raftdemo::MetadataRequestType::kCommitObject,
            "bucket-t048",
            "objects/legacy-before-join.bin",
            "accepted",
            0,
            1717555400001ULL,
            1717555400999ULL};
        return command;
    }

    viewdemo::NodeRegistration MakeViewStorageRegistration(
        std::string cluster_id,
        std::string node_id,
        const std::uint16_t port,
        const std::uint64_t observed_at_unix_ms,
        const std::uint64_t total_capacity_bytes,
        const std::uint64_t used_capacity_bytes,
        const std::uint64_t available_capacity_bytes,
        std::string zone)
    {
        viewdemo::NodeRegistration registration;
        registration.cluster_id = std::move(cluster_id);
        registration.node_id = std::move(node_id);
        registration.node_type = viewdemo::ViewNodeType::kStorage;
        registration.endpoint = "127.0.0.1:" + std::to_string(port);
        registration.control_plane_endpoint =
            "127.0.0.1:" + std::to_string(static_cast<std::uint32_t>(port) + 1000);
        registration.data_plane_endpoint =
            "127.0.0.1:" + std::to_string(static_cast<std::uint32_t>(port) + 2000);
        registration.data_dir_fingerprint =
            "fingerprint-" + registration.node_id;
        registration.observed_at_unix_ms = observed_at_unix_ms;
        registration.failure_domain.zone = std::move(zone);
        registration.failure_domain.rack = "rack-a";
        registration.health.health = viewdemo::ViewNodeHealth::kHealthy;
        registration.health.disk_pressure = viewdemo::ViewNodeDiskPressure::kLow;
        registration.capacity.total_capacity_bytes = total_capacity_bytes;
        registration.capacity.used_capacity_bytes = used_capacity_bytes;
        registration.capacity.available_capacity_bytes =
            available_capacity_bytes;
        registration.capacity.chunk_count = 8;
        return registration;
    }

    viewdemo::RegisterNodeRequest MakeViewRegisterRequest(
        viewdemo::NodeRegistration registration,
        std::string request_id)
    {
        viewdemo::RegisterNodeRequest request;
        request.request_id = std::move(request_id);
        request.registration = std::move(registration);
        return request;
    }

    viewdemo::HeartbeatNodeRequest MakeViewStorageHeartbeatRequest(
        std::string cluster_id,
        std::string node_id,
        const std::uint16_t port,
        std::string incarnation_id,
        const std::uint64_t sequence,
        const std::uint64_t observed_at_unix_ms,
        const std::uint64_t total_capacity_bytes,
        const std::uint64_t used_capacity_bytes,
        const std::uint64_t available_capacity_bytes,
        std::string zone)
    {
        viewdemo::HeartbeatNodeRequest request;
        request.request_id =
            "heartbeat-" + node_id + "-" + std::to_string(sequence);
        request.cluster_id = std::move(cluster_id);
        request.node_id = std::move(node_id);
        request.node_type = viewdemo::ViewNodeType::kStorage;
        request.incarnation_id = std::move(incarnation_id);
        request.sequence = sequence;
        request.observation = MakeViewStorageRegistration(request.cluster_id,
                                                          request.node_id,
                                                          port,
                                                          observed_at_unix_ms,
                                                          total_capacity_bytes,
                                                          used_capacity_bytes,
                                                          available_capacity_bytes,
                                                          std::move(zone));
        return request;
    }

    storedemo::PlacementRequest MakePlacementRequest(
        std::string object_id,
        const std::uint64_t version,
        const std::uint32_t chunk_index,
        const std::uint64_t chunk_size_bytes,
        const std::size_t replica_count,
        const std::size_t minimum_successful_writes,
        const std::uint64_t decision_epoch)
    {
        storedemo::PlacementRequest request;
        request.identity.object_id = std::move(object_id);
        request.identity.version = version;
        request.identity.chunk_index = chunk_index;
        request.chunk_size_bytes = chunk_size_bytes;
        request.policy.replica_count = replica_count;
        request.policy.minimum_successful_writes = minimum_successful_writes;
        request.decision_epoch = decision_epoch;
        return request;
    }

    bool DecisionContainsReplicaNode(
        const storedemo::PlacementDecisionResult &result,
        std::string_view node_id)
    {
        for (const auto &candidate : result.decision.replica_nodes)
        {
            if (candidate.node_id == node_id)
            {
                return true;
            }
        }
        return false;
    }

    void ExpectChunkRefsEqual(const std::vector<raftdemo::ChunkRef> &actual,
                              const std::vector<raftdemo::ChunkRef> &expected)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index)
        {
            EXPECT_EQ(actual[index].chunk_id, expected[index].chunk_id);
            EXPECT_EQ(actual[index].offset, expected[index].offset);
            EXPECT_EQ(actual[index].size, expected[index].size);
            EXPECT_EQ(actual[index].replica_nodes, expected[index].replica_nodes);
            EXPECT_EQ(actual[index].checksum, expected[index].checksum);
        }
    }

    struct AppConfigNodeSmokeView
    {
        clusterdemo::ClusterNodeType node_type{clusterdemo::ClusterNodeType::kUnknown};
        std::string cluster_id;
        std::string node_id;
        std::string endpoint;
        std::filesystem::path data_dir;
        std::optional<std::filesystem::path> snapshot_dir;
        std::optional<std::int32_t> raft_id;
        std::optional<clusterdemo::MetadataNodeInitialRole> metadata_initial_role;
    };

    struct StorageClientConfigSmokeView
    {
        std::string cluster_id;
        std::vector<std::string> view_endpoints;
        clusterdemo::ChunkPolicyConfig chunk_policy;
        clusterdemo::ClusterTimeoutConfig timeouts;
    };

    struct AppConfigSmokeResult
    {
        bool ok{false};
        std::string diagnostic;
        std::optional<AppConfigNodeSmokeView> node;
        std::optional<StorageClientConfigSmokeView> client;
    };

    clusterdemo::ClusterTimeoutConfig MakeValidAppSmokeTimeoutConfig()
    {
        return clusterdemo::ClusterTimeoutConfig{
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

    clusterdemo::ChunkPolicyConfig MakeValidAppSmokeChunkPolicy()
    {
        return clusterdemo::ChunkPolicyConfig{
            .chunk_size_bytes = 4ULL * 1024ULL * 1024ULL,
            .replica_count = 3,
            .minimum_successful_writes = 2,
            .checksum_algorithm = clusterdemo::ClusterChecksumAlgorithm::kSha256,
        };
    }

    clusterdemo::ClusterConfigGenerationRequest MakeAppConfigSmokeGenerationRequest()
    {
        const auto now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        return clusterdemo::ClusterConfigGenerationRequest{
            .cluster_id = "app-config-smoke-cluster",
            .base_dir = std::filesystem::temp_directory_path() /
                        "cqupt_raft_integrated_app_config_smoke" /
                        ("case-" + std::to_string(now_ns)),
            .bind_host = "127.0.0.1",
            .advertise_host = "",
            .view_node_count = 1,
            .metadata_node_count = 3,
            .metadata_voter_count = 3,
            .storage_node_count = 2,
            .view_port_base = 31000,
            .metadata_port_base = 32000,
            .storage_port_base = 33000,
            .default_storage_capacity_bytes = 64ULL * 1024ULL * 1024ULL,
            .chunk_policy = MakeValidAppSmokeChunkPolicy(),
            .timeouts = MakeValidAppSmokeTimeoutConfig(),
            .fixed_view_node_ids = {"view-main"},
            .fixed_metadata_node_ids = {"meta-a", "meta-b", "meta-c"},
            .fixed_metadata_raft_ids = {11, 13, 17},
            .fixed_storage_node_ids = {"store-a", "store-b"},
            .storage_capacity_overrides_bytes = {96ULL * 1024ULL * 1024ULL},
            .generation_seed = 2026041,
        };
    }

    std::string DescribeClusterValidationIssues(
        const clusterdemo::ClusterConfigValidationResult &validation)
    {
        std::ostringstream oss;
        bool first = true;
        for (const auto &issue : validation.issues)
        {
            if (!first)
            {
                oss << " | ";
            }
            first = false;
            oss << clusterdemo::DescribeClusterConfigIssue(issue);
        }
        return oss.str();
    }

    AppConfigSmokeResult ResolveNodeAppConfigSmoke(
        const clusterdemo::ClusterConfig &config,
        const clusterdemo::ClusterNodeType expected_role,
        const std::string_view node_id)
    {
        const auto validation = clusterdemo::ValidateClusterConfig(config);
        if (!validation.ok())
        {
            return AppConfigSmokeResult{
                .ok = false,
                .diagnostic = "invalid cluster config: " +
                              DescribeClusterValidationIssues(validation),
            };
        }

        if (node_id.empty())
        {
            return AppConfigSmokeResult{
                .ok = false,
                .diagnostic = "node_id must not be empty for role=" +
                              std::string(clusterdemo::ToString(expected_role)),
            };
        }

        auto make_result = [&](AppConfigNodeSmokeView view)
        {
            AppConfigSmokeResult result;
            result.ok = true;
            result.node = std::move(view);
            return result;
        };

        if (expected_role == clusterdemo::ClusterNodeType::kView)
        {
            for (const auto &node : config.view_nodes)
            {
                if (node.node_id.has_value() && *node.node_id == node_id)
                {
                    return make_result(AppConfigNodeSmokeView{
                        .node_type = expected_role,
                        .cluster_id = config.cluster_id,
                        .node_id = *node.node_id,
                        .endpoint = node.endpoint,
                        .data_dir = node.data_dir,
                    });
                }
            }
        }
        else if (expected_role == clusterdemo::ClusterNodeType::kMetadata)
        {
            for (const auto &node : config.metadata_nodes)
            {
                if (node.node_id == node_id)
                {
                    return make_result(AppConfigNodeSmokeView{
                        .node_type = expected_role,
                        .cluster_id = config.cluster_id,
                        .node_id = node.node_id,
                        .endpoint = node.endpoint,
                        .data_dir = node.data_dir,
                        .snapshot_dir = node.snapshot_dir,
                        .raft_id = node.raft_id,
                        .metadata_initial_role = node.initial_role,
                    });
                }
            }
        }
        else if (expected_role == clusterdemo::ClusterNodeType::kStorage)
        {
            for (const auto &node : config.storage_nodes)
            {
                if (node.node_id.has_value() && *node.node_id == node_id)
                {
                    return make_result(AppConfigNodeSmokeView{
                        .node_type = expected_role,
                        .cluster_id = config.cluster_id,
                        .node_id = *node.node_id,
                        .endpoint = node.endpoint,
                        .data_dir = node.data_dir,
                    });
                }
            }
        }

        const auto node_id_text = std::string(node_id);
        for (const auto &node : config.view_nodes)
        {
            if (node.node_id.has_value() && *node.node_id == node_id_text)
            {
                return AppConfigSmokeResult{
                    .ok = false,
                    .diagnostic = "node_id=" + node_id_text +
                                  " belongs to role=view, requested_role=" +
                                  std::string(clusterdemo::ToString(expected_role)),
                };
            }
        }
        for (const auto &node : config.metadata_nodes)
        {
            if (node.node_id == node_id_text)
            {
                return AppConfigSmokeResult{
                    .ok = false,
                    .diagnostic = "node_id=" + node_id_text +
                                  " belongs to role=metadata, requested_role=" +
                                  std::string(clusterdemo::ToString(expected_role)),
                };
            }
        }
        for (const auto &node : config.storage_nodes)
        {
            if (node.node_id.has_value() && *node.node_id == node_id_text)
            {
                return AppConfigSmokeResult{
                    .ok = false,
                    .diagnostic = "node_id=" + node_id_text +
                                  " belongs to role=storage, requested_role=" +
                                  std::string(clusterdemo::ToString(expected_role)),
                };
            }
        }

        return AppConfigSmokeResult{
            .ok = false,
            .diagnostic = "node_id=" + node_id_text +
                          " not found for role=" +
                          std::string(clusterdemo::ToString(expected_role)),
        };
    }

    AppConfigSmokeResult ResolveStorageClientConfigSmoke(
        const clusterdemo::ClusterConfig &config)
    {
        // storage_client 的配置解析首先关心 discovery 入口是否存在；
        // 这里优先给出缺少 ViewNode 的明确 smoke 诊断，再落到通用配置校验。
        if (config.view_nodes.empty())
        {
            return AppConfigSmokeResult{
                .ok = false,
                .diagnostic = "storage_client requires at least one view node endpoint",
            };
        }

        const auto validation = clusterdemo::ValidateClusterConfig(config);
        if (!validation.ok())
        {
            return AppConfigSmokeResult{
                .ok = false,
                .diagnostic = "invalid cluster config: " +
                              DescribeClusterValidationIssues(validation),
            };
        }

        StorageClientConfigSmokeView client_view;
        client_view.cluster_id = config.cluster_id;
        client_view.chunk_policy = config.chunk_policy;
        client_view.timeouts = config.timeouts;
        client_view.view_endpoints.reserve(config.view_nodes.size());
        for (const auto &node : config.view_nodes)
        {
            client_view.view_endpoints.push_back(node.endpoint);
        }

        AppConfigSmokeResult result;
        result.ok = true;
        result.client = std::move(client_view);
        return result;
    }
} // namespace

TEST(IntegratedObjectStorageE2ETest,
     AppConfigParsingSmokeResolvesViewMetadataStorageAndClientBootstrapFromUnifiedClusterConfig)
{
    const auto request = MakeAppConfigSmokeGenerationRequest();
    const auto generated =
        clusterdemo::GenerateDeterministicClusterConfig(request);

    ASSERT_TRUE(generated.ok()) << generated.error_detail;

    const auto view =
        ResolveNodeAppConfigSmoke(generated.config,
                                  clusterdemo::ClusterNodeType::kView,
                                  "view-main");
    ASSERT_TRUE(view.ok) << view.diagnostic;
    ASSERT_TRUE(view.node.has_value());
    EXPECT_EQ(view.node->cluster_id, request.cluster_id);
    EXPECT_EQ(view.node->node_id, "view-main");
    EXPECT_EQ(view.node->endpoint, "127.0.0.1:31000");
    EXPECT_EQ(view.node->data_dir,
              request.base_dir / "view" / "view-main");

    const auto metadata =
        ResolveNodeAppConfigSmoke(generated.config,
                                  clusterdemo::ClusterNodeType::kMetadata,
                                  "meta-b");
    ASSERT_TRUE(metadata.ok) << metadata.diagnostic;
    ASSERT_TRUE(metadata.node.has_value());
    EXPECT_EQ(metadata.node->cluster_id, request.cluster_id);
    EXPECT_EQ(metadata.node->node_id, "meta-b");
    EXPECT_EQ(metadata.node->endpoint, "127.0.0.1:32001");
    ASSERT_TRUE(metadata.node->snapshot_dir.has_value());
    EXPECT_EQ(*metadata.node->snapshot_dir,
              request.base_dir / "metadata" / "meta-b" / "snapshots");
    ASSERT_TRUE(metadata.node->raft_id.has_value());
    EXPECT_EQ(*metadata.node->raft_id, 13);
    ASSERT_TRUE(metadata.node->metadata_initial_role.has_value());
    EXPECT_EQ(*metadata.node->metadata_initial_role,
              clusterdemo::MetadataNodeInitialRole::kVoter);

    const auto storage =
        ResolveNodeAppConfigSmoke(generated.config,
                                  clusterdemo::ClusterNodeType::kStorage,
                                  "store-a");
    ASSERT_TRUE(storage.ok) << storage.diagnostic;
    ASSERT_TRUE(storage.node.has_value());
    EXPECT_EQ(storage.node->cluster_id, request.cluster_id);
    EXPECT_EQ(storage.node->node_id, "store-a");
    EXPECT_EQ(storage.node->endpoint, "127.0.0.1:33000");
    EXPECT_EQ(storage.node->data_dir,
              request.base_dir / "storage" / "store-a");

    const auto client = ResolveStorageClientConfigSmoke(generated.config);
    ASSERT_TRUE(client.ok) << client.diagnostic;
    ASSERT_TRUE(client.client.has_value());
    EXPECT_EQ(client.client->cluster_id, request.cluster_id);
    ASSERT_EQ(client.client->view_endpoints.size(), 1U);
    EXPECT_EQ(client.client->view_endpoints.front(), "127.0.0.1:31000");
    EXPECT_EQ(client.client->chunk_policy.replica_count,
              request.chunk_policy.replica_count);
    EXPECT_EQ(client.client->timeouts.discovery_rpc_timeout,
              request.timeouts.discovery_rpc_timeout);
}

TEST(IntegratedObjectStorageE2ETest,
     AppConfigParsingSmokeRejectsUnknownNodeIdAndRoleMismatchWithClearDiagnostics)
{
    const auto request = MakeAppConfigSmokeGenerationRequest();
    const auto generated =
        clusterdemo::GenerateDeterministicClusterConfig(request);
    ASSERT_TRUE(generated.ok()) << generated.error_detail;

    const auto wrong_role =
        ResolveNodeAppConfigSmoke(generated.config,
                                  clusterdemo::ClusterNodeType::kMetadata,
                                  "store-a");
    EXPECT_FALSE(wrong_role.ok);
    EXPECT_NE(wrong_role.diagnostic.find("node_id=store-a"), std::string::npos);
    EXPECT_NE(wrong_role.diagnostic.find("requested_role=metadata"),
              std::string::npos);

    const auto missing =
        ResolveNodeAppConfigSmoke(generated.config,
                                  clusterdemo::ClusterNodeType::kStorage,
                                  "store-missing");
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(missing.diagnostic.find("node_id=store-missing"),
              std::string::npos);
    EXPECT_NE(missing.diagnostic.find("role=storage"), std::string::npos);
}

TEST(IntegratedObjectStorageE2ETest,
     AppConfigParsingSmokeRejectsEndpointAndDataDirConflictsBeforeBootstrap)
{
    const auto request = MakeAppConfigSmokeGenerationRequest();
    const auto generated =
        clusterdemo::GenerateDeterministicClusterConfig(request);
    ASSERT_TRUE(generated.ok()) << generated.error_detail;

    auto conflicting = generated.config;
    conflicting.storage_nodes.front().endpoint =
        conflicting.metadata_nodes.front().endpoint;
    conflicting.storage_nodes.front().data_dir =
        conflicting.metadata_nodes.front().data_dir;

    const auto resolved =
        ResolveNodeAppConfigSmoke(conflicting,
                                  clusterdemo::ClusterNodeType::kStorage,
                                  "store-a");
    EXPECT_FALSE(resolved.ok);
    EXPECT_NE(resolved.diagnostic.find("duplicate_endpoint"), std::string::npos);
    EXPECT_NE(resolved.diagnostic.find("shared_data_dir"), std::string::npos);
}

TEST(IntegratedObjectStorageE2ETest,
     AppConfigParsingSmokeRejectsMissingViewDiscoveryForStorageClient)
{
    const auto request = MakeAppConfigSmokeGenerationRequest();
    const auto generated =
        clusterdemo::GenerateDeterministicClusterConfig(request);
    ASSERT_TRUE(generated.ok()) << generated.error_detail;

    auto no_view = generated.config;
    no_view.view_nodes.clear();

    const auto client = ResolveStorageClientConfigSmoke(no_view);
    EXPECT_FALSE(client.ok);
    EXPECT_NE(client.diagnostic.find("at least one view node endpoint"),
              std::string::npos);
}

TEST(IntegratedObjectStorageE2ETest,
     DISABLED_AppConfigParsingSmokeCliOverridesMustRespectDurableIdentityAndStartupContracts)
{
    GTEST_SKIP()
        << "T041 先锁定 unified cluster config parsing smoke 边界。"
        << "启用 CLI override 与 durable identity 冲突路径，需要后续任务完成："
        << "T042 per-node config resolution、T045/T046/T047 thin app startup、"
        << "以及 app 层对 --node_id/--data_dir/--listen override 的显式拒绝语义。";
}

TEST(IntegratedObjectStorageE2ETest,
     PayloadBoundaryAuditMetadataControlPlaneDescriptorsExcludeRawPayloadBytes)
{
    ASSERT_NE(raft::CreateObjectRequest::descriptor(), nullptr);
    ASSERT_NE(raft::CommitObjectRequest::descriptor(), nullptr);
    ASSERT_NE(raft::ChunkRef::descriptor(), nullptr);
    ASSERT_NE(raft::ObjectRecord::descriptor(), nullptr);
    ASSERT_NE(raft::HeadObjectResponse::descriptor(), nullptr);
    ASSERT_NE(raft::ListObjectsResponse::descriptor(), nullptr);

    EXPECT_EQ(raft::CreateObjectRequest::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_EQ(raft::CommitObjectRequest::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_EQ(raft::ChunkRef::descriptor()->FindFieldByName("payload"), nullptr);
    EXPECT_EQ(raft::ObjectRecord::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_EQ(raft::HeadObjectResponse::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_EQ(raft::ListObjectsResponse::descriptor()->FindFieldByName("payload"),
              nullptr);

    EXPECT_FALSE(DescriptorHasBytesField(*raft::CreateObjectRequest::descriptor()));
    EXPECT_FALSE(DescriptorHasBytesField(*raft::CommitObjectRequest::descriptor()));
    EXPECT_FALSE(DescriptorHasBytesField(*raft::ChunkRef::descriptor()));
    EXPECT_FALSE(DescriptorHasBytesField(*raft::ObjectRecord::descriptor()));
    EXPECT_FALSE(DescriptorHasBytesField(*raft::HeadObjectResponse::descriptor()));
    EXPECT_FALSE(DescriptorHasBytesField(*raft::ListObjectsResponse::descriptor()));

    ASSERT_NE(storage::WriteChunkRequest::descriptor(), nullptr);
    ASSERT_NE(storage::ReadChunkResponse::descriptor(), nullptr);
    EXPECT_NE(storage::WriteChunkRequest::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_NE(storage::ReadChunkResponse::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_TRUE(DescriptorHasBytesField(*storage::WriteChunkRequest::descriptor()));
    EXPECT_TRUE(DescriptorHasBytesField(*storage::ReadChunkResponse::descriptor()));
}

TEST(IntegratedObjectStorageE2ETest,
     PayloadBoundaryAuditMetadataCommandsSerializeOnlyManifestFacts)
{
    const raftdemo::MetadataCommand create_command = MakeCreateObjectAuditCommand();
    const raftdemo::MetadataCommand commit_command = MakeCommitObjectAuditCommand();

    std::string error;
    ASSERT_TRUE(raftdemo::ValidateMetadataCommand(create_command, &error)) << error;
    error.clear();
    ASSERT_TRUE(raftdemo::ValidateMetadataCommand(commit_command, &error)) << error;

    const std::string create_encoded =
        raftdemo::SerializeMetadataCommand(create_command);
    const std::string commit_encoded =
        raftdemo::SerializeMetadataCommand(commit_command);

    EXPECT_EQ(create_encoded.find("record_payload="), std::string::npos);
    EXPECT_EQ(commit_encoded.find("record_payload="), std::string::npos);
    EXPECT_EQ(create_encoded.find("payload"), std::string::npos);
    EXPECT_EQ(commit_encoded.find("payload"), std::string::npos);

    EXPECT_NE(create_encoded.find("target_bucket=bucket-t022"), std::string::npos);
    EXPECT_NE(create_encoded.find("target_object_id=obj-t022"), std::string::npos);
    EXPECT_NE(create_encoded.find("target_size=6144"), std::string::npos);
    EXPECT_NE(commit_encoded.find("target_chunk_count=2"), std::string::npos);
    EXPECT_NE(commit_encoded.find("target_chunk_0_id=chunk-t022-0"),
              std::string::npos);
    EXPECT_NE(commit_encoded.find("target_chunk_1_offset=4096"),
              std::string::npos);
    EXPECT_NE(commit_encoded.find("target_chunk_1_checksum=sha256:chunk-t022-1"),
              std::string::npos);

    raftdemo::MetadataCommand parsed_commit;
    ASSERT_TRUE(raftdemo::ParseMetadataCommand(commit_encoded, &parsed_commit));
    ASSERT_TRUE(parsed_commit.commit_object.has_value());
    EXPECT_EQ(parsed_commit.commit_object->bucket, "bucket-t022");
    EXPECT_EQ(parsed_commit.commit_object->object_key,
              "objects/boundary-audit.bin");
    EXPECT_EQ(parsed_commit.commit_object->object_id, "obj-t022");
    EXPECT_EQ(parsed_commit.commit_object->size, 6144U);
    ExpectChunkRefsEqual(parsed_commit.commit_object->chunks, MakeAuditChunks());
}

TEST(IntegratedObjectStorageE2ETest,
     PayloadBoundaryAuditMetadataSnapshotRoundTripKeepsManifestFactsOnly)
{
    raftdemo::MetadataStateMachine machine;
    const std::vector<raftdemo::ChunkRef> expected_chunks = MakeAuditChunks();

    std::uint64_t index = 1;
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     MakeCreateBucketCommand("bucket-t022",
                                                             "create-bucket-t022"))
                    .Ok);
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     MakeCreateObjectAuditCommand())
                    .Ok);
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     MakeCommitObjectAuditCommand())
                    .Ok);

    const std::filesystem::path snapshot_path =
        MakeSnapshotPath("t022-payload-boundary.snapshot");
    std::error_code ec;
    std::filesystem::remove(snapshot_path, ec);

    const auto save = machine.SaveSnapshot(snapshot_path.string());
    ASSERT_EQ(save.status, raftdemo::SnapshotStatus::kOk) << save.message;

    const std::vector<char> snapshot_bytes = ReadBinaryFile(snapshot_path);
    const std::string snapshot_text(snapshot_bytes.begin(), snapshot_bytes.end());
    EXPECT_EQ(snapshot_text.find("record_payload="), std::string::npos);
    EXPECT_EQ(snapshot_text.find("payload"), std::string::npos);
    EXPECT_NE(snapshot_text.find("chunk-t022-0"), std::string::npos);
    EXPECT_NE(snapshot_text.find("chunk-t022-1"), std::string::npos);
    EXPECT_NE(snapshot_text.find("sha256:chunk-t022-1"), std::string::npos);

    raftdemo::MetadataStateMachine restored;
    const auto load = restored.LoadSnapshot(snapshot_path.string());
    ASSERT_EQ(load.status, raftdemo::SnapshotStatus::kOk) << load.message;

    const auto head = restored.HeadObject(
        {.bucket = "bucket-t022", .object_key = "objects/boundary-audit.bin"});
    ASSERT_EQ(head.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(head.record.has_value());
    EXPECT_TRUE(head.record->IsCommitted());
    EXPECT_EQ(head.record->object_id, "obj-t022");
    EXPECT_EQ(head.record->version, 3U);
    EXPECT_EQ(head.record->size, 6144U);
    EXPECT_EQ(head.record->etag, "sha256:object-t022");
    ExpectChunkRefsEqual(head.record->chunks, expected_chunks);

    const auto chunk_refs = restored.FindChunkRefs("bucket-t022",
                                                   "objects/boundary-audit.bin");
    ASSERT_TRUE(chunk_refs.has_value());
    ExpectChunkRefsEqual(*chunk_refs, expected_chunks);
}

TEST(IntegratedObjectStorageE2ETest,
     HappyPathUploadDownloadScaffoldPreparesRealFileAndChecksumExpectation)
{
    const auto workspace = MakeHappyPathE2EScaffoldWorkspace();
    const std::string payload = MakeHappyPathFixturePayload();

    WriteBinaryFileOrThrow(workspace.source_path, payload);

    ASSERT_TRUE(std::filesystem::exists(workspace.source_path));
    EXPECT_FALSE(std::filesystem::exists(workspace.download_path));

    const std::string source_checksum =
        ComputeFileSha256OrThrow(workspace.source_path);
    EXPECT_EQ(source_checksum.size(), storedemo::kSha256DigestHexChars);
    EXPECT_FALSE(source_checksum.empty());

    const std::string payload_round_trip =
        ReadBinaryFileToStringOrThrow(workspace.source_path);
    EXPECT_EQ(payload_round_trip, payload);

    const auto object_key =
        std::string("objects/") + workspace.source_path.filename().string();
    EXPECT_EQ(object_key, "objects/fixture.bin");
    EXPECT_EQ(std::filesystem::file_size(workspace.source_path),
              static_cast<std::uintmax_t>(payload.size()));

    // T026 只建立 happy-path E2E scaffold：真实输入文件、目标下载路径和
    // SHA-256 比对入口已经就位；真实 upload/download/manifest 流程由后续任务接入。
}

TEST(IntegratedObjectStorageE2ETest,
     DISABLED_HappyPathUploadDownloadRoundTripViaIntegratedObjectStorage)
{
    const auto workspace = MakeHappyPathE2EScaffoldWorkspace();
    WriteBinaryFileOrThrow(workspace.source_path, MakeHappyPathFixturePayload());
    const std::string expected_sha256 =
        ComputeFileSha256OrThrow(workspace.source_path);

    ASSERT_TRUE(std::filesystem::exists(workspace.source_path));
    ASSERT_FALSE(expected_sha256.empty());
    ASSERT_FALSE(std::filesystem::exists(workspace.download_path));

    GTEST_SKIP()
        << "T026 仅提供 happy-path E2E scaffold。启用该 round-trip 用例需要后续任务完成："
        << "T029/T030 object_transfer、T031/T032 metadata transfer adapter、"
        << "T033/T034 storage transfer adapter、T035 ViewNode discovery 接入、"
        << "T036 manifest-driven download reconstruction、T037 storage_client upload/download。";
}

TEST(IntegratedObjectStorageE2ETest,
     ManifestVisibilityPendingHiddenCommittedVisible)
{
    raftdemo::MetadataStateMachine machine;
    std::uint64_t index = 1;

    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     MakeCreateBucketCommand("bucket-t027",
                                                             "create-bucket-t027"))
                    .Ok);
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     raftdemo::test::MakeCreateObjectCommand(
                                         "bucket-t027",
                                         "objects/visibility.bin",
                                         "obj-t027",
                                         "create-object-t027"))
                    .Ok);

    // T027 锁定 manifest 可见性边界：普通可见路径只能来自 MetadataNode
    // 已提交的 COMMITTED manifest，不能从 ViewNode 观测或 StorageNode 本地状态推断。
    const auto pending_head = machine.HeadObject(
        {.bucket = "bucket-t027", .object_key = "objects/visibility.bin"});
    EXPECT_EQ(pending_head.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_FALSE(pending_head.record.has_value());

    const auto pending_list =
        machine.ListObjects({.bucket = "bucket-t027", .prefix = "objects/"});
    ASSERT_EQ(pending_list.result.code, raftdemo::MetadataStatusCode::kOk);
    EXPECT_TRUE(pending_list.records.empty());

    const auto pending_chunks =
        machine.FindChunkRefs("bucket-t027", "objects/visibility.bin");
    EXPECT_FALSE(pending_chunks.has_value());

    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     raftdemo::test::MakeCommitObjectCommand(
                                         "bucket-t027",
                                         "objects/visibility.bin",
                                         "obj-t027",
                                         "commit-object-t027"))
                    .Ok);

    const auto committed_head = machine.HeadObject(
        {.bucket = "bucket-t027", .object_key = "objects/visibility.bin"});
    ASSERT_EQ(committed_head.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(committed_head.record.has_value());
    EXPECT_TRUE(committed_head.record->IsCommitted());
    EXPECT_EQ(committed_head.record->object_id, "obj-t027");

    const auto committed_list =
        machine.ListObjects({.bucket = "bucket-t027", .prefix = "objects/"});
    ASSERT_EQ(committed_list.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_EQ(committed_list.records.size(), 1U);
    EXPECT_EQ(committed_list.records.front().object_key, "objects/visibility.bin");
    EXPECT_TRUE(committed_list.records.front().IsCommitted());

    const auto committed_chunks =
        machine.FindChunkRefs("bucket-t027", "objects/visibility.bin");
    ASSERT_TRUE(committed_chunks.has_value());
    ExpectChunkRefsEqual(*committed_chunks,
                         raftdemo::test::MakeCommitObjectCommand("bucket-t027",
                                                                 "objects/visibility.bin",
                                                                 "obj-t027",
                                                                 "commit-object-t027")
                             .commit_object->chunks);
}

TEST(IntegratedObjectStorageE2ETest,
     DynamicStorageNodePlacementSeesNewNodeWithoutRewritingCommittedManifest)
{
    const std::string cluster_id = "cluster-t048";
    raftdemo::MetadataStateMachine machine;
    std::uint64_t index = 1;

    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     MakeCreateBucketCommand("bucket-t048",
                                                             "create-bucket-t048"))
                    .Ok);
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     raftdemo::test::MakeCreateObjectCommand(
                                         "bucket-t048",
                                         "objects/legacy-before-join.bin",
                                         "obj-t048-old",
                                         "create-object-t048-old"))
                    .Ok);
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     MakeDynamicStoragePlacementCommitCommand())
                    .Ok);

    const auto original_head = machine.HeadObject(
        {.bucket = "bucket-t048", .object_key = "objects/legacy-before-join.bin"});
    ASSERT_EQ(original_head.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(original_head.record.has_value());
    ASSERT_TRUE(original_head.record->IsCommitted());
    const std::vector<raftdemo::ChunkRef> original_manifest =
        original_head.record->chunks;
    ExpectChunkRefsEqual(original_manifest,
                         MakeDynamicStoragePlacementLegacyChunks());

    viewdemo::ViewNodeRegistry registry;
    const auto register_store_a = registry.RegisterNode(
        MakeViewRegisterRequest(
            MakeViewStorageRegistration(cluster_id,
                                        "store-a",
                                        7501,
                                        1717555401000ULL,
                                        256ULL * 1024ULL * 1024ULL,
                                        64ULL * 1024ULL * 1024ULL,
                                        192ULL * 1024ULL * 1024ULL,
                                        "zone-a"),
            "register-store-a"));
    ASSERT_EQ(register_store_a.summary.status,
              viewdemo::ViewRegistryStatusCode::kOk);
    const auto register_store_b = registry.RegisterNode(
        MakeViewRegisterRequest(
            MakeViewStorageRegistration(cluster_id,
                                        "store-b",
                                        7502,
                                        1717555401000ULL,
                                        224ULL * 1024ULL * 1024ULL,
                                        64ULL * 1024ULL * 1024ULL,
                                        160ULL * 1024ULL * 1024ULL,
                                        "zone-b"),
            "register-store-b"));
    ASSERT_EQ(register_store_b.summary.status,
              viewdemo::ViewRegistryStatusCode::kOk);

    storedemo::PlacementManager placement_manager;
    viewdemo::DiscoverStorageRequest discover_request;
    discover_request.request_id = "discover-storage-before-join";
    discover_request.cluster_id = cluster_id;
    discover_request.live_only = false;
    discover_request.require_writable = false;

    const auto placement_before_join = placement_manager.SelectPlacement(
        MakePlacementRequest("obj-t048-future-before-join",
                             1,
                             0,
                             256,
                             2,
                             2,
                             101),
        registry,
        discover_request,
        1717555401000ULL);
    ASSERT_TRUE(placement_before_join.ok()) << placement_before_join.error_detail;
    ASSERT_EQ(placement_before_join.decision.replica_nodes.size(), 2U);
    EXPECT_TRUE(DecisionContainsReplicaNode(placement_before_join, "store-a"));
    EXPECT_TRUE(DecisionContainsReplicaNode(placement_before_join, "store-b"));
    EXPECT_FALSE(DecisionContainsReplicaNode(placement_before_join, "store-c"));

    const auto register_store_c = registry.RegisterNode(
        MakeViewRegisterRequest(
            MakeViewStorageRegistration(cluster_id,
                                        "store-c",
                                        7503,
                                        1717555402000ULL,
                                        192ULL * 1024ULL * 1024ULL,
                                        64ULL * 1024ULL * 1024ULL,
                                        128ULL * 1024ULL * 1024ULL,
                                        "zone-c"),
            "register-store-c"));
    ASSERT_EQ(register_store_c.summary.status,
              viewdemo::ViewRegistryStatusCode::kOk);
    const auto heartbeat_store_c = registry.HeartbeatNode(
        MakeViewStorageHeartbeatRequest(cluster_id,
                                        "store-c",
                                        7503,
                                        "store-c:boot:1717555402000000000:201:1",
                                        1,
                                        1717555402000ULL,
                                        192ULL * 1024ULL * 1024ULL,
                                        64ULL * 1024ULL * 1024ULL,
                                        128ULL * 1024ULL * 1024ULL,
                                        "zone-c"));
    ASSERT_EQ(heartbeat_store_c.summary.status,
              viewdemo::ViewRegistryStatusCode::kOk);
    ASSERT_TRUE(heartbeat_store_c.applied);

    discover_request.request_id = "discover-storage-after-join";
    const auto placement_after_join = placement_manager.SelectPlacement(
        MakePlacementRequest("obj-t048-future-after-join",
                             1,
                             0,
                             256,
                             3,
                             2,
                             102),
        registry,
        discover_request,
        1717555402000ULL);
    ASSERT_TRUE(placement_after_join.ok()) << placement_after_join.error_detail;
    ASSERT_EQ(placement_after_join.decision.replica_nodes.size(), 3U);
    EXPECT_TRUE(DecisionContainsReplicaNode(placement_after_join, "store-a"));
    EXPECT_TRUE(DecisionContainsReplicaNode(placement_after_join, "store-b"));
    EXPECT_TRUE(DecisionContainsReplicaNode(placement_after_join, "store-c"));

    const auto committed_head_after_join = machine.HeadObject(
        {.bucket = "bucket-t048", .object_key = "objects/legacy-before-join.bin"});
    ASSERT_EQ(committed_head_after_join.result.code,
              raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(committed_head_after_join.record.has_value());
    ASSERT_TRUE(committed_head_after_join.record->IsCommitted());
    ExpectChunkRefsEqual(committed_head_after_join.record->chunks,
                         original_manifest);

    const auto committed_chunks_after_join =
        machine.FindChunkRefs("bucket-t048", "objects/legacy-before-join.bin");
    ASSERT_TRUE(committed_chunks_after_join.has_value());
    ExpectChunkRefsEqual(*committed_chunks_after_join, original_manifest);

    const auto generated =
        clusterdemo::GenerateDeterministicClusterConfig(
            MakeAppConfigSmokeGenerationRequest());
    ASSERT_TRUE(generated.ok()) << generated.error_detail;

    const auto quorum_before_join =
        clusterdemo::ComputeInitialRaftQuorum(generated.config);
    ASSERT_TRUE(quorum_before_join.ok()) << quorum_before_join.error_detail;
    ASSERT_TRUE(quorum_before_join.summary.has_value());
    EXPECT_EQ(quorum_before_join.summary->voter_raft_ids,
              generated.config.initial_raft_membership.voter_raft_ids);
    EXPECT_EQ(quorum_before_join.summary->voter_count, 3U);
    EXPECT_EQ(quorum_before_join.summary->commit_quorum, 2U);

    const auto quorum_after_join =
        clusterdemo::ComputeInitialRaftQuorum(generated.config);
    ASSERT_TRUE(quorum_after_join.ok()) << quorum_after_join.error_detail;
    ASSERT_TRUE(quorum_after_join.summary.has_value());
    EXPECT_EQ(quorum_after_join.summary->voter_raft_ids,
              quorum_before_join.summary->voter_raft_ids);
    EXPECT_EQ(quorum_after_join.summary->commit_quorum, 2U);
    EXPECT_EQ(clusterdemo::ComputeInitialRaftQuorumSize(
                  generated.config.initial_raft_membership),
              2U);
}

TEST(IntegratedObjectStorageE2ETest,
     ChecksumMismatchDownloadFailureScaffoldPreparesCommittedManifestAndCorruptChunkFixture)
{
    const auto workspace = MakeHappyPathE2EScaffoldWorkspace();
    const std::string source_payload = MakeHappyPathFixturePayload();
    const std::string corrupted_payload =
        MakeCorruptedPayloadCopy(source_payload, source_payload.size() * 3 / 4);
    const std::vector<raftdemo::ChunkRef> expected_chunks =
        MakeChecksumMismatchChunks(source_payload);
    const std::string expected_object_checksum =
        ComputePayloadSha256OrThrow(source_payload);

    raftdemo::MetadataStateMachine machine;
    std::uint64_t index = 1;
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     MakeCreateBucketCommand("bucket-t028",
                                                             "create-bucket-t028"))
                    .Ok);
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     raftdemo::test::MakeCreateObjectCommand(
                                         "bucket-t028",
                                         "objects/checksum-mismatch.bin",
                                         "obj-t028",
                                         "create-object-t028"))
                    .Ok);
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     MakeChecksumMismatchCommitCommand(
                                         expected_chunks,
                                         static_cast<std::uint64_t>(
                                             source_payload.size()),
                                         expected_object_checksum))
                    .Ok);

    const auto committed_head = machine.HeadObject(
        {.bucket = "bucket-t028", .object_key = "objects/checksum-mismatch.bin"});
    ASSERT_EQ(committed_head.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(committed_head.record.has_value());
    EXPECT_TRUE(committed_head.record->IsCommitted());
    EXPECT_EQ(committed_head.record->etag, expected_object_checksum);

    const auto committed_chunks =
        machine.FindChunkRefs("bucket-t028", "objects/checksum-mismatch.bin");
    ASSERT_TRUE(committed_chunks.has_value());
    ExpectChunkRefsEqual(*committed_chunks, expected_chunks);

    WriteBinaryFileOrThrow(workspace.source_path, source_payload);
    const auto corrupted_chunk_path = workspace.root / "chunks" / "chunk-1.bin";
    WriteBinaryFileOrThrow(corrupted_chunk_path,
                           corrupted_payload.substr(source_payload.size() / 2));

    const std::string healthy_first_chunk_checksum = ComputePayloadSha256OrThrow(
        source_payload.substr(0, source_payload.size() / 2));
    const std::string corrupted_second_chunk_checksum =
        ComputeFileSha256OrThrow(corrupted_chunk_path);
    const std::string corrupted_object_checksum =
        ComputePayloadSha256OrThrow(corrupted_payload);

    // T028 先锁定 checksum mismatch 失败前置条件：manifest 提供 committed
    // checksum，损坏 chunk 的实际校验值与 manifest 不一致，后续真实下载实现必须失败。
    EXPECT_EQ(healthy_first_chunk_checksum, expected_chunks[0].checksum);
    EXPECT_NE(corrupted_second_chunk_checksum, expected_chunks[1].checksum);
    EXPECT_NE(corrupted_object_checksum, expected_object_checksum);
    EXPECT_FALSE(std::filesystem::exists(workspace.download_path));
}

TEST(IntegratedObjectStorageE2ETest,
     DISABLED_ChecksumMismatchDownloadFailsWithoutPublishingCorruptedFile)
{
    const auto workspace = MakeHappyPathE2EScaffoldWorkspace();
    const std::string source_payload = MakeHappyPathFixturePayload();
    const std::string corrupted_payload =
        MakeCorruptedPayloadCopy(source_payload, source_payload.size() * 3 / 4);

    WriteBinaryFileOrThrow(workspace.source_path, source_payload);
    ASSERT_TRUE(std::filesystem::exists(workspace.source_path));
    ASSERT_FALSE(std::filesystem::exists(workspace.download_path));
    ASSERT_NE(ComputePayloadSha256OrThrow(corrupted_payload),
              ComputePayloadSha256OrThrow(source_payload));

    GTEST_SKIP()
        << "T028 仅提供 checksum mismatch 下载失败测试骨架。启用该用例需要后续任务完成："
        << "T030 object_transfer 上传/下载会话、T032 metadata transfer adapter、"
        << "T034 storage transfer adapter、T035 ViewNode discovery 接入、"
        << "T036 manifest-driven download reconstruction 与 checksum fail-fast、"
        << "T037 storage_client upload/download。";
}
