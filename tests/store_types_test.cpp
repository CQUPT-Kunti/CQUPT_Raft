#include <gtest/gtest.h>

#include <limits>
#include <string>

#include "store/common/store_types.h"

namespace storedemo {
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

TEST(StoreTypesTest, ChunkChecksumHelperComputesStableSha256ForSamePayload) {
  const std::string payload = "chunk-payload-for-checksum";
  ChunkChecksum first;
  ChunkChecksum second;
  std::string error_detail;

  EXPECT_EQ(ComputeChunkChecksum(payload, &first, &error_detail),
            StorageNodeStatusCode::kOk);
  EXPECT_EQ(ComputeChunkChecksum(payload, &second, &error_detail),
            StorageNodeStatusCode::kOk);

  EXPECT_TRUE(error_detail.empty());
  EXPECT_EQ(first.algorithm, ChunkChecksumAlgorithm::kSha256);
  EXPECT_EQ(first.size_bytes, payload.size());
  EXPECT_EQ(first.computed_at, 0U);
  EXPECT_EQ(first.value.size(), kSha256DigestHexChars);
  EXPECT_TRUE(first.IsSet());

  EXPECT_EQ(first.algorithm, second.algorithm);
  EXPECT_EQ(first.size_bytes, second.size_bytes);
  EXPECT_EQ(first.value, second.value);
}

TEST(StoreTypesTest, ChunkChecksumHelperSupportsEmptyAndBinaryPayload) {
  ChunkChecksum empty_checksum;
  std::string error_detail;
  EXPECT_EQ(ComputeChunkChecksum("", &empty_checksum, &error_detail),
            StorageNodeStatusCode::kOk);
  EXPECT_TRUE(error_detail.empty());
  EXPECT_EQ(empty_checksum.algorithm, ChunkChecksumAlgorithm::kSha256);
  EXPECT_EQ(empty_checksum.size_bytes, 0U);
  EXPECT_EQ(
      empty_checksum.value,
      "e3b0c44298fc1c149afbf4c8996fb924"
      "27ae41e4649b934ca495991b7852b855");
  EXPECT_TRUE(empty_checksum.IsSet());

  const std::string binary_payload(
      {static_cast<char>(0x00),
       static_cast<char>(0x01),
       static_cast<char>(0x02),
       static_cast<char>(0x7f),
       static_cast<char>(0x80),
       static_cast<char>(0xff),
       'b',
       'i',
       'n',
       static_cast<char>(0x00),
       't',
       'a',
       'i',
       'l'});
  ChunkChecksum binary_checksum;
  EXPECT_EQ(ComputeChunkChecksum(binary_payload, &binary_checksum, &error_detail),
            StorageNodeStatusCode::kOk);
  EXPECT_EQ(binary_checksum.algorithm, ChunkChecksumAlgorithm::kSha256);
  EXPECT_EQ(binary_checksum.size_bytes, binary_payload.size());
  EXPECT_EQ(binary_checksum.value.size(), kSha256DigestHexChars);

  ChunkChecksum binary_checksum_again;
  EXPECT_EQ(ComputeChunkChecksum(binary_payload, &binary_checksum_again, &error_detail),
            StorageNodeStatusCode::kOk);
  EXPECT_EQ(binary_checksum.value, binary_checksum_again.value);
}

TEST(StoreTypesTest, ChunkChecksumHelperDetectsMismatchAndSupportsVerification) {
  const std::string payload = "payload-a";
  const std::string different_payload = "payload-b";
  ChunkChecksum expected_checksum;
  std::string error_detail;

  EXPECT_EQ(ComputeChunkChecksum(payload, &expected_checksum, &error_detail),
            StorageNodeStatusCode::kOk);

  ChunkChecksum actual_checksum;
  EXPECT_EQ(VerifyChunkChecksum(payload, expected_checksum, &actual_checksum, &error_detail),
            StorageNodeStatusCode::kOk);
  EXPECT_EQ(actual_checksum.value, expected_checksum.value);
  EXPECT_EQ(actual_checksum.size_bytes, expected_checksum.size_bytes);

  EXPECT_EQ(VerifyChunkChecksum(different_payload,
                                expected_checksum,
                                &actual_checksum,
                                &error_detail),
            StorageNodeStatusCode::kChecksumMismatch);
  EXPECT_NE(error_detail.find("mismatch"), std::string::npos);

  ChunkChecksum invalid_expected = expected_checksum;
  invalid_expected.algorithm = ChunkChecksumAlgorithm::kUnknown;
  EXPECT_EQ(VerifyChunkChecksum(payload, invalid_expected, nullptr, &error_detail),
            StorageNodeStatusCode::kInvalidArgument);

  invalid_expected = expected_checksum;
  invalid_expected.value = "short";
  EXPECT_EQ(VerifyChunkChecksum(payload, invalid_expected, nullptr, &error_detail),
            StorageNodeStatusCode::kInvalidArgument);
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

TEST(StoreTypesTest, ChunkIdHelpersBuildParseAndValidateCanonicalIds) {
  std::string chunk_id;
  std::string error_detail;
  EXPECT_EQ(MakeChunkId("object-7", 3, 0, &chunk_id, &error_detail),
            StorageNodeStatusCode::kOk);
  EXPECT_TRUE(error_detail.empty());
  EXPECT_EQ(chunk_id, "object-7~3~0");

  ChunkIdentity identity;
  EXPECT_EQ(ParseChunkId(chunk_id, &identity, &error_detail),
            StorageNodeStatusCode::kOk);
  EXPECT_EQ(identity.chunk_id, chunk_id);
  EXPECT_EQ(identity.object_id, "object-7");
  EXPECT_EQ(identity.version, 3U);
  EXPECT_EQ(identity.chunk_index, 0U);
  EXPECT_EQ(identity.offset, 0U);

  EXPECT_EQ(ValidateChunkId(chunk_id, &error_detail),
            StorageNodeStatusCode::kOk);
  EXPECT_TRUE(error_detail.empty());
}

TEST(StoreTypesTest, ChunkIdHelpersRejectUnsafeObjectIdsAndInvalidGenerationArgs) {
  std::string chunk_id;
  std::string error_detail;

  EXPECT_EQ(MakeChunkId("", 1, 0, &chunk_id, &error_detail),
            StorageNodeStatusCode::kInvalidArgument);
  EXPECT_NE(error_detail.find("object_id must not be empty"), std::string::npos);

  EXPECT_EQ(MakeChunkId("object/7", 1, 0, &chunk_id, &error_detail),
            StorageNodeStatusCode::kInvalidArgument);
  EXPECT_NE(error_detail.find("unsafe character"), std::string::npos);

  EXPECT_EQ(MakeChunkId("../object7", 1, 0, &chunk_id, &error_detail),
            StorageNodeStatusCode::kInvalidArgument);
  EXPECT_NE(error_detail.find("path escape"), std::string::npos);

  EXPECT_EQ(MakeChunkId(".hidden", 1, 0, &chunk_id, &error_detail),
            StorageNodeStatusCode::kInvalidArgument);
  EXPECT_NE(error_detail.find("start or end with '.'"), std::string::npos);

  EXPECT_EQ(MakeChunkId("object-7", 0, 0, &chunk_id, &error_detail),
            StorageNodeStatusCode::kInvalidArgument);
  EXPECT_NE(error_detail.find("version must be greater than zero"),
            std::string::npos);
}

TEST(StoreTypesTest, ChunkIdHelpersRejectInvalidOrNonCanonicalChunkIds) {
  std::string error_detail;

  EXPECT_EQ(ValidateChunkId("", &error_detail),
            StorageNodeStatusCode::kInvalidArgument);
  EXPECT_NE(error_detail.find("must not be empty"), std::string::npos);

  EXPECT_EQ(ValidateChunkId("object-7:3:0", &error_detail),
            StorageNodeStatusCode::kInvalidArgument);
  EXPECT_NE(error_detail.find("missing first separator"), std::string::npos);

  EXPECT_EQ(ValidateChunkId("object-7~0~1", &error_detail),
            StorageNodeStatusCode::kInvalidArgument);
  EXPECT_NE(error_detail.find("version must be greater than zero"),
            std::string::npos);

  EXPECT_EQ(ValidateChunkId("object-7~01~1", &error_detail),
            StorageNodeStatusCode::kInvalidArgument);
  EXPECT_NE(error_detail.find("canonical unsigned encoding"),
            std::string::npos);

  EXPECT_EQ(ValidateChunkId("object-7~1~01", &error_detail),
            StorageNodeStatusCode::kInvalidArgument);
  EXPECT_NE(error_detail.find("canonical unsigned encoding"),
            std::string::npos);

  EXPECT_EQ(ValidateChunkId("object-7~1~4294967296", &error_detail),
            StorageNodeStatusCode::kInvalidArgument);
  EXPECT_NE(error_detail.find("valid uint32"), std::string::npos);

  EXPECT_EQ(ValidateChunkId("object/7~1~0", &error_detail),
            StorageNodeStatusCode::kInvalidArgument);
  EXPECT_NE(error_detail.find("unsafe character"), std::string::npos);
}

TEST(StoreTypesTest, ChunkIdHelpersAcceptSafeBoundaryValues) {
  const std::string object_id(kMaxChunkObjectIdLength, 'a');
  std::string chunk_id;
  std::string error_detail;

  EXPECT_EQ(MakeChunkId(object_id,
                        std::numeric_limits<std::uint64_t>::max(),
                        std::numeric_limits<std::uint32_t>::max(),
                        &chunk_id,
                        &error_detail),
            StorageNodeStatusCode::kOk);
  EXPECT_LE(chunk_id.size(), kMaxChunkIdLength);

  ChunkIdentity identity;
  EXPECT_EQ(ParseChunkId(chunk_id, &identity, &error_detail),
            StorageNodeStatusCode::kOk);
  EXPECT_EQ(identity.object_id, object_id);
  EXPECT_EQ(identity.version, std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(identity.chunk_index, std::numeric_limits<std::uint32_t>::max());

  const std::string too_long_object_id(kMaxChunkObjectIdLength + 1U, 'b');
  EXPECT_EQ(MakeChunkId(too_long_object_id, 1, 0, &chunk_id, &error_detail),
            StorageNodeStatusCode::kInvalidArgument);
  EXPECT_NE(error_detail.find("safe length"), std::string::npos);
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
}  // namespace storedemo
