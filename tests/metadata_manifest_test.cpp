#include "raft/common/metadata_command.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace raftdemo
{
    bool ValidateMetadataCommand(const MetadataCommand &command, std::string *error);
}

namespace
{
    raftdemo::MetadataRecord MakeValidRecord()
    {
        raftdemo::MetadataRecord record;
        record.object_key = "object/manifest";
        record.object_size = 16;
        record.chunk_size = 8;
        record.chunk_count = 2;
        record.checksum = "sha256:mock-checksum";
        record.mock_locations = {"missing-node-a/chunk-0", "/definitely/not/real/chunk-1"};
        record.payload = "metadata-only";
        record.create_request_id = "create-manifest-req";
        return record;
    }
} // namespace

TEST(MetadataManifestTest, LegalManifestPassesValidation)
{
    const raftdemo::MetadataCommand command =
        raftdemo::MakeCreateMetadataCommand(MakeValidRecord());

    std::string error;
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(command, &error));
    EXPECT_EQ(error, "");
}

TEST(MetadataManifestTest, ZeroChunkSizeIsRejected)
{
    raftdemo::MetadataRecord record = MakeValidRecord();
    record.chunk_size = 0;

    const raftdemo::MetadataCommand command =
        raftdemo::MakeCreateMetadataCommand(std::move(record));

    std::string error;
    EXPECT_FALSE(raftdemo::ValidateMetadataCommand(command, &error));
    EXPECT_EQ(error, "create record chunk_size and chunk_count must be positive");
}

TEST(MetadataManifestTest, ChunkCountMismatchIsRejected)
{
    raftdemo::MetadataRecord record = MakeValidRecord();
    record.chunk_count = 3;

    const raftdemo::MetadataCommand command =
        raftdemo::MakeCreateMetadataCommand(std::move(record));

    std::string error;
    EXPECT_FALSE(raftdemo::ValidateMetadataCommand(command, &error));
    EXPECT_EQ(error, "create record chunk_count must match object_size and chunk_size");
}

TEST(MetadataManifestTest, MissingChecksumIsRejected)
{
    raftdemo::MetadataRecord record = MakeValidRecord();
    record.checksum.clear();

    const raftdemo::MetadataCommand command =
        raftdemo::MakeCreateMetadataCommand(std::move(record));

    std::string error;
    EXPECT_FALSE(raftdemo::ValidateMetadataCommand(command, &error));
    EXPECT_EQ(error, "create record missing checksum");
}

TEST(MetadataManifestTest, PayloadOverLimitIsRejected)
{
    raftdemo::MetadataRecord record = MakeValidRecord();
    record.payload = std::string(4097, 'x');

    const raftdemo::MetadataCommand command =
        raftdemo::MakeCreateMetadataCommand(std::move(record));

    std::string error;
    EXPECT_FALSE(raftdemo::ValidateMetadataCommand(command, &error));
    EXPECT_EQ(error, "create record payload exceeds limit");
}

TEST(MetadataManifestTest, EmptyMockLocationsIsRejected)
{
    raftdemo::MetadataRecord record = MakeValidRecord();
    record.mock_locations.clear();

    const raftdemo::MetadataCommand command =
        raftdemo::MakeCreateMetadataCommand(std::move(record));

    std::string error;
    EXPECT_FALSE(raftdemo::ValidateMetadataCommand(command, &error));
    EXPECT_EQ(error, "create record missing mock_locations");
}

TEST(MetadataManifestTest, NonExistentMockStorageNodeLocationsRemainAllowed)
{
    raftdemo::MetadataRecord record = MakeValidRecord();
    record.mock_locations = {
        "node-does-not-exist/chunk-0",
        "/tmp/this/path/is/not/validated/by/metadata",
        "rack-9/slot-404"};

    const raftdemo::MetadataCommand command =
        raftdemo::MakeCreateMetadataCommand(std::move(record));

    std::string error;
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(command, &error));
    EXPECT_EQ(error, "");
}
