#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "raft/common/config.h"
#include "raft/common/propose.h"
#include "raft/node/raft_node.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "raft/storage/snapshot_storage.h"
#include "support/metadata_test_utils.h"

namespace raftdemo::test
{
    using Clock = std::chrono::steady_clock;
    inline constexpr const char *kSnapshotStorageFailpointEnv =
        "RAFT_TEST_SNAPSHOT_STORAGE_FAILPOINT";
    inline constexpr const char *kSyntheticMetadataBucket =
        "__raft_test_metadata_bridge_bucket__";

    inline std::string ProposeStatusName(const ProposeStatus status)
    {
        switch (status)
        {
        case ProposeStatus::kOk:
            return "Ok";
        case ProposeStatus::kNotLeader:
            return "NotLeader";
        case ProposeStatus::kInvalidCommand:
            return "InvalidCommand";
        case ProposeStatus::kNodeStopping:
            return "NodeStopping";
        case ProposeStatus::kReplicationFailed:
            return "ReplicationFailed";
        case ProposeStatus::kCommitFailed:
            return "CommitFailed";
        case ProposeStatus::kApplyFailed:
            return "ApplyFailed";
        case ProposeStatus::kTimeout:
            return "Timeout";
        }
        return "Unknown";
    }

    inline bool Contains(const std::string &text, const std::string &needle)
    {
        return text.find(needle) != std::string::npos;
    }

    inline bool IsLeaderNode(const std::shared_ptr<RaftNode> &node)
    {
        return node && Contains(node->Describe(), "role=Leader");
    }

    inline std::string DescribeCluster(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                                       const std::vector<std::size_t> &excluded = {})
    {
        std::ostringstream oss;
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            bool excluded_node = false;
            for (const std::size_t excluded_index : excluded)
            {
                if (excluded_index == i)
                {
                    excluded_node = true;
                    break;
                }
            }
            if (i != 0)
            {
                oss << " | ";
            }
            oss << "node[" << i << "]";
            if (excluded_node)
            {
                oss << "(excluded)";
            }
            oss << "=";
            if (!nodes[i])
            {
                oss << "null";
            }
            else
            {
                oss << nodes[i]->Describe();
            }
        }
        return oss.str();
    }

    struct SyntheticMetadataMutation
    {
        enum class Operation
        {
            kWriteObject,
            kDeleteObject,
        };

        Operation operation{Operation::kWriteObject};
        std::string key;
        std::string value;
    };

    inline SyntheticMetadataMutation WriteSyntheticObject(const std::string &key,
                                                          const std::string &value)
    {
        return SyntheticMetadataMutation{
            .operation = SyntheticMetadataMutation::Operation::kWriteObject,
            .key = key,
            .value = value,
        };
    }

    inline SyntheticMetadataMutation DeleteSyntheticObject(const std::string &key)
    {
        return SyntheticMetadataMutation{
            .operation = SyntheticMetadataMutation::Operation::kDeleteObject,
            .key = key,
            .value = "",
        };
    }

    inline std::string HexEncode(std::string_view value)
    {
        static constexpr char kHexDigits[] = "0123456789abcdef";
        std::string encoded;
        encoded.reserve(value.size() * 2);
        for (const unsigned char ch : value)
        {
            encoded.push_back(kHexDigits[(ch >> 4U) & 0x0FU]);
            encoded.push_back(kHexDigits[ch & 0x0FU]);
        }
        return encoded;
    }

    inline bool HexDecode(std::string_view encoded, std::string *value)
    {
        if (value == nullptr || encoded.size() % 2 != 0)
        {
            return false;
        }

        auto decode_nibble = [](const char ch) -> int
        {
            if (ch >= '0' && ch <= '9')
            {
                return ch - '0';
            }
            if (ch >= 'a' && ch <= 'f')
            {
                return 10 + ch - 'a';
            }
            if (ch >= 'A' && ch <= 'F')
            {
                return 10 + ch - 'A';
            }
            return -1;
        };

        std::string decoded;
        decoded.reserve(encoded.size() / 2);
        for (std::size_t index = 0; index < encoded.size(); index += 2)
        {
            const int high = decode_nibble(encoded[index]);
            const int low = decode_nibble(encoded[index + 1]);
            if (high < 0 || low < 0)
            {
                return false;
            }
            decoded.push_back(static_cast<char>((high << 4U) | low));
        }

        *value = std::move(decoded);
        return true;
    }

    inline std::string SyntheticObjectIdForValue(const std::string &key,
                                                 const std::string &value)
    {
        return "kvbridge:" + HexEncode(key) + ":" + HexEncode(value);
    }

    inline bool DecodeSyntheticObjectId(const std::string &object_id,
                                        std::string *key,
                                        std::string *value)
    {
        constexpr std::string_view kPrefix = "kvbridge:";
        if (!object_id.starts_with(kPrefix))
        {
            return false;
        }

        const std::size_t separator =
            object_id.find(':', static_cast<std::size_t>(kPrefix.size()));
        if (separator == std::string::npos)
        {
            return false;
        }

        return HexDecode(std::string_view(object_id).substr(kPrefix.size(),
                                                            separator - kPrefix.size()),
                         key) &&
               HexDecode(std::string_view(object_id).substr(separator + 1U), value);
    }

    inline std::string SyntheticRequestId(const std::string &phase,
                                          const std::string &key,
                                          const std::string &value = "")
    {
        return "kvbridge:" + phase + ":" + HexEncode(key) + ":" + HexEncode(value);
    }

    inline const MetadataStateMachine *GetMetadataStateMachine(
        const std::shared_ptr<RaftNode> &node)
    {
        return node == nullptr ? nullptr : node->GetMetadataStateMachineV2();
    }

    inline ProposeResult ProposeMetadataBridgeCommand(const std::shared_ptr<RaftNode> &leader,
                                                      const MetadataCommand &command)
    {
        return leader->ProposeMetadata(raftdemo::SerializeMetadataCommand(command));
    }

    inline bool EnsureSyntheticBucket(const std::shared_ptr<RaftNode> &leader,
                                      ProposeResult *last_result)
    {
        const MetadataStateMachine *state_machine = GetMetadataStateMachine(leader);
        if (state_machine == nullptr)
        {
            if (last_result != nullptr)
            {
                last_result->status = ProposeStatus::kApplyFailed;
                last_result->message = "metadata state machine unavailable";
            }
            return false;
        }

        const auto bucket = state_machine->FindBucket(kSyntheticMetadataBucket);
        if (bucket.has_value() && bucket->IsActive())
        {
            return true;
        }

        const ProposeResult result = ProposeMetadataBridgeCommand(
            leader,
            MakeCreateBucketCommand(kSyntheticMetadataBucket,
                                    SyntheticRequestId("bucket", kSyntheticMetadataBucket)));
        if (last_result != nullptr)
        {
            *last_result = result;
        }
        return result.Ok();
    }

    inline bool SyntheticStateMatchesValue(const MetadataStateMachine &state_machine,
                                           const std::string &key,
                                           const std::string &expected_value)
    {
        const auto response = state_machine.HeadObject(
            {.bucket = kSyntheticMetadataBucket, .object_key = key});
        if (!response.result.Ok() || !response.record.has_value() ||
            !response.record->IsCommitted())
        {
            return false;
        }

        const auto indexed_object_id =
            state_machine.FindIndexedObjectId(kSyntheticMetadataBucket, key);
        const auto chunks = state_machine.FindChunkRefs(kSyntheticMetadataBucket, key);
        if (!indexed_object_id.has_value() || *indexed_object_id != response.record->object_id ||
            !chunks.has_value() || chunks->empty())
        {
            return false;
        }

        std::string decoded_key;
        std::string decoded_value;
        return DecodeSyntheticObjectId(response.record->object_id,
                                       &decoded_key,
                                       &decoded_value) &&
               decoded_key == key &&
               decoded_value == expected_value;
    }

    inline bool SyntheticStateMatchesMissing(const MetadataStateMachine &state_machine,
                                             const std::string &key)
    {
        const auto response = state_machine.HeadObject(
            {.bucket = kSyntheticMetadataBucket, .object_key = key});
        if (response.result.code != MetadataStatusCode::kNotFound ||
            response.record.has_value())
        {
            return false;
        }

        if (state_machine.FindIndexedObjectId(kSyntheticMetadataBucket, key).has_value() ||
            state_machine.FindChunkRefs(kSyntheticMetadataBucket, key).has_value())
        {
            return false;
        }

        const auto object = state_machine.FindObject(kSyntheticMetadataBucket, key);
        return !object.has_value() || object->IsDeleted();
    }

    inline bool ApplySyntheticMetadataMutation(const std::shared_ptr<RaftNode> &leader,
                                               const SyntheticMetadataMutation &command,
                                               ProposeResult *last_result)
    {
        if (!EnsureSyntheticBucket(leader, last_result))
        {
            return false;
        }

        const MetadataStateMachine *state_machine = GetMetadataStateMachine(leader);
        if (state_machine == nullptr)
        {
            if (last_result != nullptr)
            {
                last_result->status = ProposeStatus::kApplyFailed;
                last_result->message = "metadata state machine unavailable";
            }
            return false;
        }

        const std::string desired_object_id =
            SyntheticObjectIdForValue(command.key, command.value);
        const auto indexed_object_id =
            state_machine->FindIndexedObjectId(kSyntheticMetadataBucket, command.key);
        const auto current_object =
            state_machine->FindObject(kSyntheticMetadataBucket, command.key);

        if (command.operation == SyntheticMetadataMutation::Operation::kDeleteObject)
        {
            if (!indexed_object_id.has_value())
            {
                if (last_result != nullptr)
                {
                    last_result->status = ProposeStatus::kOk;
                    last_result->term = state_machine->LastAppliedTerm();
                    last_result->log_index = state_machine->LastAppliedIndex();
                    last_result->message = "already missing";
                }
                return true;
            }

            const ProposeResult result = ProposeMetadataBridgeCommand(
                leader,
                MakeDeleteObjectCommand(kSyntheticMetadataBucket,
                                        command.key,
                                        *indexed_object_id,
                                        SyntheticRequestId("delete",
                                                           command.key,
                                                           *indexed_object_id)));
            if (last_result != nullptr)
            {
                *last_result = result;
            }
            return result.Ok();
        }

        if (SyntheticStateMatchesValue(*state_machine, command.key, command.value))
        {
            if (last_result != nullptr)
            {
                last_result->status = ProposeStatus::kOk;
                last_result->term = state_machine->LastAppliedTerm();
                last_result->log_index = state_machine->LastAppliedIndex();
                last_result->message = "already committed";
            }
            return true;
        }

        if (indexed_object_id.has_value() && *indexed_object_id != desired_object_id)
        {
            const ProposeResult delete_result = ProposeMetadataBridgeCommand(
                leader,
                MakeDeleteObjectCommand(kSyntheticMetadataBucket,
                                        command.key,
                                        *indexed_object_id,
                                        SyntheticRequestId("replace-delete",
                                                           command.key,
                                                           *indexed_object_id)));
            if (last_result != nullptr)
            {
                *last_result = delete_result;
            }
            if (!delete_result.Ok())
            {
                return false;
            }
        }

        state_machine = GetMetadataStateMachine(leader);
        if (state_machine == nullptr)
        {
            if (last_result != nullptr)
            {
                last_result->status = ProposeStatus::kApplyFailed;
                last_result->message = "metadata state machine unavailable after delete";
            }
            return false;
        }

        const auto refreshed_object =
            state_machine->FindObject(kSyntheticMetadataBucket, command.key);
        const bool needs_create =
            !refreshed_object.has_value() || refreshed_object->object_id != desired_object_id;
        if (needs_create)
        {
            const ProposeResult create_result = ProposeMetadataBridgeCommand(
                leader,
                MakeCreateObjectCommand(kSyntheticMetadataBucket,
                                        command.key,
                                        desired_object_id,
                                        SyntheticRequestId("create",
                                                           command.key,
                                                           command.value)));
            if (last_result != nullptr)
            {
                *last_result = create_result;
            }
            if (!create_result.Ok())
            {
                return false;
            }
        }

        state_machine = GetMetadataStateMachine(leader);
        if (state_machine != nullptr &&
            SyntheticStateMatchesValue(*state_machine, command.key, command.value))
        {
            if (last_result != nullptr)
            {
                last_result->status = ProposeStatus::kOk;
                last_result->term = state_machine->LastAppliedTerm();
                last_result->log_index = state_machine->LastAppliedIndex();
                last_result->message = "committed";
            }
            return true;
        }

        const ProposeResult commit_result = ProposeMetadataBridgeCommand(
            leader,
            MakeCommitObjectCommand(kSyntheticMetadataBucket,
                                    command.key,
                                    desired_object_id,
                                    SyntheticRequestId("commit",
                                                       command.key,
                                                       command.value)));
        if (last_result != nullptr)
        {
            *last_result = commit_result;
        }
        return commit_result.Ok();
    }

    inline std::uint64_t NowForPath()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    inline int PickBasePort(const std::string &test_name)
    {
        const int name_offset = static_cast<int>(std::hash<std::string>{}(test_name) % 1800);

        if (const char *env = std::getenv("RAFT_TEST_BASE_PORT"))
        {
            try
            {
                return std::stoi(env) + name_offset;
            }
            catch (...)
            {
            }
        }

        return 36000 + name_offset * 12;
    }

    inline std::filesystem::path MakeRestartTestRoot(const std::string &test_name)
    {
        std::random_device rd;
        std::string safe_name = test_name;
        for (char &ch : safe_name)
        {
            if (ch == '/' || ch == '\\' || ch == ':' || ch == ' ')
            {
                ch = '_';
            }
        }

#ifdef _WIN32
        const std::string name = "sr_" + std::to_string(NowForPath()) + "_" +
                                 std::to_string(rd());
        return std::filesystem::temp_directory_path() / "rq_sr" / name;
#else
        const std::string name = "raft_snapshot_restart_" + safe_name + "_" +
                                 std::to_string(NowForPath()) + "_" +
                                 std::to_string(rd());
        return std::filesystem::temp_directory_path() / name;
#endif
    }

    inline void WriteTextFile(const std::filesystem::path &path, const std::string &content)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open()) << path.string();
        out << content;
        out.flush();
        ASSERT_TRUE(static_cast<bool>(out)) << path.string();
    }

    inline void CopyFile(const std::filesystem::path &from, const std::filesystem::path &to)
    {
        std::error_code ec;
        std::filesystem::create_directories(to.parent_path(), ec);
        ASSERT_FALSE(ec) << ec.message();
        std::filesystem::copy_file(
            from, to, std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "copy snapshot input failed: from=" << from.string()
                         << ", to=" << to.string()
                         << ", error=" << ec.message();
    }

    inline void CopyDirectoryRecursively(const std::filesystem::path &from,
                                         const std::filesystem::path &to)
    {
        std::error_code ec;
        std::filesystem::create_directories(to.parent_path(), ec);
        ASSERT_FALSE(ec) << ec.message();
        std::filesystem::copy(from,
                              to,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing,
                              ec);
        ASSERT_FALSE(ec) << "copy snapshot directory failed: from=" << from.string()
                         << ", to=" << to.string()
                         << ", error=" << ec.message();
    }

    inline std::string JoinIssueReasons(const std::vector<SnapshotValidationIssue> &issues)
    {
        std::ostringstream oss;
        for (const auto &issue : issues)
        {
            oss << issue.path << ": " << issue.reason << "\n";
        }
        return oss.str();
    }

    inline std::string FormatSnapshotIndex(const std::uint64_t index)
    {
        std::ostringstream oss;
        oss << std::setw(20) << std::setfill('0') << index;
        return oss.str();
    }

    inline const char *ExpectedLinuxSpecificMarker()
    {
#if defined(__linux__)
        return "linux_specific=true";
#else
        return "linux_specific=false";
#endif
    }

    inline void SetEnvVar(const char *name, const std::string &value)
    {
#if defined(_WIN32)
        ASSERT_EQ(_putenv_s(name, value.c_str()), 0) << name;
#else
        ASSERT_EQ(::setenv(name, value.c_str(), 1), 0) << name;
#endif
    }

    inline void UnsetEnvVar(const char *name)
    {
#if defined(_WIN32)
        ASSERT_EQ(_putenv_s(name, ""), 0) << name;
#else
        ASSERT_EQ(::unsetenv(name), 0) << name;
#endif
    }

    class ScopedEnvVar
    {
    public:
        ScopedEnvVar(const char *name, std::string value)
            : name_(name)
        {
            const char *current = std::getenv(name_);
            if (current != nullptr)
            {
                had_original_ = true;
                original_value_ = current;
            }
            SetEnvVar(name_, value);
        }

        ~ScopedEnvVar()
        {
            if (had_original_)
            {
                SetEnvVar(name_, original_value_);
            }
            else
            {
                UnsetEnvVar(name_);
            }
        }

    private:
        const char *name_;
        bool had_original_{false};
        std::string original_value_;
    };

    inline std::vector<std::filesystem::path> ListSnapshotDirs(
        const std::filesystem::path &snapshot_root)
    {
        std::vector<std::filesystem::path> dirs;
        std::error_code ec;
        if (!std::filesystem::exists(snapshot_root, ec))
        {
            return dirs;
        }

        for (const auto &entry : std::filesystem::directory_iterator(snapshot_root, ec))
        {
            if (ec)
            {
                break;
            }
            if (!entry.is_directory())
            {
                continue;
            }
            const std::string name = entry.path().filename().string();
            if (name.rfind("snapshot_", 0) == 0)
            {
                dirs.push_back(entry.path());
            }
        }

        std::sort(dirs.begin(), dirs.end());
        return dirs;
    }

    inline std::optional<std::uint64_t> SnapshotIndexFromDir(
        const std::filesystem::path &snapshot_dir)
    {
        const std::string name = snapshot_dir.filename().string();
        constexpr std::size_t kPrefixSize = 9;
        if (name.size() <= kPrefixSize || name.rfind("snapshot_", 0) != 0)
        {
            return std::nullopt;
        }
        try
        {
            return static_cast<std::uint64_t>(std::stoull(name.substr(kPrefixSize)));
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    inline std::optional<std::uint64_t> ExtractUintField(const std::string &describe,
                                                         const std::string &field_name)
    {
        const std::string prefix = field_name + "=";
        const std::size_t begin = describe.find(prefix);
        if (begin == std::string::npos)
        {
            return std::nullopt;
        }

        std::size_t pos = begin + prefix.size();
        std::size_t end = pos;
        while (end < describe.size() && describe[end] >= '0' && describe[end] <= '9')
        {
            ++end;
        }

        if (end == pos)
        {
            return std::nullopt;
        }

        try
        {
            return static_cast<std::uint64_t>(std::stoull(describe.substr(pos, end - pos)));
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    inline std::vector<NodeConfig> BuildThreeNodeConfigs(const std::filesystem::path &data_root,
                                                         const int base_port)
    {
        NodeConfig n1;
        n1.node_id = 1;
        n1.address = "127.0.0.1:" + std::to_string(base_port + 1);
        n1.peers = {
            PeerConfig{2, "127.0.0.1:" + std::to_string(base_port + 2)},
            PeerConfig{3, "127.0.0.1:" + std::to_string(base_port + 3)},
        };
        n1.election_timeout_min = std::chrono::milliseconds(250);
        n1.election_timeout_max = std::chrono::milliseconds(500);
        n1.heartbeat_interval = std::chrono::milliseconds(80);
        n1.rpc_deadline = std::chrono::milliseconds(300);
        n1.data_dir = (data_root / "node_1").string();

        NodeConfig n2 = n1;
        n2.node_id = 2;
        n2.address = "127.0.0.1:" + std::to_string(base_port + 2);
        n2.peers = {
            PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
            PeerConfig{3, "127.0.0.1:" + std::to_string(base_port + 3)},
        };
        n2.data_dir = (data_root / "node_2").string();

        NodeConfig n3 = n1;
        n3.node_id = 3;
        n3.address = "127.0.0.1:" + std::to_string(base_port + 3);
        n3.peers = {
            PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
            PeerConfig{2, "127.0.0.1:" + std::to_string(base_port + 2)},
        };
        n3.data_dir = (data_root / "node_3").string();

        return {n1, n2, n3};
    }

    inline std::vector<snapshotConfig> BuildThreeSnapshotConfigs(
        const std::filesystem::path &snapshot_root,
        const bool enabled,
        const std::uint64_t log_threshold)
    {
        snapshotConfig s1;
        s1.enabled = enabled;
        s1.snapshot_dir = (snapshot_root / "node_1").string();
        s1.log_threshold = log_threshold;
        s1.snapshot_interval = std::chrono::minutes(10);
        s1.max_snapshot_count = 3;
        s1.load_on_startup = true;
        s1.file_prefix = "snapshot";

        snapshotConfig s2 = s1;
        s2.snapshot_dir = (snapshot_root / "node_2").string();

        snapshotConfig s3 = s1;
        s3.snapshot_dir = (snapshot_root / "node_3").string();

        return {s1, s2, s3};
    }

    class TestCluster
    {
    public:
        TestCluster(std::vector<NodeConfig> configs,
                    std::vector<snapshotConfig> snapshot_configs)
            : configs_(std::move(configs)), snapshot_configs_(std::move(snapshot_configs))
        {
        }

        ~TestCluster() { StopAll(); }

        void Start()
        {
            StopAll();
            nodes_.clear();
            wait_threads_.clear();

            for (std::size_t i = 0; i < configs_.size(); ++i)
            {
                nodes_.push_back(std::make_shared<RaftNode>(configs_[i], snapshot_configs_[i]));
            }
            wait_threads_.resize(nodes_.size());

            for (const auto &node : nodes_)
            {
                node->Start();
            }
            for (std::size_t i = 0; i < nodes_.size(); ++i)
            {
                const auto node = nodes_[i];
                wait_threads_[i] = std::thread([node]() { node->Wait(); });
            }
        }

        void StopAll()
        {
            for (const auto &node : nodes_)
            {
                if (node)
                {
                    node->Stop();
                }
            }
            for (auto &thread : wait_threads_)
            {
                if (thread.joinable())
                {
                    thread.join();
                }
            }
            wait_threads_.clear();
        }

        void StopNode(const std::size_t index)
        {
            if (index >= nodes_.size() || !nodes_[index])
            {
                return;
            }
            nodes_[index]->Stop();
            if (index < wait_threads_.size() && wait_threads_[index].joinable())
            {
                wait_threads_[index].join();
            }
        }

        void RestartNode(const std::size_t index)
        {
            ASSERT_LT(index, configs_.size());
            StopNode(index);

            if (nodes_.size() < configs_.size())
            {
                nodes_.resize(configs_.size());
            }
            if (wait_threads_.size() < configs_.size())
            {
                wait_threads_.resize(configs_.size());
            }

            nodes_[index] = std::make_shared<RaftNode>(configs_[index], snapshot_configs_[index]);
            nodes_[index]->Start();
            const auto node = nodes_[index];
            wait_threads_[index] = std::thread([node]() { node->Wait(); });
        }

        const std::vector<std::shared_ptr<RaftNode>> &Nodes() const { return nodes_; }

    private:
        std::vector<NodeConfig> configs_;
        std::vector<snapshotConfig> snapshot_configs_;
        std::vector<std::shared_ptr<RaftNode>> nodes_;
        std::vector<std::thread> wait_threads_;
    };

    inline bool IsExcluded(const std::size_t index, const std::vector<std::size_t> &excluded)
    {
        for (const std::size_t excluded_index : excluded)
        {
            if (index == excluded_index)
            {
                return true;
            }
        }
        return false;
    }

    inline std::shared_ptr<RaftNode> WaitForSingleLeader(
        const std::vector<std::shared_ptr<RaftNode>> &nodes,
        const std::chrono::milliseconds timeout,
        const std::vector<std::size_t> &excluded = {})
    {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline)
        {
            std::shared_ptr<RaftNode> leader;
            int leader_count = 0;

            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                if (IsExcluded(i, excluded) || !nodes[i])
                {
                    continue;
                }
                if (IsLeaderNode(nodes[i]))
                {
                    leader = nodes[i];
                    ++leader_count;
                }
            }

            if (leader_count == 1)
            {
                return leader;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return nullptr;
    }

    inline std::size_t FindNodeIndex(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                                     const std::shared_ptr<RaftNode> &target)
    {
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            if (nodes[i] == target)
            {
                return i;
            }
        }
        return nodes.size();
    }

    inline std::size_t PickFollowerIndex(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                                         const std::shared_ptr<RaftNode> &leader)
    {
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            if (nodes[i] && nodes[i] != leader)
            {
                return i;
            }
        }
        return nodes.size();
    }

    struct StableLeaderObservation
    {
        std::shared_ptr<RaftNode> leader;
        std::size_t leader_index{0};
        int stable_observations{0};
        std::string diagnostics;
    };

    inline std::optional<StableLeaderObservation> WaitForStableLeader(
        const std::vector<std::shared_ptr<RaftNode>> &nodes,
        const std::chrono::milliseconds timeout,
        const std::vector<std::size_t> &excluded = {},
        const int required_observations = 3)
    {
        const auto deadline = Clock::now() + timeout;
        std::size_t last_leader_index = nodes.size();
        int stable_observations = 0;
        int last_leader_count = 0;
        std::string last_cluster_state;

        while (Clock::now() < deadline)
        {
            std::shared_ptr<RaftNode> leader;
            std::size_t leader_index = nodes.size();
            int leader_count = 0;

            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                if (IsExcluded(i, excluded) || !nodes[i])
                {
                    continue;
                }
                if (IsLeaderNode(nodes[i]))
                {
                    leader = nodes[i];
                    leader_index = i;
                    ++leader_count;
                }
            }

            last_leader_count = leader_count;
            last_cluster_state = DescribeCluster(nodes, excluded);

            if (leader_count == 1)
            {
                if (leader_index == last_leader_index)
                {
                    ++stable_observations;
                }
                else
                {
                    last_leader_index = leader_index;
                    stable_observations = 1;
                }

                if (stable_observations >= required_observations)
                {
                    StableLeaderObservation observation;
                    observation.leader = leader;
                    observation.leader_index = leader_index;
                    observation.stable_observations = stable_observations;
                    observation.diagnostics =
                        "stable leader_index=" + std::to_string(leader_index) +
                        ", observations=" + std::to_string(stable_observations) +
                        ", leader=" + leader->Describe() +
                        ", cluster=" + last_cluster_state;
                    return observation;
                }
            }
            else
            {
                last_leader_index = nodes.size();
                stable_observations = 0;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        StableLeaderObservation observation;
        observation.leader_index = last_leader_index;
        observation.stable_observations = stable_observations;
        observation.diagnostics =
            "leader did not stabilize, last_leader_count=" +
            std::to_string(last_leader_count) +
            ", last_leader_index=" + std::to_string(last_leader_index) +
            ", stable_observations=" + std::to_string(stable_observations) +
            ", cluster=" + last_cluster_state;
        return std::nullopt;
    }

    inline bool WaitForSyntheticObjectOnNode(const std::shared_ptr<RaftNode> &node,
                                             const std::string &key,
                                             const std::string &expected_value,
                                             const std::chrono::milliseconds timeout,
                                             std::string *diagnostics = nullptr)
    {
        const auto deadline = Clock::now() + timeout;
        std::string last_value;
        std::string last_describe = node ? node->Describe() : "null";
        bool saw_value = false;
        while (Clock::now() < deadline)
        {
            if (node)
            {
                const MetadataStateMachine *state_machine = GetMetadataStateMachine(node);
                if (state_machine != nullptr &&
                    SyntheticStateMatchesValue(*state_machine, key, expected_value))
                {
                    if (diagnostics != nullptr)
                    {
                        *diagnostics = "value observed, key=" + key + ", value=" + expected_value +
                                       ", describe=" + node->Describe();
                    }
                    return true;
                }
                saw_value = state_machine != nullptr &&
                            state_machine->FindIndexedObjectId(kSyntheticMetadataBucket, key)
                                .has_value();
                last_value = saw_value ? "<present-but-unexpected>" : "<missing>";
                last_describe = node->Describe();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (diagnostics != nullptr)
        {
            *diagnostics = "value not observed, key=" + key +
                           ", expected=" + expected_value +
                           ", observed=" + (saw_value ? last_value : "<missing>") +
                           ", describe=" + last_describe;
        }
        return false;
    }

    inline bool WaitForSyntheticObjectOnAll(
        const std::vector<std::shared_ptr<RaftNode>> &nodes,
        const std::string &key,
        const std::string &expected_value,
        const std::chrono::milliseconds timeout,
        const std::vector<std::size_t> &excluded = {},
        std::string *diagnostics = nullptr)
    {
        const auto deadline = Clock::now() + timeout;
        std::string last_cluster_state;
        while (Clock::now() < deadline)
        {
            bool all_match = true;
            std::ostringstream cluster_values;

            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                if (IsExcluded(i, excluded) || !nodes[i])
                {
                    continue;
                }

                if (cluster_values.tellp() > 0)
                {
                    cluster_values << " | ";
                }
                cluster_values << "node[" << i << "]=";
                const MetadataStateMachine *state_machine = GetMetadataStateMachine(nodes[i]);
                if (state_machine == nullptr ||
                    !SyntheticStateMatchesValue(*state_machine, key, expected_value))
                {
                    cluster_values << (state_machine == nullptr ? "<no-metadata-sm>"
                                                                : "<missing-or-unexpected>");
                    all_match = false;
                    break;
                }
                cluster_values << expected_value;
            }

            last_cluster_state = cluster_values.str();

            if (all_match)
            {
                if (diagnostics != nullptr)
                {
                    *diagnostics = "value observed on all nodes, key=" + key +
                                   ", value=" + expected_value +
                                   ", cluster=" + DescribeCluster(nodes, excluded);
                }
                return true;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (diagnostics != nullptr)
        {
            *diagnostics = "value not observed on all nodes, key=" + key +
                           ", expected=" + expected_value +
                           ", cluster_values=" + last_cluster_state +
                           ", cluster=" + DescribeCluster(nodes, excluded);
        }
        return false;
    }

    inline bool WaitForSyntheticObjectMissingOnAll(
        const std::vector<std::shared_ptr<RaftNode>> &nodes,
        const std::string &key,
        const std::chrono::milliseconds timeout,
        const std::vector<std::size_t> &excluded = {},
        std::string *diagnostics = nullptr)
    {
        const auto deadline = Clock::now() + timeout;
        std::string last_cluster_state;
        while (Clock::now() < deadline)
        {
            bool all_missing = true;
            std::ostringstream cluster_values;

            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                if (IsExcluded(i, excluded) || !nodes[i])
                {
                    continue;
                }

                if (cluster_values.tellp() > 0)
                {
                    cluster_values << " | ";
                }
                cluster_values << "node[" << i << "]=";
                const MetadataStateMachine *state_machine = GetMetadataStateMachine(nodes[i]);
                if (state_machine == nullptr ||
                    !SyntheticStateMatchesMissing(*state_machine, key))
                {
                    cluster_values << (state_machine == nullptr ? "<no-metadata-sm>" : "<present>");
                    all_missing = false;
                    break;
                }
                cluster_values << "<missing>";
            }

            last_cluster_state = cluster_values.str();

            if (all_missing)
            {
                if (diagnostics != nullptr)
                {
                    *diagnostics = "value missing on all nodes, key=" + key +
                                   ", cluster=" + DescribeCluster(nodes, excluded);
                }
                return true;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (diagnostics != nullptr)
        {
            *diagnostics = "value still present on some node, key=" + key +
                           ", cluster_values=" + last_cluster_state +
                           ", cluster=" + DescribeCluster(nodes, excluded);
        }
        return false;
    }

    inline bool WaitForNodeFieldAtLeast(const std::shared_ptr<RaftNode> &node,
                                        const std::string &field_name,
                                        const std::uint64_t minimum,
                                        const std::chrono::milliseconds timeout,
                                        std::string *diagnostics = nullptr)
    {
        const auto deadline = Clock::now() + timeout;
        std::optional<std::uint64_t> last_value;
        std::string last_describe = node ? node->Describe() : "null";
        while (Clock::now() < deadline)
        {
            if (node)
            {
                last_describe = node->Describe();
                const auto value = ExtractUintField(last_describe, field_name);
                if (value.has_value() && *value >= minimum)
                {
                    if (diagnostics != nullptr)
                    {
                        *diagnostics = "field reached threshold, field=" + field_name +
                                       ", value=" + std::to_string(*value) +
                                       ", minimum=" + std::to_string(minimum) +
                                       ", describe=" + last_describe;
                    }
                    return true;
                }
                last_value = value;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (diagnostics != nullptr)
        {
            *diagnostics = "field did not reach threshold, field=" + field_name +
                           ", observed=" +
                           (last_value.has_value() ? std::to_string(*last_value)
                                                   : std::string("<missing>")) +
                           ", minimum=" + std::to_string(minimum) +
                           ", describe=" + last_describe;
        }
        return false;
    }

    inline bool WaitForOrderedCommitApplyAtLeast(
        const std::vector<std::shared_ptr<RaftNode>> &nodes,
        const std::uint64_t minimum_index,
        const std::chrono::milliseconds timeout,
        std::string *diagnostics = nullptr,
        const std::vector<std::size_t> &excluded = {})
    {
        const auto deadline = Clock::now() + timeout;
        std::string last_detail;

        while (Clock::now() < deadline)
        {
            bool all_ready = true;
            last_detail.clear();

            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                if (IsExcluded(i, excluded) || !nodes[i])
                {
                    continue;
                }

                const std::string describe = nodes[i]->Describe();
                const auto commit_index = ExtractUintField(describe, "commit_index");
                const auto last_applied = ExtractUintField(describe, "last_applied");
                if (!commit_index.has_value() || !last_applied.has_value())
                {
                    all_ready = false;
                    last_detail = "missing commit/apply fields on node[" +
                                  std::to_string(i) + "], describe=" + describe;
                    continue;
                }
                if (*last_applied > *commit_index)
                {
                    if (diagnostics != nullptr)
                    {
                        *diagnostics = "last_applied exceeded commit_index on node[" +
                                       std::to_string(i) + "], describe=" + describe;
                    }
                    return false;
                }
                if (*commit_index < minimum_index || *last_applied < minimum_index)
                {
                    all_ready = false;
                    last_detail = "node[" + std::to_string(i) +
                                  "] not at replay frontier, describe=" + describe;
                }
            }

            if (all_ready)
            {
                if (diagnostics != nullptr)
                {
                    *diagnostics =
                        "commit/apply reached replay frontier on all nodes, minimum_index=" +
                        std::to_string(minimum_index) +
                        ", cluster=" + DescribeCluster(nodes, excluded);
                }
                return true;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (diagnostics != nullptr)
        {
            *diagnostics =
                "timed out waiting for ordered commit/apply frontier, minimum_index=" +
                std::to_string(minimum_index) +
                ", detail=" + last_detail +
                ", cluster=" + DescribeCluster(nodes, excluded);
        }
        return false;
    }

    inline bool ProposeWithRetry(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                                 const SyntheticMetadataMutation &command,
                                 const std::chrono::milliseconds timeout,
                                 ProposeResult *final_result,
                                 const std::vector<std::size_t> &excluded = {},
                                 std::string *diagnostics = nullptr)
    {
        const auto deadline = Clock::now() + timeout;
        ProposeResult last_result;
        std::size_t last_leader_index = nodes.size();
        std::string last_leader_describe = "none";
        std::string last_cluster_state = DescribeCluster(nodes, excluded);
        int attempts = 0;
        int no_leader_rounds = 0;

        while (Clock::now() < deadline)
        {
            auto leader = WaitForSingleLeader(nodes, std::chrono::milliseconds(500), excluded);
            if (leader == nullptr)
            {
                ++no_leader_rounds;
                last_cluster_state = DescribeCluster(nodes, excluded);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            ++attempts;
            last_leader_index = FindNodeIndex(nodes, leader);
            last_leader_describe = leader->Describe();
            last_cluster_state = DescribeCluster(nodes, excluded);

            last_result = {};
            if (!ApplySyntheticMetadataMutation(leader, command, &last_result))
            {
                if (last_result.Ok())
                {
                    last_result.status = ProposeStatus::kApplyFailed;
                    last_result.message = "synthetic metadata bridge failed";
                }
            }
            if (last_result.Ok())
            {
                if (final_result != nullptr)
                {
                    *final_result = last_result;
                }
                if (diagnostics != nullptr)
                {
                    *diagnostics =
                        "proposal committed, attempts=" + std::to_string(attempts) +
                        ", leader_index=" + std::to_string(last_leader_index) +
                        ", leader=" + last_leader_describe +
                        ", cluster=" + last_cluster_state;
                }
                return true;
            }

            if (last_result.status == ProposeStatus::kInvalidCommand ||
                last_result.status == ProposeStatus::kApplyFailed ||
                last_result.status == ProposeStatus::kCommitFailed)
            {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (final_result != nullptr)
        {
            *final_result = last_result;
        }
        if (diagnostics != nullptr)
        {
            std::string category = "proposal_failure";
            if (no_leader_rounds > 0 && attempts == 0)
            {
                category = "leader_not_available_before_propose";
            }
            else if (last_result.status == ProposeStatus::kNotLeader)
            {
                category = "leadership_churn_during_propose";
            }
            else if (last_result.status == ProposeStatus::kReplicationFailed ||
                     last_result.status == ProposeStatus::kTimeout ||
                     last_result.status == ProposeStatus::kCommitFailed)
            {
                category = "proposal_failed_before_majority_or_commit";
            }
            else if (last_result.status == ProposeStatus::kApplyFailed)
            {
                category = "proposal_committed_but_apply_failed";
            }
            else if (last_result.status == ProposeStatus::kInvalidCommand)
            {
                category = "invalid_command";
            }
            else if (last_result.status == ProposeStatus::kNodeStopping)
            {
                category = "node_stopping";
            }

            *diagnostics = "category=" + category +
                           ", attempts=" + std::to_string(attempts) +
                           ", no_leader_rounds=" +
                           std::to_string(no_leader_rounds) +
                           ", last_leader_index=" + std::to_string(last_leader_index) +
                           ", last_leader=" + last_leader_describe +
                           ", last_status=" + ProposeStatusName(last_result.status) +
                           ", last_message=" + last_result.message +
                           ", cluster=" + last_cluster_state;
        }
        return false;
    }

    inline void WriteSyntheticObjects(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                                      const std::string &prefix,
                                      const int count,
                                      const std::vector<std::size_t> &excluded = {})
    {
        ProposeResult result;
        for (int i = 0; i < count; ++i)
        {
            std::string diagnostics;
            SCOPED_TRACE(prefix + " write " + std::to_string(i));
            ASSERT_TRUE(ProposeWithRetry(nodes,
                                         WriteSyntheticObject(prefix + "_" + std::to_string(i),
                                                              "value_" + std::to_string(i)),
                                         std::chrono::seconds(10),
                                         &result,
                                         excluded,
                                         &diagnostics))
                << "write failed, status=" << ProposeStatusName(result.status)
                << ", message=" << result.message
                << ", diagnostics=" << diagnostics;
        }
    }

    class SnapshotRestartTestBase : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *test_info = ::testing::UnitTest::GetInstance()->current_test_info();
            test_name_ = std::string(test_info->test_suite_name()) + "." + test_info->name();

            root_ = MakeRestartTestRoot(test_name_);
            data_root_ = root_ / "raft_data";
            snapshot_root_ = root_ / "raft_snapshots";
            base_port_ = PickBasePort(test_name_);

            std::error_code ec;
            std::filesystem::remove_all(root_, ec);
            std::filesystem::create_directories(data_root_, ec);
            ASSERT_FALSE(ec) << "failed to create data root: " << ec.message();

            std::filesystem::create_directories(snapshot_root_, ec);
            ASSERT_FALSE(ec) << "failed to create snapshot root: " << ec.message();

            RecordProperty("test_root", root_.string());
            RecordProperty("base_port", std::to_string(base_port_));
        }

        void TearDown() override
        {
            std::error_code ec;
            if (!HasFailure())
            {
                std::filesystem::remove_all(root_, ec);
            }
            else
            {
                std::cout << "preserved test root: " << root_.string() << "\n";
            }
        }

        TestCluster MakeCluster(const std::string &case_name,
                                const bool snapshot_enabled,
                                const std::uint64_t snapshot_log_threshold) const
        {
            return TestCluster(BuildThreeNodeConfigs(data_root_ / case_name, base_port_),
                               BuildThreeSnapshotConfigs(snapshot_root_ / case_name,
                                                         snapshot_enabled,
                                                         snapshot_log_threshold));
        }

        std::string test_name_;
        std::filesystem::path root_;
        std::filesystem::path data_root_;
        std::filesystem::path snapshot_root_;
        int base_port_{0};
    };
} // namespace raftdemo::test
