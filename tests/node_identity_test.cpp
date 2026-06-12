#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

#include "cluster/node_identity.h"

namespace clusterdemo
{
    namespace
    {
        struct ParsedProcessIncarnationId
        {
            std::string durable_node_id;
            std::string boot_token;
        };

        [[nodiscard]] std::optional<ParsedProcessIncarnationId>
        ParseProcessIncarnationId(const std::string &incarnation_id)
        {
            const std::string marker = ":boot:";
            const auto marker_pos = incarnation_id.find(marker);
            if (marker_pos == std::string::npos || marker_pos == 0)
            {
                return std::nullopt;
            }

            const auto boot_token = incarnation_id.substr(
                marker_pos + marker.size());
            if (boot_token.empty())
            {
                return std::nullopt;
            }

            return ParsedProcessIncarnationId{
                .durable_node_id = incarnation_id.substr(0, marker_pos),
                .boot_token = boot_token};
        }

        [[nodiscard]] bool IsCurrentIncarnation(
            const ProcessIncarnation &candidate,
            const ProcessIncarnation &current)
        {
            return candidate.cluster_id == current.cluster_id &&
                   candidate.node_id == current.node_id &&
                   candidate.node_type == current.node_type &&
                   candidate.incarnation_id == current.incarnation_id;
        }

        class NodeIdentityTest : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                const auto now_ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
                root_ = std::filesystem::temp_directory_path() /
                        "cqupt_raft_node_identity_test" /
                        ("case-" + std::to_string(now_ns));

                std::error_code ec;
                std::filesystem::remove_all(root_, ec);
                std::filesystem::create_directories(root_, ec);
                ASSERT_FALSE(ec) << ec.message();
            }

            void TearDown() override
            {
                std::error_code ec;
                std::filesystem::remove_all(root_, ec);
            }

            [[nodiscard]] std::filesystem::path MakeDataDir(
                const std::string &name) const
            {
                const auto path = root_ / name;
                std::error_code ec;
                std::filesystem::create_directories(path, ec);
                EXPECT_FALSE(ec) << ec.message();
                return path;
            }

            [[nodiscard]] NodeIdentity MakeMetadataIdentity(
                const std::string &node_id = "meta-node-1",
                const std::int32_t raft_id = 101) const
            {
                return NodeIdentity{
                    .cluster_id = "cluster-alpha",
                    .node_id = node_id,
                    .node_type = ClusterNodeType::kMetadata,
                    .raft_id = raft_id,
                    .membership_state = NodeIdentityMembershipState::kVoter,
                    .persistent_generation = 1,
                    .identity_version = kNodeIdentityCurrentVersion,
                    .created_at_unix_ms = 1710000000000LL,
                    .source = NodeIdentitySource::kConfigGenerator};
            }

            [[nodiscard]] NodeIdentity MakeMetadataDynamicJoinCandidateIdentity(
                const std::string &node_id = "meta-join-1",
                const std::optional<std::int32_t> raft_id = 301) const
            {
                return NodeIdentity{
                    .cluster_id = "cluster-alpha",
                    .node_id = node_id,
                    .node_type = ClusterNodeType::kMetadata,
                    .raft_id = raft_id,
                    .membership_state = NodeIdentityMembershipState::kCandidate,
                    .persistent_generation = 1,
                    .identity_version = kNodeIdentityCurrentVersion,
                    .created_at_unix_ms = 1710000000000LL,
                    .source = NodeIdentitySource::kExplicitOverride};
            }

            [[nodiscard]] NodeIdentity MakeStorageIdentity(
                const std::string &node_id = "store-node-1") const
            {
                return NodeIdentity{
                    .cluster_id = "cluster-alpha",
                    .node_id = node_id,
                    .node_type = ClusterNodeType::kStorage,
                    .raft_id = std::nullopt,
                    .membership_state = NodeIdentityMembershipState::kNonRaft,
                    .persistent_generation = 1,
                    .identity_version = kNodeIdentityCurrentVersion,
                    .created_at_unix_ms = 1710000000000LL,
                    .source = NodeIdentitySource::kViewNodeAllocator};
            }

            [[nodiscard]] NodeIdentity MakeViewIdentity(
                const std::string &node_id = "view-node-1") const
            {
                return NodeIdentity{
                    .cluster_id = "cluster-alpha",
                    .node_id = node_id,
                    .node_type = ClusterNodeType::kView,
                    .raft_id = std::nullopt,
                    .membership_state = NodeIdentityMembershipState::kNonRaft,
                    .persistent_generation = 1,
                    .identity_version = kNodeIdentityCurrentVersion,
                    .created_at_unix_ms = 1710000000000LL,
                    .source = NodeIdentitySource::kConfigGenerator};
            }

            void WriteTextFile(const std::filesystem::path &path,
                               const std::string &content) const
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

            [[nodiscard]] std::string ReadTextFile(
                const std::filesystem::path &path) const
            {
                std::ifstream input(path, std::ios::binary);
                EXPECT_TRUE(input.is_open()) << path.string();
                std::ostringstream buffer;
                buffer << input.rdbuf();
                EXPECT_TRUE(input.good() || input.eof()) << path.string();
                return buffer.str();
            }

            [[nodiscard]] bool ValidationContains(
                const NodeIdentityValidationResult &validation,
                const NodeIdentityIssueCode code) const
            {
                for (const auto &issue : validation.issues)
                {
                    if (issue.code == code)
                    {
                        return true;
                    }
                }
                return false;
            }

            [[nodiscard]] ProcessIncarnation CreateIncarnationOrAssert(
                const NodeIdentity &identity) const
            {
                const auto result = CreateProcessIncarnation(identity);
                EXPECT_TRUE(result.ok()) << result.diagnostic;
                EXPECT_EQ(result.status, NodeIdentityStatusCode::kOk);
                EXPECT_TRUE(result.incarnation.has_value());
                EXPECT_FALSE(result.incarnation->incarnation_id.empty());
                EXPECT_GT(result.incarnation->started_at_unix_ms, 0);
                EXPECT_EQ(result.incarnation->startup_sequence_base,
                          kProcessIncarnationInitialSequence);
                EXPECT_EQ(result.incarnation->node_id, identity.node_id);
                EXPECT_EQ(result.incarnation->cluster_id, identity.cluster_id);
                EXPECT_EQ(result.incarnation->node_type, identity.node_type);
                return *result.incarnation;
            }

            std::filesystem::path root_;
        };

        TEST_F(NodeIdentityTest, StoreAndLoadMetadataIdentityRoundTrip)
        {
            const auto data_dir = MakeDataDir("meta");
            const auto identity = MakeMetadataIdentity();

            const auto store = StoreNodeIdentity(
                identity,
                NodeIdentityStoreOptions{
                    .data_dir = data_dir,
                    .durability_mode = NodeIdentityDurabilityMode::kBestEffortForTests,
                    .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                    .expected_existing = {}});

            ASSERT_TRUE(store.ok()) << store.diagnostic;
            EXPECT_TRUE(store.created);
            EXPECT_FALSE(store.replaced);
            EXPECT_TRUE(std::filesystem::exists(ResolveNodeIdentityPath(data_dir)));

            const auto load = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = identity.cluster_id,
                        .node_id = identity.node_id,
                        .node_type = identity.node_type,
                        .raft_id = identity.raft_id,
                        .source = identity.source,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            ASSERT_TRUE(load.ok()) << load.diagnostic;
            ASSERT_TRUE(load.identity.has_value());
            EXPECT_EQ(load.identity->cluster_id, identity.cluster_id);
            EXPECT_EQ(load.identity->node_id, identity.node_id);
            EXPECT_EQ(load.identity->node_type, identity.node_type);
            EXPECT_EQ(load.identity->raft_id, identity.raft_id);
            EXPECT_EQ(load.identity->membership_state, identity.membership_state);
            EXPECT_EQ(load.identity->persistent_generation,
                      identity.persistent_generation);
            EXPECT_EQ(load.identity->identity_version, identity.identity_version);
            EXPECT_EQ(load.identity->created_at_unix_ms, identity.created_at_unix_ms);
            EXPECT_EQ(load.identity->source, identity.source);
        }

        TEST_F(NodeIdentityTest,
               T006StorageNodeFirstStartMissingIdentityFileCreatesLocalPersistentIdentity)
        {
            const auto data_dir = MakeDataDir("t006-storage-first-start");
            const auto identity_path = ResolveNodeIdentityPath(data_dir);
            ASSERT_FALSE(std::filesystem::exists(identity_path));

            const auto missing_before_first_start = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = ClusterId{"cluster-alpha"},
                        .node_id = ClusterNodeId{"store-node-t006"},
                        .node_type = ClusterNodeType::kStorage,
                        .raft_id = std::nullopt,
                        .source = NodeIdentitySource::kExplicitOverride,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = false});

            EXPECT_EQ(missing_before_first_start.status,
                      NodeIdentityStatusCode::kNotFound);
            EXPECT_FALSE(missing_before_first_start.ok());
            EXPECT_EQ(missing_before_first_start.identity_path, identity_path);
            EXPECT_TRUE(ValidationContains(
                missing_before_first_start.validation,
                NodeIdentityIssueCode::kIdentityFileNotFound));
            EXPECT_NE(missing_before_first_start.diagnostic.find("not found"),
                      std::string::npos);

            const auto identity = NodeIdentity{
                .cluster_id = "cluster-alpha",
                .node_id = "store-node-t006",
                .node_type = ClusterNodeType::kStorage,
                .raft_id = std::nullopt,
                .identity_version = kNodeIdentityCurrentVersion,
                .created_at_unix_ms = 1710000001000LL,
                .source = NodeIdentitySource::kExplicitOverride};

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = identity.cluster_id,
                            .node_id = identity.node_id,
                            .node_type = ClusterNodeType::kStorage,
                            .raft_id = std::nullopt,
                            .source = identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = identity,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            ASSERT_TRUE(first_start.identity.has_value());
            EXPECT_TRUE(first_start.created_new);
            EXPECT_FALSE(first_start.loaded_existing);
            EXPECT_EQ(first_start.identity_path, identity_path);
            EXPECT_TRUE(std::filesystem::exists(identity_path));
            EXPECT_EQ(first_start.identity->cluster_id, identity.cluster_id);
            EXPECT_EQ(first_start.identity->node_type, ClusterNodeType::kStorage);
            EXPECT_FALSE(first_start.identity->node_id.empty());
            EXPECT_EQ(first_start.identity->node_id, identity.node_id);
            EXPECT_FALSE(first_start.identity->raft_id.has_value());
            EXPECT_EQ(first_start.identity->created_at_unix_ms,
                      identity.created_at_unix_ms);
            EXPECT_EQ(first_start.identity->source,
                      NodeIdentitySource::kExplicitOverride);

            const auto reload = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = identity.cluster_id,
                        .node_id = identity.node_id,
                        .node_type = ClusterNodeType::kStorage,
                        .raft_id = std::nullopt,
                        .source = identity.source,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            ASSERT_TRUE(reload.ok()) << reload.diagnostic;
            ASSERT_TRUE(reload.identity.has_value());
            EXPECT_EQ(reload.identity_path, identity_path);
            EXPECT_EQ(reload.identity->cluster_id, identity.cluster_id);
            EXPECT_EQ(reload.identity->node_id, identity.node_id);
            EXPECT_EQ(reload.identity->node_type, ClusterNodeType::kStorage);
            EXPECT_FALSE(reload.identity->raft_id.has_value());
            EXPECT_EQ(reload.identity->created_at_unix_ms,
                      identity.created_at_unix_ms);
            EXPECT_EQ(reload.identity->source, identity.source);
        }

        TEST_F(
            NodeIdentityTest,
            T012FirstStartIgnoresResidualStagingFileAndCreatesFinalIdentity)
        {
            const auto data_dir = MakeDataDir("t012-ignore-staging-file");
            const auto identity_path = ResolveNodeIdentityPath(data_dir);
            const auto staging_path = data_dir / "node.identity.tmp.leftover";
            ASSERT_FALSE(std::filesystem::exists(identity_path));

            WriteTextFile(
                staging_path,
                "identity_version=2\n"
                "cluster_id=cluster-alpha\n"
                "node_id=stale-temp-node\n"
                "node_type=storage\n"
                "raft_id=\n"
                "created_at_unix_ms=1710000009999\n"
                "membership_state=non_raft\n"
                "persistent_generation=1\n"
                "source=explicit_override\n");

            const auto requested = MakeStorageIdentity("store-node-t012");
            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = requested.cluster_id,
                            .node_id = requested.node_id,
                            .node_type = ClusterNodeType::kStorage,
                            .raft_id = std::nullopt,
                            .source = requested.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = requested,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            ASSERT_TRUE(first_start.identity.has_value());
            EXPECT_TRUE(first_start.created_new);
            EXPECT_FALSE(first_start.loaded_existing);
            EXPECT_TRUE(std::filesystem::exists(identity_path));
            EXPECT_TRUE(std::filesystem::exists(staging_path));
            EXPECT_EQ(first_start.identity->node_id, requested.node_id);
            EXPECT_FALSE(first_start.identity->raft_id.has_value());

            const auto reload = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = requested.cluster_id,
                        .node_id = requested.node_id,
                        .node_type = ClusterNodeType::kStorage,
                        .raft_id = std::nullopt,
                        .source = requested.source,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            ASSERT_TRUE(reload.ok()) << reload.diagnostic;
            ASSERT_TRUE(reload.identity.has_value());
            EXPECT_EQ(reload.identity->node_id, requested.node_id);
            const auto final_content = ReadTextFile(identity_path);
            EXPECT_NE(final_content.find("node_id=store-node-t012"),
                      std::string::npos);
            EXPECT_EQ(final_content.find("stale-temp-node"), std::string::npos);
            EXPECT_NE(ReadTextFile(staging_path).find("stale-temp-node"),
                      std::string::npos);
        }

        TEST_F(NodeIdentityTest,
               CreatesProcessIncarnationAfterFirstStartIdentity)
        {
            const auto data_dir = MakeDataDir("t013-storage-incarnation-first");
            const auto identity = MakeStorageIdentity("store-node-t013-first");

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = identity.cluster_id,
                            .node_id = identity.node_id,
                            .node_type = ClusterNodeType::kStorage,
                            .raft_id = std::nullopt,
                            .source = identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = identity,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            ASSERT_TRUE(first_start.identity.has_value());
            const auto incarnation =
                CreateIncarnationOrAssert(*first_start.identity);
            EXPECT_EQ(incarnation.startup_sequence_base, 1U);

            const auto persisted = ReadTextFile(first_start.identity_path);
            EXPECT_EQ(persisted.find("incarnation"), std::string::npos);
            EXPECT_EQ(persisted.find("boot_epoch"), std::string::npos);
            EXPECT_EQ(persisted.find("startup_sequence"), std::string::npos);
        }

        TEST_F(NodeIdentityTest,
               RestartReusesNodeIdButCreatesNewIncarnation)
        {
            const auto data_dir = MakeDataDir("t013-storage-restart");
            const auto identity = MakeStorageIdentity("store-node-t013-restart");

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = identity.cluster_id,
                            .node_id = identity.node_id,
                            .node_type = ClusterNodeType::kStorage,
                            .raft_id = std::nullopt,
                            .source = identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = identity,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            ASSERT_TRUE(first_start.identity.has_value());
            const auto first_incarnation =
                CreateIncarnationOrAssert(*first_start.identity);

            const auto restart = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = identity.cluster_id,
                            .node_id = identity.node_id,
                            .node_type = ClusterNodeType::kStorage,
                            .raft_id = std::nullopt,
                            .source = identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create =
                        MakeStorageIdentity("store-node-should-not-replace"),
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(restart.ok()) << restart.diagnostic;
            ASSERT_TRUE(restart.identity.has_value());
            const auto restart_incarnation =
                CreateIncarnationOrAssert(*restart.identity);
            EXPECT_EQ(restart.identity->node_id, first_start.identity->node_id);
            EXPECT_NE(restart_incarnation.incarnation_id,
                      first_incarnation.incarnation_id);
        }

        TEST_F(NodeIdentityTest, ViewNodeRestartCreatesNewIncarnation)
        {
            const auto data_dir = MakeDataDir("t013-view-restart");
            const auto identity = MakeViewIdentity("view-node-t013");

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = identity.cluster_id,
                            .node_id = identity.node_id,
                            .node_type = ClusterNodeType::kView,
                            .raft_id = std::nullopt,
                            .source = identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = identity,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            ASSERT_TRUE(first_start.identity.has_value());
            const auto first_incarnation =
                CreateIncarnationOrAssert(*first_start.identity);

            const auto restart = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = identity.cluster_id,
                            .node_id = identity.node_id,
                            .node_type = ClusterNodeType::kView,
                            .raft_id = std::nullopt,
                            .source = identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create =
                        MakeViewIdentity("view-node-should-not-replace"),
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(restart.ok()) << restart.diagnostic;
            ASSERT_TRUE(restart.identity.has_value());
            const auto restart_incarnation =
                CreateIncarnationOrAssert(*restart.identity);
            EXPECT_EQ(restart.identity->node_id, first_start.identity->node_id);
            EXPECT_NE(restart_incarnation.incarnation_id,
                      first_incarnation.incarnation_id);
        }

        TEST_F(NodeIdentityTest, RestartReusesNodeIdButRejectsOldIncarnation)
        {
            const auto data_dir = MakeDataDir("t029-view-old-incarnation");
            const auto identity = MakeViewIdentity("view-node-t029");

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = identity.cluster_id,
                            .node_id = identity.node_id,
                            .node_type = ClusterNodeType::kView,
                            .raft_id = std::nullopt,
                            .source = identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = identity,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            ASSERT_TRUE(first_start.identity.has_value());
            const auto first_incarnation =
                CreateIncarnationOrAssert(*first_start.identity);
            const auto first_incarnation_id =
                ParseProcessIncarnationId(first_incarnation.incarnation_id);
            ASSERT_TRUE(first_incarnation_id.has_value());
            EXPECT_EQ(first_incarnation_id->durable_node_id,
                      first_start.identity->node_id);

            const auto restart = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = identity.cluster_id,
                            .node_id = identity.node_id,
                            .node_type = ClusterNodeType::kView,
                            .raft_id = std::nullopt,
                            .source = identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create =
                        MakeViewIdentity("view-node-should-not-replace-t029"),
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(restart.ok()) << restart.diagnostic;
            ASSERT_TRUE(restart.identity.has_value());
            const auto current_incarnation =
                CreateIncarnationOrAssert(*restart.identity);
            const auto current_incarnation_id =
                ParseProcessIncarnationId(current_incarnation.incarnation_id);
            ASSERT_TRUE(current_incarnation_id.has_value());

            EXPECT_EQ(restart.identity->node_id, first_start.identity->node_id);
            EXPECT_EQ(current_incarnation.node_id, first_incarnation.node_id);
            EXPECT_EQ(current_incarnation.cluster_id,
                      first_incarnation.cluster_id);
            EXPECT_EQ(current_incarnation.node_type,
                      first_incarnation.node_type);
            EXPECT_NE(current_incarnation.incarnation_id,
                      first_incarnation.incarnation_id);
            EXPECT_EQ(current_incarnation_id->durable_node_id,
                      first_incarnation_id->durable_node_id);
            EXPECT_NE(current_incarnation_id->boot_token,
                      first_incarnation_id->boot_token);
            EXPECT_TRUE(IsCurrentIncarnation(current_incarnation,
                                             current_incarnation));
            EXPECT_FALSE(IsCurrentIncarnation(first_incarnation,
                                              current_incarnation));
            EXPECT_EQ(first_incarnation.startup_sequence_base,
                      kProcessIncarnationInitialSequence);
            EXPECT_EQ(current_incarnation.startup_sequence_base,
                      kProcessIncarnationInitialSequence);
        }

        TEST_F(NodeIdentityTest,
               MetadataBootstrapVoterKeepsRaftIdButChangesIncarnation)
        {
            const auto data_dir = MakeDataDir("t013-meta-bootstrap");
            const auto identity = MakeMetadataIdentity("meta-bootstrap-t013", 88);

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = identity.cluster_id,
                            .node_id = identity.node_id,
                            .node_type = ClusterNodeType::kMetadata,
                            .raft_id = identity.raft_id,
                            .source = identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = identity,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            ASSERT_TRUE(first_start.identity.has_value());
            const auto first_incarnation =
                CreateIncarnationOrAssert(*first_start.identity);

            const auto restart = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = identity.cluster_id,
                            .node_id = identity.node_id,
                            .node_type = ClusterNodeType::kMetadata,
                            .raft_id = identity.raft_id,
                            .source = identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create =
                        MakeMetadataIdentity("meta-bootstrap-other", 99),
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(restart.ok()) << restart.diagnostic;
            ASSERT_TRUE(restart.identity.has_value());
            ASSERT_TRUE(restart.identity->raft_id.has_value());
            EXPECT_EQ(*restart.identity->raft_id, 88);

            const auto restart_incarnation =
                CreateIncarnationOrAssert(*restart.identity);
            EXPECT_EQ(restart.identity->node_id, first_start.identity->node_id);
            EXPECT_NE(restart_incarnation.incarnation_id,
                      first_incarnation.incarnation_id);
        }

        TEST_F(NodeIdentityTest,
               DynamicJoinCandidateIncarnationDoesNotPromoteToVoter)
        {
            const auto data_dir = MakeDataDir("t013-meta-candidate");
            const auto identity = MakeMetadataDynamicJoinCandidateIdentity(
                "meta-candidate-t013", std::nullopt);

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = identity.cluster_id,
                            .node_id = identity.node_id,
                            .node_type = ClusterNodeType::kMetadata,
                            .raft_id = std::nullopt,
                            .source = identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = identity,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            ASSERT_TRUE(first_start.identity.has_value());
            EXPECT_EQ(first_start.identity->membership_state,
                      NodeIdentityMembershipState::kCandidate);
            const auto first_incarnation =
                CreateIncarnationOrAssert(*first_start.identity);

            const auto restart = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = identity.cluster_id,
                            .node_id = identity.node_id,
                            .node_type = ClusterNodeType::kMetadata,
                            .raft_id = std::nullopt,
                            .source = identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create =
                        MakeMetadataDynamicJoinCandidateIdentity(
                            "meta-candidate-should-not-replace",
                            std::nullopt),
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(restart.ok()) << restart.diagnostic;
            ASSERT_TRUE(restart.identity.has_value());
            EXPECT_EQ(restart.identity->membership_state,
                      NodeIdentityMembershipState::kCandidate);
            EXPECT_NE(restart.identity->membership_state,
                      NodeIdentityMembershipState::kVoter);

            const auto restart_incarnation =
                CreateIncarnationOrAssert(*restart.identity);
            EXPECT_EQ(restart.identity->node_id, first_start.identity->node_id);
            EXPECT_NE(restart_incarnation.incarnation_id,
                      first_incarnation.incarnation_id);
        }

        TEST_F(NodeIdentityTest, InvalidIdentityDoesNotCreateProcessIncarnation)
        {
            auto invalid_identity = MakeMetadataIdentity("meta-invalid-t013", 77);
            invalid_identity.raft_id.reset();

            const auto incarnation = CreateProcessIncarnation(invalid_identity);
            EXPECT_EQ(incarnation.status, NodeIdentityStatusCode::kInvalidArgument);
            EXPECT_FALSE(incarnation.ok());
            EXPECT_FALSE(incarnation.incarnation.has_value());
            EXPECT_TRUE(ValidationContains(incarnation.validation,
                                           NodeIdentityIssueCode::kMissingRaftId));
            EXPECT_NE(incarnation.diagnostic.find("raft_id"), std::string::npos);
        }

        TEST_F(NodeIdentityTest,
               CorruptIdentityLoadDoesNotYieldUsableProcessIncarnation)
        {
            const auto data_dir = MakeDataDir("t013-corrupt-no-incarnation");
            const auto identity_path = ResolveNodeIdentityPath(data_dir);
            WriteTextFile(
                identity_path,
                "identity_version=2\n"
                "cluster_id=cluster-alpha\n"
                "node_id=meta-corrupt-t013\n"
                "node_type=metadata\n"
                "raft_id=11\n"
                "created_at_unix_ms=1710000000000\n"
                "membership_state=invalid-state\n"
                "persistent_generation=1\n"
                "source=config_generator\n");

            const auto load = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = {},
                    .require_existing = true});

            EXPECT_EQ(load.status, NodeIdentityStatusCode::kCorrupt);
            EXPECT_FALSE(load.ok());
            EXPECT_FALSE(load.identity.has_value());
            EXPECT_TRUE(ValidationContains(load.validation,
                                           NodeIdentityIssueCode::kIdentityFileCorrupt));
        }

        TEST_F(NodeIdentityTest,
               T008MetadataBootstrapVoterIdentityUsesFixedNodeIdAndRaftIdAcrossCreateAndReload)
        {
            const auto data_dir = MakeDataDir("t008-bootstrap-voter");
            const auto bootstrap_identity =
                MakeMetadataIdentity("meta-bootstrap-1", 41);

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = bootstrap_identity.cluster_id,
                            .node_id = bootstrap_identity.node_id,
                            .node_type = ClusterNodeType::kMetadata,
                            .raft_id = bootstrap_identity.raft_id,
                            .source = bootstrap_identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = bootstrap_identity,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            ASSERT_TRUE(first_start.identity.has_value());
            EXPECT_TRUE(first_start.created_new);
            EXPECT_FALSE(first_start.loaded_existing);
            EXPECT_TRUE(std::filesystem::exists(first_start.identity_path));
            EXPECT_EQ(first_start.identity->cluster_id,
                      bootstrap_identity.cluster_id);
            EXPECT_EQ(first_start.identity->node_id, "meta-bootstrap-1");
            EXPECT_EQ(first_start.identity->node_type,
                      ClusterNodeType::kMetadata);
            ASSERT_TRUE(first_start.identity->raft_id.has_value());
            EXPECT_EQ(*first_start.identity->raft_id, 41);
            EXPECT_EQ(first_start.identity->source,
                      NodeIdentitySource::kConfigGenerator);

            const auto restart = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = bootstrap_identity.cluster_id,
                        .node_id = bootstrap_identity.node_id,
                        .node_type = ClusterNodeType::kMetadata,
                        .raft_id = bootstrap_identity.raft_id,
                        .source = bootstrap_identity.source,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            ASSERT_TRUE(restart.ok()) << restart.diagnostic;
            ASSERT_TRUE(restart.identity.has_value());
            EXPECT_EQ(restart.identity->cluster_id, bootstrap_identity.cluster_id);
            EXPECT_EQ(restart.identity->node_id, bootstrap_identity.node_id);
            EXPECT_EQ(restart.identity->node_type, ClusterNodeType::kMetadata);
            ASSERT_TRUE(restart.identity->raft_id.has_value());
            EXPECT_EQ(*restart.identity->raft_id, 41);
            EXPECT_EQ(restart.identity->source,
                      NodeIdentitySource::kConfigGenerator);
            EXPECT_NE(restart.identity->source,
                      NodeIdentitySource::kExplicitOverride);
        }

        TEST_F(NodeIdentityTest,
               T008MetadataBootstrapVoterIdentityRejectsDifferentExpectedRaftIdOnReload)
        {
            const auto data_dir = MakeDataDir("t008-bootstrap-raft-mismatch");
            const auto bootstrap_identity =
                MakeMetadataIdentity("meta-bootstrap-2", 57);

            const auto store = StoreNodeIdentity(
                bootstrap_identity,
                NodeIdentityStoreOptions{
                    .data_dir = data_dir,
                    .durability_mode = NodeIdentityDurabilityMode::kBestEffortForTests,
                    .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                    .expected_existing = {}});

            ASSERT_TRUE(store.ok()) << store.diagnostic;

            const auto reload_with_other_raft_id = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = bootstrap_identity.cluster_id,
                        .node_id = bootstrap_identity.node_id,
                        .node_type = ClusterNodeType::kMetadata,
                        .raft_id = 99,
                        .source = bootstrap_identity.source,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            EXPECT_EQ(reload_with_other_raft_id.status,
                      NodeIdentityStatusCode::kConflict);
            EXPECT_FALSE(reload_with_other_raft_id.ok());
            EXPECT_TRUE(ValidationContains(reload_with_other_raft_id.validation,
                                           NodeIdentityIssueCode::kRaftIdMismatch));
            EXPECT_FALSE(reload_with_other_raft_id.diagnostic.empty());
        }

        TEST_F(
            NodeIdentityTest,
            T009MetadataDynamicJoinCandidateFirstStartCreatesIdentityFileWithoutBootstrapAuthorityMarkers)
        {
            const auto data_dir = MakeDataDir("t009-dynamic-join-candidate");
            const auto identity_path = ResolveNodeIdentityPath(data_dir);
            ASSERT_FALSE(std::filesystem::exists(identity_path));

            const auto candidate_identity =
                MakeMetadataDynamicJoinCandidateIdentity(
                    "meta-join-candidate-1", 301);
            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = candidate_identity.cluster_id,
                            .node_id = candidate_identity.node_id,
                            .node_type = ClusterNodeType::kMetadata,
                            .raft_id = candidate_identity.raft_id,
                            .source = candidate_identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = candidate_identity,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            ASSERT_TRUE(first_start.identity.has_value());
            EXPECT_TRUE(first_start.created_new);
            EXPECT_FALSE(first_start.loaded_existing);
            EXPECT_TRUE(std::filesystem::exists(first_start.identity_path));
            EXPECT_EQ(first_start.identity_path, identity_path);
            EXPECT_EQ(first_start.identity->cluster_id,
                      candidate_identity.cluster_id);
            EXPECT_EQ(first_start.identity->node_type,
                      ClusterNodeType::kMetadata);
            EXPECT_FALSE(first_start.identity->node_id.empty());
            EXPECT_EQ(first_start.identity->node_id,
                      candidate_identity.node_id);
            ASSERT_TRUE(first_start.identity->raft_id.has_value());
            EXPECT_EQ(*first_start.identity->raft_id, 301);
            EXPECT_EQ(first_start.identity->membership_state,
                      NodeIdentityMembershipState::kCandidate);
            EXPECT_EQ(first_start.identity->persistent_generation, 1U);
            EXPECT_EQ(first_start.identity->source,
                      NodeIdentitySource::kExplicitOverride);

            const auto persisted = ReadTextFile(identity_path);
            EXPECT_NE(persisted.find("cluster_id=cluster-alpha"),
                      std::string::npos);
            EXPECT_NE(persisted.find("node_type=metadata"),
                      std::string::npos);
            EXPECT_NE(persisted.find("node_id=meta-join-candidate-1"),
                      std::string::npos);
            EXPECT_NE(persisted.find("raft_id=301"), std::string::npos);
            EXPECT_NE(persisted.find("membership_state=candidate"),
                      std::string::npos);
            EXPECT_NE(persisted.find("persistent_generation=1"),
                      std::string::npos);
            EXPECT_NE(persisted.find("source=explicit_override"),
                      std::string::npos);
            EXPECT_EQ(persisted.find("initial_role="), std::string::npos);
            EXPECT_EQ(persisted.find("voter"), std::string::npos);
        }

        TEST_F(
            NodeIdentityTest,
            T009MetadataDynamicJoinCandidateCannotBeReloadedAsBootstrapVoterFromLocalFile)
        {
            const auto data_dir =
                MakeDataDir("t009-dynamic-join-candidate-reload");
            const auto candidate_identity =
                MakeMetadataDynamicJoinCandidateIdentity(
                    "meta-join-candidate-2", 302);

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = candidate_identity.cluster_id,
                            .node_id = candidate_identity.node_id,
                            .node_type = ClusterNodeType::kMetadata,
                            .raft_id = candidate_identity.raft_id,
                            .source = candidate_identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = candidate_identity,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;

            const auto reload_as_bootstrap_voter = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = candidate_identity.cluster_id,
                        .node_id = candidate_identity.node_id,
                        .node_type = ClusterNodeType::kMetadata,
                        .raft_id = candidate_identity.raft_id,
                        .source = NodeIdentitySource::kConfigGenerator,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            EXPECT_EQ(reload_as_bootstrap_voter.status,
                      NodeIdentityStatusCode::kConflict);
            EXPECT_FALSE(reload_as_bootstrap_voter.ok());
            EXPECT_TRUE(ValidationContains(
                reload_as_bootstrap_voter.validation,
                NodeIdentityIssueCode::kSourceMismatch));
            EXPECT_NE(reload_as_bootstrap_voter.diagnostic.find("source mismatch"),
                      std::string::npos);
        }

        TEST_F(
            NodeIdentityTest,
            T055MetadataDynamicJoinCandidateCannotPersistLocalVoterMembershipState)
        {
            const auto data_dir =
                MakeDataDir("t055-dynamic-join-candidate-local-voter");
            auto invalid_candidate_identity =
                MakeMetadataDynamicJoinCandidateIdentity(
                    "meta-join-candidate-local-voter",
                    401);
            invalid_candidate_identity.membership_state =
                NodeIdentityMembershipState::kVoter;

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = invalid_candidate_identity.cluster_id,
                            .node_id = invalid_candidate_identity.node_id,
                            .node_type = ClusterNodeType::kMetadata,
                            .raft_id = invalid_candidate_identity.raft_id,
                            .source = invalid_candidate_identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = invalid_candidate_identity,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            EXPECT_EQ(first_start.status, NodeIdentityStatusCode::kInvalidArgument);
            EXPECT_FALSE(first_start.ok());
            EXPECT_FALSE(first_start.identity.has_value());
            EXPECT_FALSE(std::filesystem::exists(first_start.identity_path));
            EXPECT_TRUE(ValidationContains(first_start.validation,
                                           NodeIdentityIssueCode::kInvalidMembershipState));
            EXPECT_NE(first_start.diagnostic.find("must not persist voter membership_state"),
                      std::string::npos);
        }

        TEST_F(
            NodeIdentityTest,
            T055MetadataDynamicJoinCandidateReloadAsCommittedVoterFailsOnMembershipStateMismatch)
        {
            const auto data_dir =
                MakeDataDir("t055-dynamic-join-candidate-reload-voter");
            const auto candidate_identity =
                MakeMetadataDynamicJoinCandidateIdentity(
                    "meta-join-candidate-reload-voter",
                    402);

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = candidate_identity.cluster_id,
                            .node_id = candidate_identity.node_id,
                            .node_type = ClusterNodeType::kMetadata,
                            .raft_id = candidate_identity.raft_id,
                            .source = candidate_identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = candidate_identity,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;

            const auto reload_as_committed_voter = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = candidate_identity.cluster_id,
                        .node_id = candidate_identity.node_id,
                        .node_type = ClusterNodeType::kMetadata,
                        .raft_id = candidate_identity.raft_id,
                        .membership_state = NodeIdentityMembershipState::kVoter,
                        .source = candidate_identity.source,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            EXPECT_EQ(reload_as_committed_voter.status,
                      NodeIdentityStatusCode::kConflict);
            EXPECT_FALSE(reload_as_committed_voter.ok());
            EXPECT_TRUE(ValidationContains(
                reload_as_committed_voter.validation,
                NodeIdentityIssueCode::kMembershipStateMismatch));
            EXPECT_NE(reload_as_committed_voter.diagnostic.find("membership_state mismatch"),
                      std::string::npos);
        }

        TEST_F(
            NodeIdentityTest,
            T009MetadataDynamicJoinCandidateWithoutRaftIdPersistsJoiningStateWithoutBootstrapAuthority)
        {
            const auto data_dir =
                MakeDataDir("t009-dynamic-join-candidate-no-raft");
            const auto identity_path = ResolveNodeIdentityPath(data_dir);
            const auto candidate_identity =
                MakeMetadataDynamicJoinCandidateIdentity(
                    "meta-join-candidate-no-raft", std::nullopt);

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = candidate_identity.cluster_id,
                            .node_id = candidate_identity.node_id,
                            .node_type = ClusterNodeType::kMetadata,
                            .raft_id = std::nullopt,
                            .source = candidate_identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = candidate_identity,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            ASSERT_TRUE(first_start.identity.has_value());
            EXPECT_FALSE(first_start.loaded_existing);
            EXPECT_TRUE(first_start.created_new);
            EXPECT_TRUE(std::filesystem::exists(identity_path));
            EXPECT_EQ(first_start.identity->cluster_id,
                      candidate_identity.cluster_id);
            EXPECT_EQ(first_start.identity->node_type,
                      ClusterNodeType::kMetadata);
            EXPECT_EQ(first_start.identity->node_id,
                      candidate_identity.node_id);
            EXPECT_FALSE(first_start.identity->raft_id.has_value());
            EXPECT_EQ(first_start.identity->membership_state,
                      NodeIdentityMembershipState::kCandidate);
            EXPECT_EQ(first_start.identity->persistent_generation, 1U);

            const auto persisted = ReadTextFile(identity_path);
            EXPECT_NE(persisted.find("raft_id=\n"), std::string::npos);
            EXPECT_NE(persisted.find("membership_state=candidate"),
                      std::string::npos);
            EXPECT_EQ(persisted.find("voter"), std::string::npos);
        }

        TEST_F(NodeIdentityTest,
               T067StorageNodeFirstStartCreatesStableIdentityAndReloadsIt)
        {
            const auto data_dir = MakeDataDir("t067-first-start");
            const auto identity = MakeStorageIdentity("store-node-t067");

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = identity.cluster_id,
                            .node_id = identity.node_id,
                            .node_type = ClusterNodeType::kStorage,
                            .raft_id = std::nullopt,
                            .source = identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = identity,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            ASSERT_TRUE(first_start.identity.has_value());
            EXPECT_TRUE(first_start.created_new);
            EXPECT_FALSE(first_start.loaded_existing);
            EXPECT_EQ(first_start.identity->node_id, identity.node_id);
            EXPECT_EQ(first_start.identity->node_type, ClusterNodeType::kStorage);
            EXPECT_FALSE(first_start.identity->raft_id.has_value());
            EXPECT_FALSE(first_start.diagnostic.empty());
            EXPECT_TRUE(std::filesystem::exists(first_start.identity_path));

            const auto reload = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = identity.cluster_id,
                        .node_id = identity.node_id,
                        .node_type = ClusterNodeType::kStorage,
                        .raft_id = std::nullopt,
                        .source = identity.source,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            ASSERT_TRUE(reload.ok()) << reload.diagnostic;
            ASSERT_TRUE(reload.identity.has_value());
            EXPECT_EQ(reload.identity->node_id, identity.node_id);
            EXPECT_EQ(reload.identity->cluster_id, identity.cluster_id);
            EXPECT_EQ(reload.identity->node_type, ClusterNodeType::kStorage);
            EXPECT_EQ(reload.identity->source, NodeIdentitySource::kViewNodeAllocator);
            EXPECT_FALSE(reload.identity->raft_id.has_value());
            EXPECT_FALSE(reload.diagnostic.empty());
        }

        TEST_F(NodeIdentityTest,
               T007ViewNodeFirstStartCreatesPersistentIdentityAndReloadsIt)
        {
            const auto data_dir = MakeDataDir("t007-view-first-start");
            const auto identity_path = ResolveNodeIdentityPath(data_dir);
            ASSERT_FALSE(std::filesystem::exists(identity_path));

            const auto identity = MakeViewIdentity("view-node-t007");
            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = identity.cluster_id,
                            .node_id = identity.node_id,
                            .node_type = ClusterNodeType::kView,
                            .raft_id = std::nullopt,
                            .source = identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = identity,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            ASSERT_TRUE(first_start.identity.has_value());
            EXPECT_TRUE(first_start.created_new);
            EXPECT_FALSE(first_start.loaded_existing);
            EXPECT_TRUE(std::filesystem::exists(first_start.identity_path));
            EXPECT_EQ(first_start.identity_path, identity_path);
            EXPECT_EQ(first_start.identity->cluster_id, identity.cluster_id);
            EXPECT_EQ(first_start.identity->node_type, ClusterNodeType::kView);
            EXPECT_FALSE(first_start.identity->node_id.empty());
            EXPECT_EQ(first_start.identity->node_id, identity.node_id);
            EXPECT_FALSE(first_start.identity->raft_id.has_value());
            EXPECT_EQ(first_start.identity->source, NodeIdentitySource::kConfigGenerator);

            const auto reload = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = identity.cluster_id,
                        .node_id = identity.node_id,
                        .node_type = ClusterNodeType::kView,
                        .raft_id = std::nullopt,
                        .source = identity.source,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            ASSERT_TRUE(reload.ok()) << reload.diagnostic;
            ASSERT_TRUE(reload.identity.has_value());
            EXPECT_EQ(reload.identity->cluster_id, identity.cluster_id);
            EXPECT_EQ(reload.identity->node_id, identity.node_id);
            EXPECT_EQ(reload.identity->node_type, ClusterNodeType::kView);
            EXPECT_FALSE(reload.identity->raft_id.has_value());
            EXPECT_EQ(reload.identity->source, NodeIdentitySource::kConfigGenerator);
        }

        TEST_F(NodeIdentityTest, LoadOrCreateReusesExistingIdentityOnRestart)
        {
            const auto data_dir = MakeDataDir("restartable");
            const auto identity = MakeStorageIdentity();

            const auto first = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = identity.cluster_id,
                            .node_id = identity.node_id,
                            .node_type = identity.node_type,
                            .raft_id = std::nullopt,
                            .source = identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = identity,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode = NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first.ok()) << first.diagnostic;
            EXPECT_TRUE(first.created_new);
            EXPECT_FALSE(first.loaded_existing);

            const auto restart = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = identity.cluster_id,
                            .node_id = identity.node_id,
                            .node_type = identity.node_type,
                            .raft_id = std::nullopt,
                            .source = identity.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = MakeStorageIdentity("store-node-should-not-appear"),
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode = NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(restart.ok()) << restart.diagnostic;
            ASSERT_TRUE(restart.identity.has_value());
            EXPECT_TRUE(restart.loaded_existing);
            EXPECT_FALSE(restart.created_new);
            EXPECT_EQ(restart.identity->node_id, identity.node_id);
            EXPECT_EQ(restart.identity->node_type, identity.node_type);
            EXPECT_EQ(restart.identity->raft_id, identity.raft_id);
        }

        TEST_F(NodeIdentityTest,
               T068RestartReusesExistingStorageNodeIdWithoutSilentOverwrite)
        {
            const auto data_dir = MakeDataDir("t068-restart");
            const auto original = MakeStorageIdentity("store-node-t068");

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = original.cluster_id,
                            .node_id = original.node_id,
                            .node_type = ClusterNodeType::kStorage,
                            .raft_id = std::nullopt,
                            .source = original.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = original,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            ASSERT_TRUE(first_start.identity.has_value());
            const auto first_identity = *first_start.identity;

            const auto restart = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = original.cluster_id,
                            .node_id = original.node_id,
                            .node_type = ClusterNodeType::kStorage,
                            .raft_id = std::nullopt,
                            .source = original.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = MakeStorageIdentity("store-node-should-not-replace"),
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(restart.ok()) << restart.diagnostic;
            ASSERT_TRUE(restart.identity.has_value());
            EXPECT_TRUE(restart.loaded_existing);
            EXPECT_FALSE(restart.created_new);
            EXPECT_EQ(restart.identity->node_id, first_identity.node_id);
            EXPECT_EQ(restart.identity->cluster_id, first_identity.cluster_id);
            EXPECT_EQ(restart.identity->node_type, first_identity.node_type);
            EXPECT_EQ(restart.identity->created_at_unix_ms,
                      first_identity.created_at_unix_ms);
            EXPECT_EQ(restart.identity->source, first_identity.source);

            const auto verify = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = original.cluster_id,
                        .node_id = original.node_id,
                        .node_type = ClusterNodeType::kStorage,
                        .raft_id = std::nullopt,
                        .source = original.source,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            ASSERT_TRUE(verify.ok()) << verify.diagnostic;
            ASSERT_TRUE(verify.identity.has_value());
            EXPECT_EQ(verify.identity->node_id, "store-node-t068");
            EXPECT_NE(verify.identity->node_id, "store-node-should-not-replace");
        }

        TEST_F(NodeIdentityTest,
               T007ViewNodeRestartReusesStableNodeIdWithoutAuthorityDrift)
        {
            const auto data_dir = MakeDataDir("t007-view-restart");
            const auto original = MakeViewIdentity("view-node-t007-restart");

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = original.cluster_id,
                            .node_id = original.node_id,
                            .node_type = ClusterNodeType::kView,
                            .raft_id = std::nullopt,
                            .source = original.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = original,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            ASSERT_TRUE(first_start.identity.has_value());
            const auto first_identity = *first_start.identity;

            const auto restart = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = original.cluster_id,
                            .node_id = original.node_id,
                            .node_type = ClusterNodeType::kView,
                            .raft_id = std::nullopt,
                            .source = original.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create =
                        MakeViewIdentity("view-node-should-not-replace"),
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(restart.ok()) << restart.diagnostic;
            ASSERT_TRUE(restart.identity.has_value());
            EXPECT_TRUE(restart.loaded_existing);
            EXPECT_FALSE(restart.created_new);
            EXPECT_EQ(restart.identity->node_id, first_identity.node_id);
            EXPECT_EQ(restart.identity->cluster_id, first_identity.cluster_id);
            EXPECT_EQ(restart.identity->node_type, ClusterNodeType::kView);
            EXPECT_EQ(restart.identity->created_at_unix_ms,
                      first_identity.created_at_unix_ms);
            EXPECT_EQ(restart.identity->source, first_identity.source);
            EXPECT_FALSE(restart.identity->raft_id.has_value());

            const auto verify = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = original.cluster_id,
                        .node_id = original.node_id,
                        .node_type = ClusterNodeType::kView,
                        .raft_id = std::nullopt,
                        .source = original.source,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            ASSERT_TRUE(verify.ok()) << verify.diagnostic;
            ASSERT_TRUE(verify.identity.has_value());
            EXPECT_EQ(verify.identity->node_id, original.node_id);
            EXPECT_NE(verify.identity->node_id, "view-node-should-not-replace");
            EXPECT_FALSE(verify.identity->raft_id.has_value());
        }

        TEST_F(NodeIdentityTest, LoadReportsConflictWhenExpectedNodeIdMismatches)
        {
            const auto data_dir = MakeDataDir("mismatch");
            const auto identity = MakeMetadataIdentity("meta-node-a", 7);

            const auto store = StoreNodeIdentity(
                identity,
                NodeIdentityStoreOptions{
                    .data_dir = data_dir,
                    .durability_mode = NodeIdentityDurabilityMode::kBestEffortForTests,
                    .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                    .expected_existing = {}});
            ASSERT_TRUE(store.ok()) << store.diagnostic;

            const auto load = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = identity.cluster_id,
                        .node_id = "meta-node-b",
                        .node_type = ClusterNodeType::kMetadata,
                        .raft_id = identity.raft_id,
                        .source = identity.source,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            EXPECT_EQ(load.status, NodeIdentityStatusCode::kConflict);
            EXPECT_FALSE(load.ok());
            EXPECT_TRUE(ValidationContains(load.validation,
                                           NodeIdentityIssueCode::kNodeIdMismatch));
            EXPECT_FALSE(load.diagnostic.empty());
        }

        TEST_F(NodeIdentityTest,
               T069StorageNodeIdentityMismatchFailsAndKeepsExistingIdentity)
        {
            const auto data_dir = MakeDataDir("t069-mismatch");
            const auto original = MakeStorageIdentity("store-node-t069");

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = original.cluster_id,
                            .node_id = original.node_id,
                            .node_type = ClusterNodeType::kStorage,
                            .raft_id = std::nullopt,
                            .source = original.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = original,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;

            const auto mismatch = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = "cluster-beta",
                            .node_id = "store-node-t069-other",
                            .node_type = ClusterNodeType::kStorage,
                            .raft_id = std::nullopt,
                            .source = original.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = MakeStorageIdentity("store-node-should-not-appear"),
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            EXPECT_EQ(mismatch.status, NodeIdentityStatusCode::kConflict);
            EXPECT_FALSE(mismatch.ok());
            EXPECT_FALSE(mismatch.loaded_existing);
            EXPECT_FALSE(mismatch.created_new);
            EXPECT_TRUE(ValidationContains(mismatch.validation,
                                           NodeIdentityIssueCode::kClusterIdMismatch));
            EXPECT_TRUE(ValidationContains(mismatch.validation,
                                           NodeIdentityIssueCode::kNodeIdMismatch));
            EXPECT_NE(mismatch.diagnostic.find("expected"), std::string::npos);

            const auto verify = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = original.cluster_id,
                        .node_id = original.node_id,
                        .node_type = ClusterNodeType::kStorage,
                        .raft_id = std::nullopt,
                        .source = original.source,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            ASSERT_TRUE(verify.ok()) << verify.diagnostic;
            ASSERT_TRUE(verify.identity.has_value());
            EXPECT_EQ(verify.identity->node_id, original.node_id);
            EXPECT_EQ(verify.identity->cluster_id, original.cluster_id);
        }

        TEST_F(NodeIdentityTest,
               T010ClusterIdMismatchFailsFastAndDoesNotOverwriteExistingIdentity)
        {
            const auto data_dir = MakeDataDir("t010-cluster-mismatch");
            const auto original =
                MakeStorageIdentity("store-node-t010-cluster");

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = original.cluster_id,
                            .node_id = original.node_id,
                            .node_type = ClusterNodeType::kStorage,
                            .raft_id = std::nullopt,
                            .source = original.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = original,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            const auto identity_path = first_start.identity_path;
            const auto persisted_before = ReadTextFile(identity_path);

            auto requested = MakeStorageIdentity("store-node-should-not-replace");
            requested.cluster_id = "cluster-beta";
            const auto mismatch = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = ClusterId{"cluster-beta"},
                            .node_id = original.node_id,
                            .node_type = ClusterNodeType::kStorage,
                            .raft_id = std::nullopt,
                            .source = original.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = requested,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            EXPECT_EQ(mismatch.status, NodeIdentityStatusCode::kConflict);
            EXPECT_FALSE(mismatch.ok());
            EXPECT_FALSE(mismatch.loaded_existing);
            EXPECT_FALSE(mismatch.created_new);
            EXPECT_TRUE(ValidationContains(mismatch.validation,
                                           NodeIdentityIssueCode::kClusterIdMismatch));
            EXPECT_NE(mismatch.diagnostic.find("cluster_id mismatch"),
                      std::string::npos);
            EXPECT_NE(mismatch.diagnostic.find("cluster-beta"),
                      std::string::npos);
            EXPECT_NE(mismatch.diagnostic.find("cluster-alpha"),
                      std::string::npos);

            EXPECT_TRUE(std::filesystem::exists(identity_path));
            EXPECT_EQ(ReadTextFile(identity_path), persisted_before);

            const auto verify = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = original.cluster_id,
                        .node_id = original.node_id,
                        .node_type = ClusterNodeType::kStorage,
                        .raft_id = std::nullopt,
                        .source = original.source,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            ASSERT_TRUE(verify.ok()) << verify.diagnostic;
            ASSERT_TRUE(verify.identity.has_value());
            EXPECT_EQ(verify.identity->cluster_id, original.cluster_id);
            EXPECT_EQ(verify.identity->node_id, original.node_id);
        }

        TEST_F(NodeIdentityTest,
               T010NodeTypeMismatchFailsFastAndDoesNotRewriteExistingIdentity)
        {
            const auto data_dir = MakeDataDir("t010-node-type-mismatch");
            const auto original =
                MakeStorageIdentity("store-node-t010-node-type");

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = original.cluster_id,
                            .node_id = original.node_id,
                            .node_type = ClusterNodeType::kStorage,
                            .raft_id = std::nullopt,
                            .source = original.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = original,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            const auto identity_path = first_start.identity_path;
            const auto persisted_before = ReadTextFile(identity_path);

            const auto mismatch = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = original.cluster_id,
                            .node_id = original.node_id,
                            .node_type = ClusterNodeType::kMetadata,
                            .raft_id = std::nullopt,
                            .source = original.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create =
                        MakeMetadataIdentity("meta-node-should-not-replace", 81),
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            EXPECT_EQ(mismatch.status, NodeIdentityStatusCode::kConflict);
            EXPECT_FALSE(mismatch.ok());
            EXPECT_FALSE(mismatch.loaded_existing);
            EXPECT_FALSE(mismatch.created_new);
            EXPECT_TRUE(ValidationContains(mismatch.validation,
                                           NodeIdentityIssueCode::kNodeTypeMismatch));
            EXPECT_NE(mismatch.diagnostic.find("node_type mismatch"),
                      std::string::npos);
            EXPECT_NE(mismatch.diagnostic.find("metadata"), std::string::npos);
            EXPECT_NE(mismatch.diagnostic.find("storage"), std::string::npos);

            EXPECT_TRUE(std::filesystem::exists(identity_path));
            EXPECT_EQ(ReadTextFile(identity_path), persisted_before);

            const auto verify = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = original.cluster_id,
                        .node_id = original.node_id,
                        .node_type = ClusterNodeType::kStorage,
                        .raft_id = std::nullopt,
                        .source = original.source,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            ASSERT_TRUE(verify.ok()) << verify.diagnostic;
            ASSERT_TRUE(verify.identity.has_value());
            EXPECT_EQ(verify.identity->node_type, ClusterNodeType::kStorage);
            EXPECT_FALSE(verify.identity->raft_id.has_value());
        }

        TEST_F(
            NodeIdentityTest,
            T010MetadataRaftIdMismatchFailsFastAndPreservesBootstrapIdentity)
        {
            const auto data_dir = MakeDataDir("t010-metadata-raft-mismatch");
            const auto original =
                MakeMetadataIdentity("meta-bootstrap-t010", 77);

            const auto first_start = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = original.cluster_id,
                            .node_id = original.node_id,
                            .node_type = ClusterNodeType::kMetadata,
                            .raft_id = original.raft_id,
                            .source = original.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = original,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            ASSERT_TRUE(first_start.ok()) << first_start.diagnostic;
            const auto identity_path = first_start.identity_path;
            const auto persisted_before = ReadTextFile(identity_path);

            const auto mismatch = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = original.cluster_id,
                            .node_id = original.node_id,
                            .node_type = ClusterNodeType::kMetadata,
                            .raft_id = 99,
                            .source = original.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create =
                        MakeMetadataIdentity("meta-bootstrap-t010", 99),
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            EXPECT_EQ(mismatch.status, NodeIdentityStatusCode::kConflict);
            EXPECT_FALSE(mismatch.ok());
            EXPECT_FALSE(mismatch.loaded_existing);
            EXPECT_FALSE(mismatch.created_new);
            EXPECT_TRUE(ValidationContains(mismatch.validation,
                                           NodeIdentityIssueCode::kRaftIdMismatch));
            EXPECT_NE(mismatch.diagnostic.find("raft_id mismatch"),
                      std::string::npos);
            EXPECT_NE(mismatch.diagnostic.find("99"), std::string::npos);
            EXPECT_NE(mismatch.diagnostic.find("77"), std::string::npos);

            EXPECT_TRUE(std::filesystem::exists(identity_path));
            EXPECT_EQ(ReadTextFile(identity_path), persisted_before);

            const auto verify = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = original.cluster_id,
                        .node_id = original.node_id,
                        .node_type = ClusterNodeType::kMetadata,
                        .raft_id = original.raft_id,
                        .source = original.source,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            ASSERT_TRUE(verify.ok()) << verify.diagnostic;
            ASSERT_TRUE(verify.identity.has_value());
            ASSERT_TRUE(verify.identity->raft_id.has_value());
            EXPECT_EQ(*verify.identity->raft_id, 77);
        }

        TEST_F(NodeIdentityTest, LoadRejectsCorruptIdentityFile)
        {
            const auto data_dir = MakeDataDir("corrupt");
            const auto identity_path = ResolveNodeIdentityPath(data_dir);

            WriteTextFile(
                identity_path,
                "identity_version=2\n"
                "cluster_id=cluster-alpha\n"
                "node_id=meta-node-1\n"
                "node_type=metadata\n"
                "raft_id=11\n"
                "created_at_unix_ms=1710000000000\n"
                "membership_state=not-a-valid-state\n"
                "persistent_generation=1\n"
                "source=config_generator\n");

            const auto load = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = {},
                    .require_existing = true});

            EXPECT_EQ(load.status, NodeIdentityStatusCode::kCorrupt);
            EXPECT_FALSE(load.ok());
            EXPECT_TRUE(ValidationContains(load.validation,
                                           NodeIdentityIssueCode::kIdentityFileCorrupt));
            EXPECT_FALSE(load.diagnostic.empty());
        }

        TEST_F(
            NodeIdentityTest,
            RejectsLegacyV1IdentityWithoutCompatibility)
        {
            const auto data_dir = MakeDataDir("t011-legacy-identity");
            const auto identity_path = ResolveNodeIdentityPath(data_dir);
            const std::string legacy_content =
                "identity_version=1\n"
                "cluster_id=cluster-alpha\n"
                "node_id=meta-legacy-1\n"
                "node_type=metadata\n"
                "raft_id=41\n"
                "created_at_unix_ms=1710000000000\n"
                "source=config_generator\n";

            WriteTextFile(identity_path, legacy_content);

            const auto load = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = ClusterId{"cluster-alpha"},
                        .node_id = ClusterNodeId{"meta-legacy-1"},
                        .node_type = ClusterNodeType::kMetadata,
                        .raft_id = 41,
                        .membership_state =
                            NodeIdentityMembershipState::kVoter,
                        .source = NodeIdentitySource::kConfigGenerator,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            EXPECT_EQ(load.status, NodeIdentityStatusCode::kCorrupt);
            EXPECT_FALSE(load.ok());
            EXPECT_FALSE(load.identity.has_value());
            EXPECT_TRUE(ValidationContains(load.validation,
                                           NodeIdentityIssueCode::kIdentityFileCorrupt));
            EXPECT_NE(load.diagnostic.find("membership_state"),
                      std::string::npos);
            EXPECT_NE(load.diagnostic.find("persistent_generation"),
                      std::string::npos);
            EXPECT_EQ(ReadTextFile(identity_path), legacy_content);

            const auto requested = MakeMetadataIdentity("meta-legacy-1", 41);
            const auto load_or_create = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = requested.cluster_id,
                            .node_id = requested.node_id,
                            .node_type = ClusterNodeType::kMetadata,
                            .raft_id = requested.raft_id,
                            .source = requested.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = requested,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            EXPECT_EQ(load_or_create.status, NodeIdentityStatusCode::kCorrupt);
            EXPECT_FALSE(load_or_create.ok());
            EXPECT_FALSE(load_or_create.loaded_existing);
            EXPECT_FALSE(load_or_create.created_new);
            EXPECT_TRUE(ValidationContains(load_or_create.validation,
                                           NodeIdentityIssueCode::kIdentityFileCorrupt));
            EXPECT_EQ(ReadTextFile(identity_path), legacy_content);
        }

        TEST_F(NodeIdentityTest, MissingNewRequiredIdentityFieldsFailFast)
        {
            const auto data_dir = MakeDataDir("t011-missing-required-fields");
            const auto identity_path = ResolveNodeIdentityPath(data_dir);
            const std::string incomplete_content =
                "identity_version=2\n"
                "cluster_id=cluster-alpha\n"
                "node_id=store-node-incomplete\n"
                "node_type=storage\n"
                "raft_id=\n"
                "created_at_unix_ms=1710000000000\n"
                "source=explicit_override\n";
            WriteTextFile(identity_path, incomplete_content);

            const auto load = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = ExpectedNodeIdentity{
                        .cluster_id = ClusterId{"cluster-alpha"},
                        .node_id = ClusterNodeId{"store-node-incomplete"},
                        .node_type = ClusterNodeType::kStorage,
                        .raft_id = std::nullopt,
                        .source = NodeIdentitySource::kExplicitOverride,
                        .require_raft_id_for_metadata = true,
                        .forbid_raft_id_for_non_metadata = true},
                    .require_existing = true});

            EXPECT_EQ(load.status, NodeIdentityStatusCode::kCorrupt);
            EXPECT_FALSE(load.ok());
            EXPECT_TRUE(ValidationContains(load.validation,
                                           NodeIdentityIssueCode::kIdentityFileCorrupt));
            EXPECT_NE(load.diagnostic.find("membership_state"),
                      std::string::npos);
            EXPECT_NE(load.diagnostic.find("persistent_generation"),
                      std::string::npos);

            const auto requested = MakeStorageIdentity("store-node-incomplete");
            const auto load_or_create = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = requested.cluster_id,
                            .node_id = requested.node_id,
                            .node_type = ClusterNodeType::kStorage,
                            .raft_id = std::nullopt,
                            .source = requested.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = requested,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            EXPECT_EQ(load_or_create.status, NodeIdentityStatusCode::kCorrupt);
            EXPECT_FALSE(load_or_create.ok());
            EXPECT_FALSE(load_or_create.loaded_existing);
            EXPECT_FALSE(load_or_create.created_new);
            EXPECT_TRUE(ValidationContains(load_or_create.validation,
                                           NodeIdentityIssueCode::kIdentityFileCorrupt));
            EXPECT_EQ(ReadTextFile(identity_path), incomplete_content);
        }

        TEST_F(
            NodeIdentityTest,
            T010CorruptIdentityFileFailsFastAndIsNotTreatedAsMissingOnLoadOrCreate)
        {
            const auto data_dir = MakeDataDir("t010-corrupt-load-or-create");
            const auto identity_path = ResolveNodeIdentityPath(data_dir);
            const std::string corrupt_content =
                "identity_version=1\n"
                "cluster_id=cluster-alpha\n"
                "node_id=store-node-t010-corrupt\n"
                "this-line-is-not-key-value\n";
            WriteTextFile(identity_path, corrupt_content);

            const auto requested = MakeStorageIdentity("store-node-new");
            const auto load_or_create = LoadOrCreateNodeIdentity(
                NodeIdentityLoadOrCreateRequest{
                    .load_options = NodeIdentityLoadOptions{
                        .data_dir = data_dir,
                        .expected = ExpectedNodeIdentity{
                            .cluster_id = requested.cluster_id,
                            .node_id = requested.node_id,
                            .node_type = ClusterNodeType::kStorage,
                            .raft_id = std::nullopt,
                            .source = requested.source,
                            .require_raft_id_for_metadata = true,
                            .forbid_raft_id_for_non_metadata = true},
                        .require_existing = false},
                    .identity_to_create = requested,
                    .store_options = NodeIdentityStoreOptions{
                        .data_dir = data_dir,
                        .durability_mode =
                            NodeIdentityDurabilityMode::kBestEffortForTests,
                        .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                        .expected_existing = {}}});

            EXPECT_EQ(load_or_create.status, NodeIdentityStatusCode::kCorrupt);
            EXPECT_FALSE(load_or_create.ok());
            EXPECT_FALSE(load_or_create.loaded_existing);
            EXPECT_FALSE(load_or_create.created_new);
            EXPECT_TRUE(ValidationContains(load_or_create.validation,
                                           NodeIdentityIssueCode::kIdentityFileCorrupt));
            EXPECT_NE(load_or_create.diagnostic.find("line[4]"),
                      std::string::npos);
            EXPECT_TRUE(std::filesystem::exists(identity_path));
            EXPECT_EQ(ReadTextFile(identity_path), corrupt_content);

            const auto still_corrupt = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = data_dir,
                    .expected = {},
                    .require_existing = true});

            EXPECT_EQ(still_corrupt.status, NodeIdentityStatusCode::kCorrupt);
            EXPECT_FALSE(still_corrupt.ok());
            EXPECT_TRUE(ValidationContains(still_corrupt.validation,
                                           NodeIdentityIssueCode::kIdentityFileCorrupt));
            EXPECT_EQ(ReadTextFile(identity_path), corrupt_content);
        }

        TEST_F(NodeIdentityTest, StoreRejectsStorageIdentityThatCarriesRaftId)
        {
            auto identity = MakeStorageIdentity();
            identity.raft_id = 5;

            const auto store = StoreNodeIdentity(
                identity,
                NodeIdentityStoreOptions{
                    .data_dir = MakeDataDir("storage-invalid"),
                    .durability_mode = NodeIdentityDurabilityMode::kBestEffortForTests,
                    .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                    .expected_existing = {}});

            EXPECT_EQ(store.status, NodeIdentityStatusCode::kInvalidArgument);
            EXPECT_FALSE(store.ok());
            EXPECT_TRUE(ValidationContains(store.validation,
                                           NodeIdentityIssueCode::kUnexpectedRaftId));
        }

        TEST_F(NodeIdentityTest, StoreRejectsMetadataIdentityWithoutRaftId)
        {
            auto identity = MakeMetadataIdentity();
            identity.raft_id.reset();

            const auto store = StoreNodeIdentity(
                identity,
                NodeIdentityStoreOptions{
                    .data_dir = MakeDataDir("metadata-missing-raft"),
                    .durability_mode = NodeIdentityDurabilityMode::kBestEffortForTests,
                    .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                    .expected_existing = {}});

            EXPECT_EQ(store.status, NodeIdentityStatusCode::kInvalidArgument);
            EXPECT_FALSE(store.ok());
            EXPECT_TRUE(ValidationContains(store.validation,
                                           NodeIdentityIssueCode::kMissingRaftId));
        }

        TEST_F(NodeIdentityTest, CreateOnlyModeDoesNotSilentlyOverwriteExistingIdentity)
        {
            const auto data_dir = MakeDataDir("create-only");
            const auto identity = MakeStorageIdentity("store-node-1");

            const auto first = StoreNodeIdentity(
                identity,
                NodeIdentityStoreOptions{
                    .data_dir = data_dir,
                    .durability_mode = NodeIdentityDurabilityMode::kBestEffortForTests,
                    .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                    .expected_existing = {}});
            ASSERT_TRUE(first.ok()) << first.diagnostic;

            const auto second = StoreNodeIdentity(
                identity,
                NodeIdentityStoreOptions{
                    .data_dir = data_dir,
                    .durability_mode = NodeIdentityDurabilityMode::kBestEffortForTests,
                    .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                    .expected_existing = {}});

            EXPECT_EQ(second.status, NodeIdentityStatusCode::kConflict);
            EXPECT_FALSE(second.ok());
            EXPECT_TRUE(ValidationContains(second.validation,
                                           NodeIdentityIssueCode::kExistingIdentityConflict));
        }

        TEST_F(NodeIdentityTest, LoadRejectsDataDirThatIsNotDirectory)
        {
            const auto file_path = root_ / "not-a-directory";
            WriteTextFile(file_path, "placeholder");

            const auto load = LoadNodeIdentity(
                NodeIdentityLoadOptions{
                    .data_dir = file_path,
                    .expected = {},
                    .require_existing = true});

            EXPECT_EQ(load.status, NodeIdentityStatusCode::kInvalidArgument);
            EXPECT_FALSE(load.ok());
            EXPECT_FALSE(load.diagnostic.empty());
        }

        TEST_F(NodeIdentityTest, RequiredDurabilityDoesNotSilentlySucceed)
        {
            const auto data_dir = MakeDataDir("required-durability");
            const auto identity = MakeStorageIdentity("store-node-durable");

            const auto store = StoreNodeIdentity(
                identity,
                NodeIdentityStoreOptions{
                    .data_dir = data_dir,
                    .durability_mode = NodeIdentityDurabilityMode::kRequired,
                    .store_mode = NodeIdentityStoreMode::kCreateNewOnly,
                    .expected_existing = {}});

#ifdef _WIN32
            EXPECT_EQ(store.status, NodeIdentityStatusCode::kDurabilityError);
            EXPECT_FALSE(store.ok());
            EXPECT_FALSE(store.durable);
            EXPECT_FALSE(store.diagnostic.empty());
#else
            ASSERT_TRUE(store.ok()) << store.diagnostic;
            EXPECT_TRUE(store.created);
            EXPECT_TRUE(store.durable);
#endif
        }
    } // namespace
} // namespace clusterdemo
