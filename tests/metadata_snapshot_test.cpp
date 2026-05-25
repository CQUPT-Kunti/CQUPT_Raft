#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "raft/common/metadata_command.h"
#include "raft/common/metadata_result.h"
#include "raft/state_machine/metadata_state_machine.h"

namespace raftdemo
{
    namespace
    {
        class TempSnapshotPath
        {
        public:
            TempSnapshotPath()
            {
                const auto now =
                    std::chrono::steady_clock::now().time_since_epoch().count();
                path_ = std::filesystem::temp_directory_path() /
                        ("metadata-snapshot-test-" + std::to_string(now) + ".bin");
            }

            ~TempSnapshotPath()
            {
                std::error_code ec;
                std::filesystem::remove(path_, ec);
            }

            const std::filesystem::path &path() const
            {
                return path_;
            }

        private:
            std::filesystem::path path_;
        };

        MetadataRecord MakeRecord(const std::string &object_key,
                                  const std::string &create_request_id)
        {
            MetadataRecord record;
            record.object_key = object_key;
            record.state = MetadataRecordState::kPending;
            record.object_size = 1024;
            record.chunk_size = 256;
            record.chunk_count = 4;
            record.checksum = "checksum-" + object_key;
            record.mock_locations = {"node-a", "node-b"};
            record.payload = "payload-" + object_key;
            record.create_request_id = create_request_id;
            return record;
        }

        MetadataCommand MakeCommitCommand(const std::string &object_key,
                                          const std::string &request_id,
                                          const std::string &commit_info)
        {
            MetadataCommand command;
            command.operation = MetadataOperation::kCommit;
            command.request_id = request_id;
            command.object_key = object_key;
            command.commit_info = commit_info;
            return command;
        }

        MetadataCommand MakeDeleteCommand(const std::string &object_key,
                                          const std::string &request_id,
                                          const std::string &delete_info)
        {
            MetadataCommand command;
            command.operation = MetadataOperation::kDelete;
            command.request_id = request_id;
            command.object_key = object_key;
            command.delete_info = delete_info;
            return command;
        }

        auto ApplyMetadataCommand(StrongConsistencyMetadataStateMachine *state_machine,
                                  const std::uint64_t index,
                                  const MetadataCommand &command)
        {
            return state_machine->Apply(index, SerializeMetadataCommand(command));
        }

        std::vector<std::string> ExtractObjectKeys(const MetadataListResponse &response)
        {
            std::vector<std::string> keys;
            keys.reserve(response.records.size());
            for (const auto &record : response.records)
            {
                keys.push_back(record.object_key);
            }
            return keys;
        }
    } // namespace

    TEST(MetadataSnapshotTest, SaveAndLoadRestoresCommittedAndKeepsPendingInvisible)
    {
        StrongConsistencyMetadataStateMachine state_machine;

        const MetadataRecord committed_record = MakeRecord("object/committed", "create-committed");
        const MetadataCommand committed_create = MakeCreateMetadataCommand(committed_record);
        const MetadataCommand committed_commit =
            MakeCommitCommand(committed_record.object_key, "commit-committed", "manifest-v1");

        const MetadataRecord pending_record = MakeRecord("object/pending", "create-pending");
        const MetadataCommand pending_create = MakeCreateMetadataCommand(pending_record);

        const auto [create_ok, create_message] =
            ApplyMetadataCommand(&state_machine, 1, committed_create);
        EXPECT_TRUE(create_ok) << create_message;
        const auto [commit_ok, commit_message] =
            ApplyMetadataCommand(&state_machine, 2, committed_commit);
        EXPECT_TRUE(commit_ok) << commit_message;
        const auto [pending_ok, pending_message] =
            ApplyMetadataCommand(&state_machine, 3, pending_create);
        EXPECT_TRUE(pending_ok) << pending_message;

        TempSnapshotPath snapshot;
        const auto [save_status, save_message] =
            state_machine.SaveSnapshot(snapshot.path().string());
        EXPECT_EQ(save_status, SnapshotStatus::kOk) << save_message;

        StrongConsistencyMetadataStateMachine restored;
        const auto [load_status, load_message] =
            restored.LoadSnapshot(snapshot.path().string());
        EXPECT_EQ(load_status, SnapshotStatus::kOk) << load_message;

        const MetadataHeadResponse committed_head =
            restored.HeadMetadataRecord({.object_key = committed_record.object_key});
        ASSERT_EQ(committed_head.result.code, MetadataStatusCode::kOk);
        ASSERT_TRUE(committed_head.record.has_value());
        EXPECT_EQ(committed_head.record->object_key, committed_record.object_key);
        EXPECT_EQ(committed_head.record->state, MetadataRecordState::kCommitted);

        const MetadataHeadResponse pending_head =
            restored.HeadMetadataRecord({.object_key = pending_record.object_key});
        EXPECT_EQ(pending_head.result.code, MetadataStatusCode::kNotFound);
        EXPECT_FALSE(pending_head.record.has_value());

        const MetadataListResponse list_response =
            restored.ListMetadataRecords({.prefix = "object/", .limit = std::nullopt, .page_token = ""});
        ASSERT_EQ(list_response.result.code, MetadataStatusCode::kOk);
        EXPECT_EQ(ExtractObjectKeys(list_response),
                  std::vector<std::string>({committed_record.object_key}));
    }

    TEST(MetadataSnapshotTest, SaveAndLoadPreservesTombstoneAndReplaySemantics)
    {
        StrongConsistencyMetadataStateMachine state_machine;

        const MetadataRecord record = MakeRecord("object/deleted", "create-deleted");
        const MetadataCommand create_command = MakeCreateMetadataCommand(record);
        const MetadataCommand commit_command =
            MakeCommitCommand(record.object_key, "commit-deleted", "commit-info");
        const MetadataCommand delete_command =
            MakeDeleteCommand(record.object_key, "delete-deleted", "delete-info");

        const auto [create_ok, create_message] =
            ApplyMetadataCommand(&state_machine, 1, create_command);
        EXPECT_TRUE(create_ok) << create_message;
        const auto [commit_ok, commit_message] =
            ApplyMetadataCommand(&state_machine, 2, commit_command);
        EXPECT_TRUE(commit_ok) << commit_message;
        const auto [delete_ok, delete_message] =
            ApplyMetadataCommand(&state_machine, 3, delete_command);
        EXPECT_TRUE(delete_ok) << delete_message;

        TempSnapshotPath snapshot;
        const auto [save_status, save_message] =
            state_machine.SaveSnapshot(snapshot.path().string());
        EXPECT_EQ(save_status, SnapshotStatus::kOk) << save_message;

        StrongConsistencyMetadataStateMachine restored;
        const auto [load_status, load_message] =
            restored.LoadSnapshot(snapshot.path().string());
        EXPECT_EQ(load_status, SnapshotStatus::kOk) << load_message;

        const MetadataHeadResponse head_response =
            restored.HeadMetadataRecord({.object_key = record.object_key});
        EXPECT_EQ(head_response.result.code, MetadataStatusCode::kNotFound);

        const MetadataListResponse list_response =
            restored.ListMetadataRecords({.prefix = "object/", .limit = std::nullopt, .page_token = ""});
        EXPECT_TRUE(ExtractObjectKeys(list_response).empty());

        const auto [delete_replay_ok, delete_replay_message] =
            ApplyMetadataCommand(&restored, 10, delete_command);
        EXPECT_TRUE(delete_replay_ok);
        EXPECT_EQ(delete_replay_message, "idempotent replay");

        const auto [create_replay_ok, create_replay_message] =
            ApplyMetadataCommand(&restored, 11, create_command);
        EXPECT_TRUE(create_replay_ok);
        EXPECT_EQ(create_replay_message, "idempotent replay");

        const auto [commit_replay_ok, commit_replay_message] =
            ApplyMetadataCommand(&restored, 12, commit_command);
        EXPECT_TRUE(commit_replay_ok);
        EXPECT_EQ(commit_replay_message, "idempotent replay");

        const MetadataCommand fresh_create =
            MakeCreateMetadataCommand(MakeRecord(record.object_key, "create-new"));
        const auto [fresh_create_ok, fresh_create_message] =
            ApplyMetadataCommand(&restored, 13, fresh_create);
        EXPECT_FALSE(fresh_create_ok);
        EXPECT_EQ(fresh_create_message, "state conflict: object is tombstoned");

        const MetadataHeadResponse post_replay_head =
            restored.HeadMetadataRecord({.object_key = record.object_key});
        EXPECT_EQ(post_replay_head.result.code, MetadataStatusCode::kNotFound);
    }

    TEST(MetadataSnapshotTest, LoadSnapshotRejectsCorruptedSnapshot)
    {
        TempSnapshotPath snapshot;
        {
            std::ofstream out(snapshot.path(), std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(out.is_open());
            out.write("broken", 6);
            out.flush();
            ASSERT_TRUE(static_cast<bool>(out));
        }

        StrongConsistencyMetadataStateMachine restored;
        const auto [status, message] = restored.LoadSnapshot(snapshot.path().string());
        EXPECT_EQ(status, SnapshotStatus::kCorruptedData);
        EXPECT_EQ(message, "failed to read metadata snapshot header");
    }

    TEST(MetadataSnapshotTest, LoadSnapshotRejectsMagicMismatch)
    {
        StrongConsistencyMetadataStateMachine state_machine;
        const MetadataRecord record = MakeRecord("object/magic", "create-magic");
        const MetadataCommand create_command = MakeCreateMetadataCommand(record);
        const MetadataCommand commit_command =
            MakeCommitCommand(record.object_key, "commit-magic", "commit-info");

        const auto [create_ok, create_message] =
            ApplyMetadataCommand(&state_machine, 1, create_command);
        EXPECT_TRUE(create_ok) << create_message;
        const auto [commit_ok, commit_message] =
            ApplyMetadataCommand(&state_machine, 2, commit_command);
        EXPECT_TRUE(commit_ok) << commit_message;

        TempSnapshotPath snapshot;
        const auto [save_status, save_message] =
            state_machine.SaveSnapshot(snapshot.path().string());
        EXPECT_EQ(save_status, SnapshotStatus::kOk) << save_message;

        {
            std::fstream io(snapshot.path(), std::ios::binary | std::ios::in | std::ios::out);
            ASSERT_TRUE(io.is_open());
            const std::uint32_t invalid_magic = 0x42414421U;
            io.write(reinterpret_cast<const char *>(&invalid_magic), sizeof(invalid_magic));
            io.flush();
            ASSERT_TRUE(static_cast<bool>(io));
        }

        StrongConsistencyMetadataStateMachine restored;
        const auto [status, message] = restored.LoadSnapshot(snapshot.path().string());
        EXPECT_EQ(status, SnapshotStatus::kCorruptedData);
        EXPECT_EQ(message, "invalid metadata snapshot magic");
    }

    TEST(MetadataSnapshotTest, LoadSnapshotRejectsVersionMismatch)
    {
        StrongConsistencyMetadataStateMachine state_machine;
        const MetadataRecord record = MakeRecord("object/version", "create-version");
        const MetadataCommand create_command = MakeCreateMetadataCommand(record);
        const MetadataCommand commit_command =
            MakeCommitCommand(record.object_key, "commit-version", "commit-info");

        const auto [create_ok, create_message] =
            ApplyMetadataCommand(&state_machine, 1, create_command);
        EXPECT_TRUE(create_ok) << create_message;
        const auto [commit_ok, commit_message] =
            ApplyMetadataCommand(&state_machine, 2, commit_command);
        EXPECT_TRUE(commit_ok) << commit_message;

        TempSnapshotPath snapshot;
        const auto [save_status, save_message] =
            state_machine.SaveSnapshot(snapshot.path().string());
        EXPECT_EQ(save_status, SnapshotStatus::kOk) << save_message;

        {
            std::fstream io(snapshot.path(), std::ios::binary | std::ios::in | std::ios::out);
            ASSERT_TRUE(io.is_open());
            io.seekp(sizeof(std::uint32_t));
            const std::uint32_t unsupported_version = 999U;
            io.write(reinterpret_cast<const char *>(&unsupported_version),
                     sizeof(unsupported_version));
            io.flush();
            ASSERT_TRUE(static_cast<bool>(io));
        }

        StrongConsistencyMetadataStateMachine restored;
        const auto [status, message] = restored.LoadSnapshot(snapshot.path().string());
        EXPECT_EQ(status, SnapshotStatus::kVersionMismatch);
        EXPECT_EQ(message, "unsupported metadata snapshot version");
    }

} // namespace raftdemo
