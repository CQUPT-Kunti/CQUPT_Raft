#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include "cluster/node_identity.h"

namespace clusterdemo
{
    namespace
    {
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
                    .identity_version = kNodeIdentityCurrentVersion,
                    .created_at_unix_ms = 1710000000000LL,
                    .source = NodeIdentitySource::kConfigGenerator};
            }

            [[nodiscard]] NodeIdentity MakeStorageIdentity(
                const std::string &node_id = "store-node-1") const
            {
                return NodeIdentity{
                    .cluster_id = "cluster-alpha",
                    .node_id = node_id,
                    .node_type = ClusterNodeType::kStorage,
                    .raft_id = std::nullopt,
                    .identity_version = kNodeIdentityCurrentVersion,
                    .created_at_unix_ms = 1710000000000LL,
                    .source = NodeIdentitySource::kViewNodeAllocator};
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
            EXPECT_EQ(load.identity->identity_version, identity.identity_version);
            EXPECT_EQ(load.identity->created_at_unix_ms, identity.created_at_unix_ms);
            EXPECT_EQ(load.identity->source, identity.source);
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

        TEST_F(NodeIdentityTest, LoadRejectsCorruptIdentityFile)
        {
            const auto data_dir = MakeDataDir("corrupt");
            const auto identity_path = ResolveNodeIdentityPath(data_dir);

            WriteTextFile(
                identity_path,
                "identity_version=1\n"
                "cluster_id=cluster-alpha\n"
                "node_id=meta-node-1\n"
                "node_type=metadata\n"
                "raft_id=11\n"
                "created_at_unix_ms=1710000000000\n");

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
