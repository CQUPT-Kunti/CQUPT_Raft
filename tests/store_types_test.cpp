#include <gtest/gtest.h>

#include "store/common/store_types.h"

namespace raftdemo {
namespace {

TEST(StoreTypesTest, ChunkLocationIsValidWhenNodeAndChunkArePresent) {
  ChunkLocation location;
  location.node_id = "node-a";
  location.chunk_id = "chunk-1";

  EXPECT_TRUE(location.IsValid());
}

TEST(StoreTypesTest, ChunkLocationRejectsMissingNodeId) {
  ChunkLocation location;
  location.chunk_id = "chunk-1";

  EXPECT_FALSE(location.IsValid());
}

TEST(StoreTypesTest, ChunkLocationRejectsMissingChunkId) {
  ChunkLocation location;
  location.node_id = "node-a";

  EXPECT_FALSE(location.IsValid());
}

TEST(StoreTypesTest, PlaceholderStageKeepsStableDefaultValue) {
  EXPECT_EQ(static_cast<int>(StoreModuleStage::kPlaceholder), 0);
}

}  // namespace
}  // namespace raftdemo
