#include <gtest/gtest.h>

#include <string>

#include "raft/common/command.h"
#include "support/metadata_test_utils.h"

namespace raftdemo {
namespace {

std::string MakeSerializedMetadataPayload() {
  return SerializeMetadataCommand(
      test::MakeCreateBucketCommand("command-test-bucket",
                                    "command-test-create-bucket-1"));
}

TEST(CommandTest, MetadataCommandSerializeAndDeserialize) {
  const std::string payload = MakeSerializedMetadataPayload();

  Command cmd;
  cmd.type = CommandType::kMetadata;
  cmd.metadata_payload = payload;

  EXPECT_TRUE(cmd.IsValid());
  EXPECT_EQ(cmd.Serialize(), "META|" + std::to_string(payload.size()) + "|" + payload);

  Command parsed{};
  EXPECT_TRUE(Command::Deserialize(cmd.Serialize(), &parsed));
  EXPECT_EQ(parsed.type, CommandType::kMetadata);
  EXPECT_EQ(parsed.metadata_payload, payload);
}

TEST(CommandTest, MetadataCommandKeepsExplicitPayloadSizeFraming) {
  const std::string payload = "metadata|payload|with|separators";

  Command cmd;
  cmd.type = CommandType::kMetadata;
  cmd.metadata_payload = payload;

  EXPECT_TRUE(cmd.IsValid());
  EXPECT_TRUE(Command::Deserialize(cmd.Serialize(), &cmd));
  EXPECT_EQ(cmd.metadata_payload, payload);
}

TEST(CommandTest, EmptyMetadataPayloadIsInvalid) {
  Command cmd;
  cmd.type = CommandType::kMetadata;
  cmd.metadata_payload.clear();

  EXPECT_FALSE(cmd.IsValid());
}

TEST(CommandTest, UnknownCommandIsInvalid) {
  Command cmd;
  cmd.type = CommandType::kUnknown;
  cmd.metadata_payload = MakeSerializedMetadataPayload();

  EXPECT_FALSE(cmd.IsValid());
  EXPECT_TRUE(cmd.Serialize().empty());
}

TEST(CommandTest, DeserializeRejectsBadInput) {
  Command out{};

  EXPECT_FALSE(Command::Deserialize("", &out));
  EXPECT_FALSE(Command::Deserialize("META|only_size", &out));
  EXPECT_FALSE(Command::Deserialize("META|abc|payload", &out));
  EXPECT_FALSE(Command::Deserialize("META|3|xy", &out));
  EXPECT_FALSE(Command::Deserialize("UNKNOWN|x|1", &out));
  EXPECT_FALSE(Command::Deserialize("LEGACY|x|1", &out));
  EXPECT_FALSE(Command::Deserialize("META|1|x", nullptr));
}

}  // namespace
}  // namespace raftdemo
