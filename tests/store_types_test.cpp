#include <gtest/gtest.h>

#include "store/common/store_types.h"

namespace raftdemo {
namespace {

TEST(StoreTypesTest, StorageNodeStatusCodeCoversExpectedClassification) {
  EXPECT_STREQ(ToString(StorageNodeStatusCode::kOk), "Ok");
  EXPECT_STREQ(ToString(StorageNodeStatusCode::kChecksumMismatch),
               "ChecksumMismatch");

  EXPECT_TRUE(IsRetriableStatus(StorageNodeStatusCode::kTimeout));
  EXPECT_TRUE(IsRetriableStatus(StorageNodeStatusCode::kNodeUnavailable));
  EXPECT_TRUE(IsRetriableStatus(StorageNodeStatusCode::kIoError));

  EXPECT_FALSE(IsRetriableStatus(StorageNodeStatusCode::kConflict));
  EXPECT_FALSE(IsRetriableStatus(StorageNodeStatusCode::kChecksumMismatch));
  EXPECT_FALSE(IsRetriableStatus(StorageNodeStatusCode::kDiskFull));
}

TEST(StoreTypesTest, ChunkStateCoversExpectedLifecycleSemantics) {
  EXPECT_STREQ(ToString(ChunkState::kStaging), "Staging");
  EXPECT_STREQ(ToString(ChunkState::kLive), "Live");
  EXPECT_STREQ(ToString(ChunkState::kMissing), "Missing");

  EXPECT_TRUE(IsReadableChunkState(ChunkState::kLive));
  EXPECT_FALSE(IsReadableChunkState(ChunkState::kStaging));
  EXPECT_FALSE(IsReadableChunkState(ChunkState::kCorrupted));

  EXPECT_TRUE(IsTerminalChunkState(ChunkState::kDeleted));
  EXPECT_TRUE(IsTerminalChunkState(ChunkState::kQuarantined));
  EXPECT_TRUE(IsTerminalChunkState(ChunkState::kMissing));
  EXPECT_FALSE(IsTerminalChunkState(ChunkState::kLive));
  EXPECT_FALSE(IsTerminalChunkState(ChunkState::kDeleting));
}

TEST(StoreTypesTest, ChunkChecksumDefaultsRemainUnsetUntilAlgorithmValueAndSizeExist) {
  ChunkChecksum checksum;
  EXPECT_EQ(checksum.algorithm, ChunkChecksumAlgorithm::kUnknown);
  EXPECT_TRUE(checksum.value.empty());
  EXPECT_EQ(checksum.size_bytes, 0U);
  EXPECT_EQ(checksum.computed_at, 0U);
  EXPECT_FALSE(checksum.IsSet());

  checksum.algorithm = ChunkChecksumAlgorithm::kSha256;
  checksum.value = "sha256:fixture";
  checksum.size_bytes = 64;
  checksum.computed_at = 1710000000;

  EXPECT_TRUE(checksum.IsSet());
}

TEST(StoreTypesTest, ChunkLocationAndIdentityValidateLightweightKeys) {
  ChunkLocation location;
  location.node_id = "node-a";
  location.chunk_id = "chunk-1";
  EXPECT_TRUE(location.IsValid());

  ChunkIdentity identity;
  EXPECT_FALSE(identity.HasChunkKey());
  identity.chunk_id = "chunk-1";
  identity.object_id = "object-7";
  identity.version = 3;
  identity.chunk_index = 1;
  identity.offset = 4096;

  EXPECT_TRUE(identity.HasChunkKey());
  EXPECT_EQ(identity.object_id, "object-7");
  EXPECT_EQ(identity.version, 3U);
  EXPECT_EQ(identity.chunk_index, 1U);
  EXPECT_EQ(identity.offset, 4096U);
}

TEST(StoreTypesTest, ChunkReplicaMetadataAndIndexEntryExposeClearDefaults) {
  ChunkReplica replica;
  EXPECT_TRUE(replica.chunk_id.empty());
  EXPECT_TRUE(replica.node_id.empty());
  EXPECT_EQ(replica.size, 0U);
  EXPECT_EQ(replica.state, ChunkState::kMissing);
  EXPECT_EQ(replica.last_error, StorageNodeStatusCode::kOk);
  EXPECT_FALSE(replica.IsReadable());

  replica.chunk_id = "chunk-1";
  replica.node_id = "node-a";
  replica.size = 128;
  replica.state = ChunkState::kLive;
  replica.checksum.algorithm = ChunkChecksumAlgorithm::kSha256;
  replica.checksum.value = "sha256:chunk-1";
  replica.checksum.size_bytes = 128;

  EXPECT_TRUE(replica.IsReadable());

  ChunkMetadata metadata;
  EXPECT_EQ(metadata.state, ChunkState::kMissing);
  EXPECT_EQ(metadata.last_error, StorageNodeStatusCode::kOk);
  EXPECT_FALSE(metadata.IsReadable());

  metadata.identity.chunk_id = "chunk-1";
  metadata.identity.object_id = "object-7";
  metadata.size = 128;
  metadata.state = ChunkState::kLive;
  metadata.checksum = replica.checksum;

  EXPECT_TRUE(metadata.IsReadable());

  ChunkIndexEntry entry;
  EXPECT_EQ(entry.state, ChunkState::kMissing);
  EXPECT_EQ(entry.size, 0U);
  EXPECT_EQ(entry.lock_shard, 0U);
  EXPECT_EQ(entry.updated_at, 0U);
  EXPECT_FALSE(entry.HasFinalPath());

  entry.identity = metadata.identity;
  entry.size = metadata.size;
  entry.state = ChunkState::kLive;
  entry.final_path = "chunks/chunk-1.bin";
  entry.lock_shard = 3;

  EXPECT_TRUE(entry.HasFinalPath());
  EXPECT_EQ(entry.final_path.generic_string(), "chunks/chunk-1.bin");
  EXPECT_EQ(entry.lock_shard, 3U);
}

TEST(StoreTypesTest, PlaceholderStageKeepsStableDefaultValue) {
  EXPECT_EQ(static_cast<int>(StoreModuleStage::kPlaceholder), 0);
}

}  // namespace
}  // namespace raftdemo
