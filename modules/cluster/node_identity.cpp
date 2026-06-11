#include "cluster/node_identity.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <process.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace clusterdemo
{
    namespace
    {
        struct ParsedIdentityFile
        {
            NodeIdentity identity;
            bool saw_raft_id{false};
            bool saw_membership_state{false};
            bool saw_persistent_generation{false};
        };

        std::string PathToDiagnosticString(const std::filesystem::path &path)
        {
            return path.generic_string();
        }

        std::string TrimAscii(std::string_view value)
        {
            std::size_t begin = 0;
            while (begin < value.size() &&
                   std::isspace(static_cast<unsigned char>(value[begin])) != 0)
            {
                ++begin;
            }

            std::size_t end = value.size();
            while (end > begin &&
                   std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
            {
                --end;
            }

            return std::string(value.substr(begin, end - begin));
        }

        void AddIssue(NodeIdentityValidationResult *result,
                      const NodeIdentityIssueCode code,
                      std::string field_path,
                      std::string message,
                      const ClusterNodeType node_type = ClusterNodeType::kUnknown,
                      ClusterNodeId node_id = {},
                      std::optional<std::int32_t> raft_id = std::nullopt,
                      std::filesystem::path path = {})
        {
            if (result == nullptr)
            {
                return;
            }

            result->issues.push_back(NodeIdentityIssue{
                .code = code,
                .field_path = std::move(field_path),
                .message = std::move(message),
                .node_type = node_type,
                .node_id = std::move(node_id),
                .raft_id = raft_id,
                .path = std::move(path)});
        }

        bool HasIssueCode(const NodeIdentityValidationResult &result,
                          const NodeIdentityIssueCode code)
        {
            return std::any_of(result.issues.begin(),
                               result.issues.end(),
                               [code](const NodeIdentityIssue &issue)
                               {
                                   return issue.code == code;
                               });
        }

        std::string JoinIssues(const NodeIdentityValidationResult &result)
        {
            std::ostringstream oss;
            for (std::size_t i = 0; i < result.issues.size(); ++i)
            {
                if (i != 0)
                {
                    oss << "; ";
                }
                oss << DescribeNodeIdentityIssue(result.issues[i]);
            }
            return oss.str();
        }

        std::string QuoteDiagnosticValue(const std::string_view value)
        {
            return "'" + std::string(value) + "'";
        }

        std::string QuoteDiagnosticOptionalRaftId(
            const std::optional<std::int32_t> raft_id)
        {
            if (!raft_id.has_value())
            {
                return "<none>";
            }
            return "'" + std::to_string(*raft_id) + "'";
        }

        bool IsSupportedIdentityVersion(const std::uint32_t identity_version)
        {
            return identity_version == kNodeIdentityCurrentVersion;
        }

        NodeIdentityMembershipState InferDefaultMembershipState(
            const NodeIdentity &identity)
        {
            if (identity.node_type != ClusterNodeType::kMetadata)
            {
                return NodeIdentityMembershipState::kNonRaft;
            }

            if (identity.source == NodeIdentitySource::kConfigGenerator &&
                identity.raft_id.has_value() && *identity.raft_id > 0)
            {
                return NodeIdentityMembershipState::kVoter;
            }

            if (identity.source == NodeIdentitySource::kExplicitOverride)
            {
                return NodeIdentityMembershipState::kCandidate;
            }

            if (identity.raft_id.has_value() && *identity.raft_id > 0)
            {
                return NodeIdentityMembershipState::kCandidate;
            }

            return NodeIdentityMembershipState::kJoining;
        }

        NodeIdentity NormalizeNodeIdentity(NodeIdentity identity)
        {
            if (identity.membership_state ==
                NodeIdentityMembershipState::kUnknown)
            {
                identity.membership_state =
                    InferDefaultMembershipState(identity);
            }
            return identity;
        }

        NodeIdentityValidationResult ValidateNodeIdentityImpl(
            const NodeIdentity &identity,
            const bool allow_membership_inference)
        {
            NodeIdentityValidationResult result;
            const auto normalized_identity = allow_membership_inference
                                                 ? NormalizeNodeIdentity(identity)
                                                 : identity;

            if (normalized_identity.cluster_id.empty())
            {
                AddIssue(&result,
                         NodeIdentityIssueCode::kMissingClusterId,
                         "cluster_id",
                         "cluster_id must not be empty",
                         normalized_identity.node_type,
                         normalized_identity.node_id,
                         normalized_identity.raft_id);
            }

            if (normalized_identity.node_id.empty())
            {
                AddIssue(&result,
                         NodeIdentityIssueCode::kMissingNodeId,
                         "node_id",
                         "node_id must not be empty",
                         normalized_identity.node_type,
                         normalized_identity.node_id,
                         normalized_identity.raft_id);
            }

            if (normalized_identity.node_type == ClusterNodeType::kUnknown)
            {
                AddIssue(&result,
                         NodeIdentityIssueCode::kInvalidNodeType,
                         "node_type",
                         "node_type must be view, metadata or storage",
                         normalized_identity.node_type,
                         normalized_identity.node_id,
                         normalized_identity.raft_id);
            }

            if (normalized_identity.identity_version == 0)
            {
                AddIssue(&result,
                         NodeIdentityIssueCode::kInvalidIdentityVersion,
                         "identity_version",
                         "identity_version must be greater than zero",
                         normalized_identity.node_type,
                         normalized_identity.node_id,
                         normalized_identity.raft_id);
            }
            else if (!IsSupportedIdentityVersion(normalized_identity.identity_version))
            {
                AddIssue(&result,
                         NodeIdentityIssueCode::kUnsupportedIdentityVersion,
                         "identity_version",
                         "identity_version is not supported by current binary; 009 only accepts the current node.identity schema",
                         normalized_identity.node_type,
                         normalized_identity.node_id,
                         normalized_identity.raft_id);
            }

            if (normalized_identity.persistent_generation == 0)
            {
                AddIssue(&result,
                         NodeIdentityIssueCode::kInvalidPersistentGeneration,
                         "persistent_generation",
                         "persistent_generation must be greater than zero",
                         normalized_identity.node_type,
                         normalized_identity.node_id,
                         normalized_identity.raft_id);
            }

            if (normalized_identity.node_type == ClusterNodeType::kMetadata)
            {
                switch (normalized_identity.membership_state)
                {
                case NodeIdentityMembershipState::kJoining:
                case NodeIdentityMembershipState::kCandidate:
                    if (normalized_identity.raft_id.has_value() &&
                        *normalized_identity.raft_id <= 0)
                    {
                        AddIssue(
                            &result,
                            NodeIdentityIssueCode::kMissingRaftId,
                            "raft_id",
                            "metadata joining/candidate identity may omit raft_id, but any provided raft_id must be positive",
                            normalized_identity.node_type,
                            normalized_identity.node_id,
                            normalized_identity.raft_id);
                    }
                    break;
                case NodeIdentityMembershipState::kLearner:
                case NodeIdentityMembershipState::kVoter:
                    if (!normalized_identity.raft_id.has_value() ||
                        *normalized_identity.raft_id <= 0)
                    {
                        AddIssue(&result,
                                 NodeIdentityIssueCode::kMissingRaftId,
                                 "raft_id",
                                 "metadata learner/voter identity requires positive raft_id",
                                 normalized_identity.node_type,
                                 normalized_identity.node_id,
                                 normalized_identity.raft_id);
                    }
                    break;
                case NodeIdentityMembershipState::kUnknown:
                case NodeIdentityMembershipState::kNonRaft:
                    AddIssue(&result,
                             NodeIdentityIssueCode::kInvalidMembershipState,
                             "membership_state",
                             "metadata identity must use joining, candidate, learner or voter membership_state",
                             normalized_identity.node_type,
                             normalized_identity.node_id,
                             normalized_identity.raft_id);
                    break;
                }

                if (normalized_identity.membership_state ==
                        NodeIdentityMembershipState::kVoter &&
                    normalized_identity.source == NodeIdentitySource::kExplicitOverride)
                {
                    AddIssue(&result,
                             NodeIdentityIssueCode::kInvalidMembershipState,
                             "membership_state",
                             "metadata dynamic join/local override identity must not persist voter membership_state without bootstrap or committed membership authority",
                             normalized_identity.node_type,
                             normalized_identity.node_id,
                             normalized_identity.raft_id);
                }
            }
            else
            {
                if (normalized_identity.raft_id.has_value())
                {
                    AddIssue(&result,
                             NodeIdentityIssueCode::kUnexpectedRaftId,
                             "raft_id",
                             "non-metadata node identity must not carry raft_id",
                             normalized_identity.node_type,
                             normalized_identity.node_id,
                             normalized_identity.raft_id);
                }

                if (normalized_identity.membership_state !=
                    NodeIdentityMembershipState::kNonRaft)
                {
                    AddIssue(&result,
                             NodeIdentityIssueCode::kInvalidMembershipState,
                             "membership_state",
                             "view/storage identity must persist non_raft membership_state",
                             normalized_identity.node_type,
                             normalized_identity.node_id,
                             normalized_identity.raft_id);
                }
            }

            return result;
        }

        std::string BuildIdentitySummary(const NodeIdentity &identity)
        {
            std::ostringstream oss;
            oss << "cluster_id=" << QuoteDiagnosticValue(identity.cluster_id)
                << ", node_id=" << QuoteDiagnosticValue(identity.node_id)
                << ", node_type=" << QuoteDiagnosticValue(ToString(identity.node_type))
                << ", raft_id=" << QuoteDiagnosticOptionalRaftId(identity.raft_id)
                << ", membership_state="
                << QuoteDiagnosticValue(ToString(identity.membership_state))
                << ", persistent_generation='"
                << identity.persistent_generation << "'"
                << ", source=" << QuoteDiagnosticValue(ToString(identity.source));
            return oss.str();
        }

        NodeIdentityValidationResult ValidateNodeIdentityMatchesDetailed(
            const NodeIdentity &identity,
            const ExpectedNodeIdentity &expected,
            const std::filesystem::path &identity_path = {},
            const std::string_view subject = "existing identity")
        {
            NodeIdentityValidationResult result;
            const auto make_message = [&](const std::string &detail)
            {
                std::ostringstream oss;
                oss << subject << " " << detail;
                if (!identity_path.empty())
                {
                    oss << " at " << PathToDiagnosticString(identity_path);
                }
                return oss.str();
            };

            if (expected.cluster_id.has_value() &&
                identity.cluster_id != *expected.cluster_id)
            {
                AddIssue(&result,
                         NodeIdentityIssueCode::kClusterIdMismatch,
                         "cluster_id",
                         make_message("cluster_id mismatch: expected=" +
                                      QuoteDiagnosticValue(*expected.cluster_id) +
                                      ", actual=" +
                                      QuoteDiagnosticValue(identity.cluster_id) +
                                      "; refusing to reuse data_dir for a different cluster"),
                         identity.node_type,
                         identity.node_id,
                         identity.raft_id,
                         identity_path);
            }

            if (expected.node_id.has_value() &&
                identity.node_id != *expected.node_id)
            {
                AddIssue(&result,
                         NodeIdentityIssueCode::kNodeIdMismatch,
                         "node_id",
                         make_message("node_id mismatch: expected=" +
                                      QuoteDiagnosticValue(*expected.node_id) +
                                      ", actual=" +
                                      QuoteDiagnosticValue(identity.node_id) +
                                      "; refusing to start with another node's durable identity"),
                         identity.node_type,
                         identity.node_id,
                         identity.raft_id,
                         identity_path);
            }

            if (expected.node_type != ClusterNodeType::kUnknown &&
                identity.node_type != expected.node_type)
            {
                AddIssue(&result,
                         NodeIdentityIssueCode::kNodeTypeMismatch,
                         "node_type",
                         make_message("node_type mismatch: expected=" +
                                      QuoteDiagnosticValue(
                                          ToString(expected.node_type)) +
                                      ", actual=" +
                                      QuoteDiagnosticValue(
                                          ToString(identity.node_type)) +
                                      "; refusing to reuse identity across node roles"),
                         identity.node_type,
                         identity.node_id,
                         identity.raft_id,
                         identity_path);
            }

            if (expected.raft_id.has_value() &&
                identity.raft_id != expected.raft_id)
            {
                AddIssue(&result,
                         NodeIdentityIssueCode::kRaftIdMismatch,
                         "raft_id",
                         make_message("raft_id mismatch: expected=" +
                                      QuoteDiagnosticOptionalRaftId(
                                          expected.raft_id) +
                                      ", actual=" +
                                      QuoteDiagnosticOptionalRaftId(
                                          identity.raft_id) +
                                      "; refusing to reuse a different Raft identity"),
                         identity.node_type,
                         identity.node_id,
                         identity.raft_id,
                         identity_path);
            }

            if (expected.membership_state.has_value() &&
                identity.membership_state != *expected.membership_state)
            {
                AddIssue(&result,
                         NodeIdentityIssueCode::kMembershipStateMismatch,
                         "membership_state",
                         make_message("membership_state mismatch: expected=" +
                                      QuoteDiagnosticValue(ToString(
                                          *expected.membership_state)) +
                                      ", actual=" +
                                      QuoteDiagnosticValue(ToString(
                                          identity.membership_state)) +
                                      "; refusing to reinterpret durable metadata authority state"),
                         identity.node_type,
                         identity.node_id,
                         identity.raft_id,
                         identity_path);
            }

            if (expected.source.has_value() &&
                identity.source != *expected.source)
            {
                AddIssue(&result,
                         NodeIdentityIssueCode::kSourceMismatch,
                         "source",
                         make_message("source mismatch: expected=" +
                                      QuoteDiagnosticValue(
                                          ToString(*expected.source)) +
                                      ", actual=" +
                                      QuoteDiagnosticValue(
                                          ToString(identity.source)) +
                                      "; refusing to silently reinterpret durable identity provenance"),
                         identity.node_type,
                         identity.node_id,
                         identity.raft_id,
                         identity_path);
            }

            const auto expected_membership_state =
                expected.membership_state.value_or(identity.membership_state);
            const bool expected_metadata_needs_positive_raft_id =
                expected.require_raft_id_for_metadata &&
                identity.node_type == ClusterNodeType::kMetadata &&
                expected_membership_state !=
                    NodeIdentityMembershipState::kJoining &&
                expected_membership_state !=
                    NodeIdentityMembershipState::kCandidate;
            if (expected_metadata_needs_positive_raft_id &&
                (!identity.raft_id.has_value() || *identity.raft_id <= 0))
            {
                AddIssue(&result,
                         NodeIdentityIssueCode::kMissingRaftId,
                         "raft_id",
                         make_message("metadata identity must provide positive raft_id; actual=" +
                                      QuoteDiagnosticOptionalRaftId(identity.raft_id)),
                         identity.node_type,
                         identity.node_id,
                         identity.raft_id,
                         identity_path);
            }

            if (expected.forbid_raft_id_for_non_metadata &&
                identity.node_type != ClusterNodeType::kMetadata &&
                identity.raft_id.has_value())
            {
                AddIssue(&result,
                         NodeIdentityIssueCode::kUnexpectedRaftId,
                         "raft_id",
                         make_message("non-metadata identity must not provide raft_id; actual=" +
                                      QuoteDiagnosticOptionalRaftId(identity.raft_id) +
                                      "; refusing to treat non-Raft nodes as MetadataNode members"),
                         identity.node_type,
                         identity.node_id,
                         identity.raft_id,
                         identity_path);
            }

            return result;
        }

        bool ParseInt64(std::string_view text, std::int64_t *out)
        {
            if (out == nullptr)
            {
                return false;
            }

            const auto trimmed = TrimAscii(text);
            if (trimmed.empty())
            {
                return false;
            }

            const char *begin = trimmed.data();
            const char *end = trimmed.data() + trimmed.size();
            std::int64_t value = 0;
            const auto parsed = std::from_chars(begin, end, value);
            if (parsed.ec != std::errc{} || parsed.ptr != end)
            {
                return false;
            }

            *out = value;
            return true;
        }

        bool ParseUInt32(std::string_view text, std::uint32_t *out)
        {
            if (out == nullptr)
            {
                return false;
            }

            const auto trimmed = TrimAscii(text);
            if (trimmed.empty())
            {
                return false;
            }

            const char *begin = trimmed.data();
            const char *end = trimmed.data() + trimmed.size();
            std::uint32_t value = 0;
            const auto parsed = std::from_chars(begin, end, value);
            if (parsed.ec != std::errc{} || parsed.ptr != end)
            {
                return false;
            }

            *out = value;
            return true;
        }

        bool ParseUInt64(std::string_view text, std::uint64_t *out)
        {
            if (out == nullptr)
            {
                return false;
            }

            const auto trimmed = TrimAscii(text);
            if (trimmed.empty())
            {
                return false;
            }

            const char *begin = trimmed.data();
            const char *end = trimmed.data() + trimmed.size();
            std::uint64_t value = 0;
            const auto parsed = std::from_chars(begin, end, value);
            if (parsed.ec != std::errc{} || parsed.ptr != end)
            {
                return false;
            }

            *out = value;
            return true;
        }

        bool ParseInt32(std::string_view text, std::int32_t *out)
        {
            if (out == nullptr)
            {
                return false;
            }

            std::int64_t value = 0;
            if (!ParseInt64(text, &value) ||
                value < std::numeric_limits<std::int32_t>::min() ||
                value > std::numeric_limits<std::int32_t>::max())
            {
                return false;
            }

            *out = static_cast<std::int32_t>(value);
            return true;
        }

        std::optional<ClusterNodeType> ParseClusterNodeType(std::string_view text)
        {
            const auto trimmed = TrimAscii(text);
            if (trimmed == "view")
            {
                return ClusterNodeType::kView;
            }
            if (trimmed == "metadata")
            {
                return ClusterNodeType::kMetadata;
            }
            if (trimmed == "storage")
            {
                return ClusterNodeType::kStorage;
            }
            if (trimmed == "unknown")
            {
                return ClusterNodeType::kUnknown;
            }
            return std::nullopt;
        }

        std::optional<NodeIdentitySource> ParseNodeIdentitySource(
            std::string_view text)
        {
            const auto trimmed = TrimAscii(text);
            if (trimmed == "config_generator")
            {
                return NodeIdentitySource::kConfigGenerator;
            }
            if (trimmed == "view_node_allocator")
            {
                return NodeIdentitySource::kViewNodeAllocator;
            }
            if (trimmed == "explicit_override")
            {
                return NodeIdentitySource::kExplicitOverride;
            }
            if (trimmed == "unknown")
            {
                return NodeIdentitySource::kUnknown;
            }
            return std::nullopt;
        }

        std::optional<NodeIdentityMembershipState> ParseMembershipState(
            std::string_view text)
        {
            const auto trimmed = TrimAscii(text);
            if (trimmed == "unknown")
            {
                return NodeIdentityMembershipState::kUnknown;
            }
            if (trimmed == "non_raft")
            {
                return NodeIdentityMembershipState::kNonRaft;
            }
            if (trimmed == "joining")
            {
                return NodeIdentityMembershipState::kJoining;
            }
            if (trimmed == "candidate")
            {
                return NodeIdentityMembershipState::kCandidate;
            }
            if (trimmed == "learner")
            {
                return NodeIdentityMembershipState::kLearner;
            }
            if (trimmed == "voter")
            {
                return NodeIdentityMembershipState::kVoter;
            }
            return std::nullopt;
        }

        bool NodeIdentityEquals(const NodeIdentity &lhs, const NodeIdentity &rhs)
        {
            return lhs.cluster_id == rhs.cluster_id &&
                   lhs.node_id == rhs.node_id &&
                   lhs.node_type == rhs.node_type &&
                   lhs.raft_id == rhs.raft_id &&
                   lhs.membership_state == rhs.membership_state &&
                   lhs.persistent_generation == rhs.persistent_generation &&
                   lhs.identity_version == rhs.identity_version &&
                   lhs.created_at_unix_ms == rhs.created_at_unix_ms &&
                   lhs.source == rhs.source;
        }

        std::string BuildIoDiagnostic(const char *operation,
                                      const std::filesystem::path &path)
        {
            std::ostringstream oss;
            oss << operation << " failed for " << PathToDiagnosticString(path);
            if (errno != 0)
            {
                oss << ": " << std::strerror(errno);
            }
            return oss.str();
        }

#ifdef _WIN32
        std::string BuildWindowsDiagnostic(const char *operation,
                                           const std::filesystem::path &path,
                                           const DWORD error_value)
        {
            std::ostringstream oss;
            oss << operation << " failed for " << PathToDiagnosticString(path)
                << ": win32_error=" << error_value;
            return oss.str();
        }
#endif

        std::string BuildFilesystemDiagnostic(const char *operation,
                                              const std::filesystem::path &path,
                                              const std::error_code &ec)
        {
            std::ostringstream oss;
            oss << operation << " failed for " << PathToDiagnosticString(path);
            if (ec)
            {
                oss << ": " << ec.message();
            }
            return oss.str();
        }

        std::filesystem::path MakeStagingPath(
            const std::filesystem::path &identity_path)
        {
            const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
#ifdef _WIN32
            const auto pid = static_cast<unsigned long>(_getpid());
#else
            const auto pid = static_cast<unsigned long>(::getpid());
#endif
            std::ostringstream suffix;
            suffix << ".tmp." << pid << "." << now_ns;
            auto path = identity_path;
            path += suffix.str();
            return path;
        }

        bool ReadTextFile(const std::filesystem::path &path,
                          std::string *content,
                          std::string *diagnostic)
        {
            if (content == nullptr)
            {
                return false;
            }

            std::ifstream input(path, std::ios::binary);
            if (!input.is_open())
            {
                if (diagnostic != nullptr)
                {
                    *diagnostic = BuildIoDiagnostic("open", path);
                }
                return false;
            }

            std::ostringstream buffer;
            buffer << input.rdbuf();
            if (!input.good() && !input.eof())
            {
                if (diagnostic != nullptr)
                {
                    *diagnostic = BuildIoDiagnostic("read", path);
                }
                return false;
            }

            *content = buffer.str();
            return true;
        }

        std::optional<ParsedIdentityFile> ParseIdentityFile(
            const std::string &content,
            NodeIdentityValidationResult *validation,
            std::string *diagnostic)
        {
            ParsedIdentityFile parsed;
            std::array<bool, 9> seen_fields{
                false, false, false, false, false, false, false, false, false};

            auto add_corrupt_issue = [&](std::string field_path, std::string message)
            {
                AddIssue(validation,
                         NodeIdentityIssueCode::kIdentityFileCorrupt,
                         std::move(field_path),
                         std::move(message));
            };

            std::size_t line_no = 0;
            std::size_t offset = 0;
            while (offset <= content.size())
            {
                const auto next = content.find('\n', offset);
                const auto line_end =
                    next == std::string::npos ? content.size() : next;
                std::string line = content.substr(offset, line_end - offset);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                offset = next == std::string::npos ? content.size() + 1 : next + 1;
                ++line_no;

                const auto trimmed = TrimAscii(line);
                if (trimmed.empty())
                {
                    continue;
                }

                const auto separator = trimmed.find('=');
                if (separator == std::string::npos || separator == 0)
                {
                    add_corrupt_issue("line[" + std::to_string(line_no) + "]",
                                      "node.identity line must use key=value format");
                    continue;
                }

                const auto key = TrimAscii(
                    std::string_view(trimmed.data(), separator));
                const auto value = TrimAscii(
                    std::string_view(trimmed.data() + separator + 1,
                                     trimmed.size() - separator - 1));

                auto mark_seen = [&](const std::size_t index) -> bool
                {
                    if (seen_fields[index])
                    {
                        add_corrupt_issue(key, "duplicate field in node.identity");
                        return false;
                    }
                    seen_fields[index] = true;
                    return true;
                };

                if (key == "identity_version")
                {
                    if (!mark_seen(0))
                    {
                        continue;
                    }

                    std::uint32_t identity_version = 0;
                    if (!ParseUInt32(value, &identity_version))
                    {
                        add_corrupt_issue(key, "identity_version must be uint32");
                        continue;
                    }
                    parsed.identity.identity_version = identity_version;
                    continue;
                }

                if (key == "cluster_id")
                {
                    if (!mark_seen(1))
                    {
                        continue;
                    }
                    parsed.identity.cluster_id = value;
                    continue;
                }

                if (key == "node_id")
                {
                    if (!mark_seen(2))
                    {
                        continue;
                    }
                    parsed.identity.node_id = value;
                    continue;
                }

                if (key == "node_type")
                {
                    if (!mark_seen(3))
                    {
                        continue;
                    }

                    const auto parsed_type = ParseClusterNodeType(value);
                    if (!parsed_type.has_value())
                    {
                        add_corrupt_issue(key,
                                          "node_type must be view, metadata, storage or unknown");
                        continue;
                    }
                    parsed.identity.node_type = *parsed_type;
                    continue;
                }

                if (key == "raft_id")
                {
                    if (!mark_seen(4))
                    {
                        continue;
                    }
                    parsed.saw_raft_id = true;
                    if (value.empty())
                    {
                        parsed.identity.raft_id.reset();
                        continue;
                    }

                    std::int32_t raft_id = 0;
                    if (!ParseInt32(value, &raft_id))
                    {
                        add_corrupt_issue(key, "raft_id must be int32 when provided");
                        continue;
                    }
                    parsed.identity.raft_id = raft_id;
                    continue;
                }

                if (key == "created_at_unix_ms")
                {
                    if (!mark_seen(5))
                    {
                        continue;
                    }

                    std::int64_t created_at_unix_ms = 0;
                    if (!ParseInt64(value, &created_at_unix_ms))
                    {
                        add_corrupt_issue(key, "created_at_unix_ms must be int64");
                        continue;
                    }
                    parsed.identity.created_at_unix_ms = created_at_unix_ms;
                    continue;
                }

                if (key == "membership_state")
                {
                    if (!mark_seen(6))
                    {
                        continue;
                    }

                    parsed.saw_membership_state = true;
                    const auto parsed_membership_state =
                        ParseMembershipState(value);
                    if (!parsed_membership_state.has_value())
                    {
                        add_corrupt_issue(
                            key,
                            "membership_state must be unknown, non_raft, joining, candidate, learner or voter");
                        continue;
                    }
                    parsed.identity.membership_state =
                        *parsed_membership_state;
                    continue;
                }

                if (key == "persistent_generation")
                {
                    if (!mark_seen(7))
                    {
                        continue;
                    }

                    parsed.saw_persistent_generation = true;
                    std::uint64_t persistent_generation = 0;
                    if (!ParseUInt64(value, &persistent_generation))
                    {
                        add_corrupt_issue(
                            key,
                            "persistent_generation must be uint64");
                        continue;
                    }
                    parsed.identity.persistent_generation =
                        persistent_generation;
                    continue;
                }

                if (key == "source")
                {
                    if (!mark_seen(8))
                    {
                        continue;
                    }

                    const auto parsed_source = ParseNodeIdentitySource(value);
                    if (!parsed_source.has_value())
                    {
                        add_corrupt_issue(key,
                                          "source must be config_generator, view_node_allocator, explicit_override or unknown");
                        continue;
                    }
                    parsed.identity.source = *parsed_source;
                    continue;
                }

                add_corrupt_issue(key, "unknown field in node.identity");
            }

            if (!seen_fields[0])
            {
                add_corrupt_issue("identity_version",
                                  "missing identity_version field");
            }
            if (!seen_fields[1])
            {
                add_corrupt_issue("cluster_id", "missing cluster_id field");
            }
            if (!seen_fields[2])
            {
                add_corrupt_issue("node_id", "missing node_id field");
            }
            if (!seen_fields[3])
            {
                add_corrupt_issue("node_type", "missing node_type field");
            }
            if (!seen_fields[4])
            {
                add_corrupt_issue("raft_id",
                                  "missing raft_id field; use empty value when raft_id is intentionally unset");
            }
            if (!seen_fields[5])
            {
                add_corrupt_issue("created_at_unix_ms",
                                  "missing created_at_unix_ms field");
            }
            if (!seen_fields[6])
            {
                add_corrupt_issue("membership_state",
                                  "missing membership_state field in node.identity");
            }
            if (!seen_fields[7])
            {
                add_corrupt_issue("persistent_generation",
                                  "missing persistent_generation field in node.identity");
            }
            if (!seen_fields[8])
            {
                add_corrupt_issue("source", "missing source field");
            }

            if (validation != nullptr && !validation->ok())
            {
                if (diagnostic != nullptr)
                {
                    *diagnostic = JoinIssues(*validation);
                }
                return std::nullopt;
            }

            return parsed;
        }

        std::string SerializeIdentity(const NodeIdentity &identity)
        {
            std::ostringstream oss;
            oss << "identity_version=" << identity.identity_version << '\n'
                << "cluster_id=" << identity.cluster_id << '\n'
                << "node_id=" << identity.node_id << '\n'
                << "node_type=" << ToString(identity.node_type) << '\n'
                << "raft_id=";
            if (identity.raft_id.has_value())
            {
                oss << *identity.raft_id;
            }
            oss << '\n'
                << "created_at_unix_ms=" << identity.created_at_unix_ms << '\n'
                << "membership_state=" << ToString(identity.membership_state)
                << '\n'
                << "persistent_generation=" << identity.persistent_generation
                << '\n'
                << "source=" << ToString(identity.source) << '\n';
            return oss.str();
        }

        NodeIdentityStatusCode StoreStatusFromValidation(
            const NodeIdentityValidationResult &validation)
        {
            if (HasIssueCode(validation,
                             NodeIdentityIssueCode::kUnsupportedIdentityVersion) ||
                HasIssueCode(validation,
                             NodeIdentityIssueCode::kUnsupportedDurabilityMode))
            {
                return NodeIdentityStatusCode::kUnsupported;
            }
            return NodeIdentityStatusCode::kInvalidArgument;
        }

        NodeIdentityStatusCode LoadStatusFromValidation(
            const NodeIdentityValidationResult &validation)
        {
            if (HasIssueCode(validation,
                             NodeIdentityIssueCode::kUnsupportedIdentityVersion))
            {
                return NodeIdentityStatusCode::kUnsupported;
            }
            return NodeIdentityStatusCode::kCorrupt;
        }

        NodeIdentityStatusCode MatchStatusFromValidation(
            const NodeIdentityValidationResult &validation)
        {
            if (HasIssueCode(validation,
                             NodeIdentityIssueCode::kUnsupportedIdentityVersion))
            {
                return NodeIdentityStatusCode::kUnsupported;
            }
            return NodeIdentityStatusCode::kConflict;
        }

        void CleanupStagingFile(const std::filesystem::path &staging_path)
        {
            std::error_code ignored;
            std::filesystem::remove(staging_path, ignored);
        }

#ifndef _WIN32
        bool WriteAllLinux(const int fd,
                           const std::string &content,
                           std::string *diagnostic,
                           const std::filesystem::path &path)
        {
            const char *data = content.data();
            std::size_t remaining = content.size();
            while (remaining > 0)
            {
                const auto write_result = ::write(fd, data, remaining);
                if (write_result < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    if (diagnostic != nullptr)
                    {
                        *diagnostic = BuildIoDiagnostic("write", path);
                    }
                    return false;
                }
                data += write_result;
                remaining -= static_cast<std::size_t>(write_result);
            }
            return true;
        }

        bool SyncLinuxDirectory(const std::filesystem::path &directory_path,
                                std::string *diagnostic)
        {
            const int directory_fd =
                ::open(directory_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (directory_fd < 0)
            {
                if (diagnostic != nullptr)
                {
                    *diagnostic = BuildIoDiagnostic("open directory", directory_path);
                }
                return false;
            }

            if (::fsync(directory_fd) != 0)
            {
                if (diagnostic != nullptr)
                {
                    *diagnostic = BuildIoDiagnostic("fsync directory", directory_path);
                }
                ::close(directory_fd);
                return false;
            }

            if (::close(directory_fd) != 0)
            {
                if (diagnostic != nullptr)
                {
                    *diagnostic = BuildIoDiagnostic("close directory", directory_path);
                }
                return false;
            }

            return true;
        }
#else
        bool WriteAllWindows(const HANDLE handle,
                             const std::string &content,
                             std::string *diagnostic,
                             const std::filesystem::path &path)
        {
            const char *data = content.data();
            std::size_t remaining = content.size();
            while (remaining > 0)
            {
                const auto chunk_length = static_cast<DWORD>(std::min<std::size_t>(
                    remaining, static_cast<std::size_t>(0xffffffffU)));
                DWORD bytes_written = 0;
                if (::WriteFile(handle,
                                data,
                                chunk_length,
                                &bytes_written,
                                nullptr) == 0)
                {
                    if (diagnostic != nullptr)
                    {
                        *diagnostic = BuildIoDiagnostic("WriteFile", path);
                    }
                    return false;
                }

                data += bytes_written;
                remaining -= static_cast<std::size_t>(bytes_written);
            }
            return true;
        }
#endif

        NodeIdentityStoreResult PublishIdentityFile(
            const NodeIdentity &identity,
            const NodeIdentityStoreOptions &options)
        {
            NodeIdentityStoreResult result;
            result.identity_path = ResolveNodeIdentityPath(options.data_dir);

            std::error_code create_ec;
            std::filesystem::create_directories(options.data_dir, create_ec);
            if (create_ec)
            {
                result.status = NodeIdentityStatusCode::kIoError;
                result.diagnostic = BuildFilesystemDiagnostic(
                    "create_directories", options.data_dir, create_ec);
                return result;
            }

            const auto staging_path = MakeStagingPath(result.identity_path);
            const auto payload = SerializeIdentity(identity);

#ifdef _WIN32
            const auto staging_wide = staging_path.wstring();
            const auto final_wide = result.identity_path.wstring();
            const HANDLE handle = ::CreateFileW(staging_wide.c_str(),
                                                GENERIC_WRITE,
                                                0,
                                                nullptr,
                                                CREATE_NEW,
                                                FILE_ATTRIBUTE_NORMAL,
                                                nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                const auto error_value = ::GetLastError();
                result.status = NodeIdentityStatusCode::kIoError;
                result.diagnostic = BuildWindowsDiagnostic(
                    "CreateFileW", staging_path, error_value);
                return result;
            }

            const auto close_handle = [&]() -> bool
            {
                if (::CloseHandle(handle) == 0)
                {
                    const auto error_value = ::GetLastError();
                    result.status = NodeIdentityStatusCode::kIoError;
                    result.diagnostic = BuildWindowsDiagnostic(
                        "CloseHandle", staging_path, error_value);
                    return false;
                }
                return true;
            };

            if (!WriteAllWindows(handle, payload, &result.diagnostic, staging_path))
            {
                result.status = NodeIdentityStatusCode::kIoError;
                close_handle();
                CleanupStagingFile(staging_path);
                return result;
            }

            if (::FlushFileBuffers(handle) == 0)
            {
                const auto error_value = ::GetLastError();
                result.status = NodeIdentityStatusCode::kDurabilityError;
                result.diagnostic = BuildWindowsDiagnostic(
                    "FlushFileBuffers", staging_path, error_value);
                close_handle();
                CleanupStagingFile(staging_path);
                return result;
            }

            if (!close_handle())
            {
                CleanupStagingFile(staging_path);
                return result;
            }

            DWORD move_flags = MOVEFILE_WRITE_THROUGH;
            if (options.store_mode == NodeIdentityStoreMode::kReplaceOnlyIfMatchesExpected)
            {
                move_flags |= MOVEFILE_REPLACE_EXISTING;
            }

            if (::MoveFileExW(staging_wide.c_str(), final_wide.c_str(), move_flags) == 0)
            {
                const auto error_value = ::GetLastError();
                result.status = error_value == ERROR_ALREADY_EXISTS ||
                                        error_value == ERROR_FILE_EXISTS
                                    ? NodeIdentityStatusCode::kConflict
                                    : NodeIdentityStatusCode::kIoError;
                result.diagnostic = BuildWindowsDiagnostic(
                    "MoveFileExW", result.identity_path, error_value);
                CleanupStagingFile(staging_path);
                return result;
            }

            if (options.durability_mode == NodeIdentityDurabilityMode::kRequired)
            {
                result.status = NodeIdentityStatusCode::kDurabilityError;
                result.diagnostic =
                    "Windows directory durability is not implemented for node.identity; "
                    "required durability refuses silent success after MoveFileExW publish";
                result.created =
                    options.store_mode == NodeIdentityStoreMode::kCreateNewOnly;
                result.replaced =
                    options.store_mode ==
                    NodeIdentityStoreMode::kReplaceOnlyIfMatchesExpected;
                result.durable = false;
                return result;
            }

            result.status = NodeIdentityStatusCode::kOk;
            result.identity = identity;
            result.created =
                options.store_mode == NodeIdentityStoreMode::kCreateNewOnly;
            result.replaced =
                options.store_mode ==
                NodeIdentityStoreMode::kReplaceOnlyIfMatchesExpected;
            result.durable = false;
            result.diagnostic =
                "best-effort node.identity publish completed on Windows; "
                "file flush and MoveFileExW succeeded, directory durability not claimed";
            return result;
#else
            const int fd = ::open(staging_path.c_str(),
                                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                                  0644);
            if (fd < 0)
            {
                result.status = NodeIdentityStatusCode::kIoError;
                result.diagnostic = BuildIoDiagnostic("open", staging_path);
                return result;
            }

            auto close_fd = [&]() -> bool
            {
                if (::close(fd) != 0)
                {
                    result.status = NodeIdentityStatusCode::kIoError;
                    result.diagnostic = BuildIoDiagnostic("close", staging_path);
                    return false;
                }
                return true;
            };

            if (!WriteAllLinux(fd, payload, &result.diagnostic, staging_path))
            {
                result.status = NodeIdentityStatusCode::kIoError;
                close_fd();
                CleanupStagingFile(staging_path);
                return result;
            }

            if (::fsync(fd) != 0)
            {
                result.status = NodeIdentityStatusCode::kDurabilityError;
                result.diagnostic = BuildIoDiagnostic("fsync", staging_path);
                close_fd();
                CleanupStagingFile(staging_path);
                return result;
            }

            if (!close_fd())
            {
                CleanupStagingFile(staging_path);
                return result;
            }

            if (options.store_mode == NodeIdentityStoreMode::kCreateNewOnly)
            {
                if (::link(staging_path.c_str(), result.identity_path.c_str()) != 0)
                {
                    result.status = errno == EEXIST ? NodeIdentityStatusCode::kConflict
                                                    : NodeIdentityStatusCode::kIoError;
                    result.diagnostic = BuildIoDiagnostic("link", result.identity_path);
                    CleanupStagingFile(staging_path);
                    return result;
                }

                if (::unlink(staging_path.c_str()) != 0)
                {
                    result.status = NodeIdentityStatusCode::kIoError;
                    result.diagnostic = BuildIoDiagnostic("unlink", staging_path);
                    return result;
                }
            }
            else if (::rename(staging_path.c_str(), result.identity_path.c_str()) != 0)
            {
                result.status = NodeIdentityStatusCode::kIoError;
                result.diagnostic = BuildIoDiagnostic("rename", result.identity_path);
                CleanupStagingFile(staging_path);
                return result;
            }

            // Linux required durability 以 “临时文件 fsync -> 原子 publish ->
            // data_dir fsync” 为完成边界，避免崩溃后把半写 identity 误当成功。
            if (!SyncLinuxDirectory(options.data_dir, &result.diagnostic))
            {
                result.status = NodeIdentityStatusCode::kDurabilityError;
                return result;
            }

            result.status = NodeIdentityStatusCode::kOk;
            result.identity = identity;
            result.created =
                options.store_mode == NodeIdentityStoreMode::kCreateNewOnly;
            result.replaced =
                options.store_mode ==
                NodeIdentityStoreMode::kReplaceOnlyIfMatchesExpected;
            result.durable = true;
            result.diagnostic = "node.identity durable publish completed";
            return result;
#endif
        }
    } // namespace

    std::filesystem::path ResolveNodeIdentityPath(
        const std::filesystem::path &data_dir)
    {
        return data_dir / std::string(kNodeIdentityFileName);
    }

    NodeIdentityValidationResult ValidateNodeIdentity(
        const NodeIdentity &identity)
    {
        return ValidateNodeIdentityImpl(identity, true);
    }

    NodeIdentityValidationResult ValidateNodeIdentityMatches(
        const NodeIdentity &identity,
        const ExpectedNodeIdentity &expected)
    {
        return ValidateNodeIdentityMatchesDetailed(identity, expected);
    }

    NodeIdentityLoadResult LoadNodeIdentity(const NodeIdentityLoadOptions &options)
    {
        NodeIdentityLoadResult result;
        result.identity_path = ResolveNodeIdentityPath(options.data_dir);

        if (options.data_dir.empty())
        {
            result.status = NodeIdentityStatusCode::kInvalidArgument;
            result.diagnostic = "data_dir must not be empty";
            return result;
        }

        std::error_code type_ec;
        const bool data_dir_exists =
            std::filesystem::exists(options.data_dir, type_ec);
        if (type_ec)
        {
            result.status = NodeIdentityStatusCode::kIoError;
            result.diagnostic = BuildFilesystemDiagnostic(
                "exists", options.data_dir, type_ec);
            return result;
        }

        if (!data_dir_exists)
        {
            result.status = NodeIdentityStatusCode::kNotFound;
            AddIssue(&result.validation,
                     NodeIdentityIssueCode::kIdentityFileNotFound,
                     "node.identity",
                     "node.identity file does not exist under data_dir",
                     ClusterNodeType::kUnknown,
                     {},
                     std::nullopt,
                     result.identity_path);
            result.diagnostic = options.require_existing
                                    ? "required node.identity file is missing"
                                    : "node.identity file not found";
            return result;
        }

        if (!std::filesystem::is_directory(options.data_dir, type_ec))
        {
            result.status = NodeIdentityStatusCode::kInvalidArgument;
            result.diagnostic = "data_dir must reference an existing directory";
            return result;
        }

        const bool identity_exists =
            std::filesystem::exists(result.identity_path, type_ec);
        if (type_ec)
        {
            result.status = NodeIdentityStatusCode::kIoError;
            result.diagnostic = BuildFilesystemDiagnostic(
                "exists", result.identity_path, type_ec);
            return result;
        }

        if (!identity_exists)
        {
            result.status = NodeIdentityStatusCode::kNotFound;
            AddIssue(&result.validation,
                     NodeIdentityIssueCode::kIdentityFileNotFound,
                     "node.identity",
                     "node.identity file does not exist under data_dir",
                     ClusterNodeType::kUnknown,
                     {},
                     std::nullopt,
                     result.identity_path);
            result.diagnostic = options.require_existing
                                    ? "required node.identity file is missing"
                                    : "node.identity file not found";
            return result;
        }

        if (!std::filesystem::is_regular_file(result.identity_path, type_ec))
        {
            result.status = NodeIdentityStatusCode::kCorrupt;
            AddIssue(&result.validation,
                     NodeIdentityIssueCode::kIdentityFileCorrupt,
                     "node.identity",
                     "node.identity path exists but is not a regular file",
                     ClusterNodeType::kUnknown,
                     {},
                     std::nullopt,
                     result.identity_path);
            result.diagnostic = JoinIssues(result.validation);
            return result;
        }

        std::string content;
        if (!ReadTextFile(result.identity_path, &content, &result.diagnostic))
        {
            result.status = NodeIdentityStatusCode::kIoError;
            AddIssue(&result.validation,
                     NodeIdentityIssueCode::kIoFailure,
                     "node.identity",
                     result.diagnostic,
                     ClusterNodeType::kUnknown,
                     {},
                     std::nullopt,
                     result.identity_path);
            return result;
        }

        const auto parsed =
            ParseIdentityFile(content, &result.validation, &result.diagnostic);
        if (!parsed.has_value())
        {
            result.status = NodeIdentityStatusCode::kCorrupt;
            return result;
        }

        const auto loaded_identity = parsed->identity;
        auto identity_validation =
            ValidateNodeIdentityImpl(loaded_identity, false);
        if (!identity_validation.ok())
        {
            for (const auto &issue : identity_validation.issues)
            {
                result.validation.issues.push_back(issue);
            }
            result.status = LoadStatusFromValidation(identity_validation);
            result.diagnostic = JoinIssues(result.validation);
            return result;
        }

        // load 阶段要把 durable identity 的路径和 expected/actual 差异一起返回，
        // 避免调用方只看到 conflict 却无法定位是哪份 node.identity 被错误复用。
        auto match_validation = ValidateNodeIdentityMatchesDetailed(
            loaded_identity,
            options.expected,
            result.identity_path,
            "existing node.identity");
        if (!match_validation.ok())
        {
            result.validation = std::move(match_validation);
            result.status = MatchStatusFromValidation(result.validation);
            result.diagnostic = JoinIssues(result.validation);
            return result;
        }

        result.status = NodeIdentityStatusCode::kOk;
        result.identity = loaded_identity;
        result.diagnostic = "node.identity loaded successfully";
        return result;
    }

    NodeIdentityStoreResult StoreNodeIdentity(const NodeIdentity &identity,
                                              const NodeIdentityStoreOptions &options)
    {
        NodeIdentityStoreResult result;
        result.identity_path = ResolveNodeIdentityPath(options.data_dir);

        if (options.data_dir.empty())
        {
            result.status = NodeIdentityStatusCode::kInvalidArgument;
            result.diagnostic = "data_dir must not be empty";
            return result;
        }

        const auto normalized_identity = NormalizeNodeIdentity(identity);
        auto identity_validation = ValidateNodeIdentity(normalized_identity);
        if (!identity_validation.ok())
        {
            result.validation = std::move(identity_validation);
            result.status = StoreStatusFromValidation(result.validation);
            result.diagnostic = JoinIssues(result.validation);
            return result;
        }

        std::error_code exists_ec;
        const bool identity_exists =
            std::filesystem::exists(result.identity_path, exists_ec);
        if (exists_ec)
        {
            result.status = NodeIdentityStatusCode::kIoError;
            result.diagnostic = BuildFilesystemDiagnostic(
                "exists", result.identity_path, exists_ec);
            return result;
        }

        if (identity_exists)
        {
            const auto loaded = LoadNodeIdentity(NodeIdentityLoadOptions{
                .data_dir = options.data_dir,
                .expected = {},
                .require_existing = true});

            if (!loaded.ok())
            {
                result.status = loaded.status;
                result.validation = loaded.validation;
                result.diagnostic = loaded.diagnostic;
                return result;
            }

            if (options.store_mode == NodeIdentityStoreMode::kCreateNewOnly)
            {
                result.status = NodeIdentityStatusCode::kConflict;
                const ExpectedNodeIdentity requested_identity{
                    .cluster_id = identity.cluster_id,
                    .node_id = normalized_identity.node_id,
                    .node_type = normalized_identity.node_type,
                    .raft_id = normalized_identity.raft_id,
                    .membership_state = normalized_identity.membership_state,
                    .source = normalized_identity.source,
                    .require_raft_id_for_metadata = true,
                    .forbid_raft_id_for_non_metadata = true};
                // create-only 冲突不能只报“已存在”，还要返回请求身份和现存身份
                // 的具体差异，避免调用方误把错误 data_dir 当成可安全复用。
                auto mismatch_validation = ValidateNodeIdentityMatchesDetailed(
                    *loaded.identity,
                    requested_identity,
                    result.identity_path,
                    "existing node.identity");
                result.validation = std::move(mismatch_validation);
                AddIssue(&result.validation,
                         NodeIdentityIssueCode::kExistingIdentityConflict,
                         "node.identity",
                         "refusing to overwrite existing node.identity in create-only mode; "
                             "requested identity=" +
                             BuildIdentitySummary(normalized_identity) +
                             ", existing identity=" +
                             BuildIdentitySummary(*loaded.identity),
                         loaded.identity->node_type,
                         loaded.identity->node_id,
                         loaded.identity->raft_id,
                         result.identity_path);
                result.diagnostic = JoinIssues(result.validation);
                return result;
            }

            auto expected_validation = ValidateNodeIdentityMatchesDetailed(
                *loaded.identity,
                options.expected_existing,
                result.identity_path,
                "existing node.identity");
            if (!expected_validation.ok())
            {
                result.status = NodeIdentityStatusCode::kConflict;
                result.validation = std::move(expected_validation);
                result.diagnostic = JoinIssues(result.validation);
                return result;
            }

            if (!NodeIdentityEquals(*loaded.identity, normalized_identity))
            {
                result.status = NodeIdentityStatusCode::kConflict;
                const ExpectedNodeIdentity requested_identity{
                    .cluster_id = normalized_identity.cluster_id,
                    .node_id = normalized_identity.node_id,
                    .node_type = normalized_identity.node_type,
                    .raft_id = normalized_identity.raft_id,
                    .membership_state = normalized_identity.membership_state,
                    .source = normalized_identity.source,
                    .require_raft_id_for_metadata = true,
                    .forbid_raft_id_for_non_metadata = true};
                // replace-only 只允许重写同一身份文件；如果请求身份不同，必须显式
                // 返回 expected/actual 诊断，不能静默把其他节点身份覆盖进去。
                auto mismatch_validation = ValidateNodeIdentityMatchesDetailed(
                    *loaded.identity,
                    requested_identity,
                    result.identity_path,
                    "existing node.identity");
                result.validation = std::move(mismatch_validation);
                AddIssue(&result.validation,
                         NodeIdentityIssueCode::kExistingIdentityConflict,
                         "node.identity",
                         "replace mode only permits rewriting the same identity; "
                             "requested identity=" +
                             BuildIdentitySummary(normalized_identity) +
                             ", existing identity=" +
                             BuildIdentitySummary(*loaded.identity),
                         loaded.identity->node_type,
                         loaded.identity->node_id,
                         loaded.identity->raft_id,
                         result.identity_path);
                result.diagnostic = JoinIssues(result.validation);
                return result;
            }
        }
        else if (options.store_mode ==
                 NodeIdentityStoreMode::kReplaceOnlyIfMatchesExpected)
        {
            result.status = NodeIdentityStatusCode::kNotFound;
            AddIssue(&result.validation,
                     NodeIdentityIssueCode::kIdentityFileNotFound,
                     "node.identity",
                     "replace mode requires an existing node.identity file",
                     identity.node_type,
                     identity.node_id,
                     identity.raft_id,
                     result.identity_path);
            result.diagnostic = JoinIssues(result.validation);
            return result;
        }

        result = PublishIdentityFile(normalized_identity, options);
        return result;
    }

    NodeIdentityLoadOrCreateResult LoadOrCreateNodeIdentity(
        const NodeIdentityLoadOrCreateRequest &request)
    {
        NodeIdentityLoadOrCreateResult result;

        const auto loaded = LoadNodeIdentity(request.load_options);
        result.identity_path = loaded.identity_path;
        if (loaded.ok())
        {
            result.status = NodeIdentityStatusCode::kOk;
            result.identity = loaded.identity;
            result.validation = loaded.validation;
            result.loaded_existing = true;
            result.diagnostic = loaded.diagnostic;
            return result;
        }

        if (loaded.status != NodeIdentityStatusCode::kNotFound ||
            request.load_options.require_existing)
        {
            result.status = loaded.status;
            result.validation = loaded.validation;
            result.diagnostic = loaded.diagnostic;
            return result;
        }

        const auto stored =
            StoreNodeIdentity(request.identity_to_create, request.store_options);
        result.identity_path = stored.identity_path;
        if (stored.ok())
        {
            result.status = NodeIdentityStatusCode::kOk;
            result.identity = stored.identity;
            result.validation = stored.validation;
            result.created_new = true;
            result.durable = stored.durable;
            result.diagnostic = stored.diagnostic;
            return result;
        }

        // create-only 场景下若并发进程先写入，尝试再 load 一次，避免把正常复用误报成失败。
        if (stored.status == NodeIdentityStatusCode::kConflict)
        {
            const auto retry_loaded = LoadNodeIdentity(request.load_options);
            if (retry_loaded.ok())
            {
                result.status = NodeIdentityStatusCode::kOk;
                result.identity = retry_loaded.identity;
                result.validation = retry_loaded.validation;
                result.loaded_existing = true;
                result.diagnostic = retry_loaded.diagnostic;
                return result;
            }
        }

        result.status = stored.status;
        result.validation = stored.validation;
        result.durable = stored.durable;
        result.diagnostic = stored.diagnostic;
        return result;
    }

    const char *ToString(const NodeIdentitySource source)
    {
        switch (source)
        {
        case NodeIdentitySource::kConfigGenerator:
            return "config_generator";
        case NodeIdentitySource::kViewNodeAllocator:
            return "view_node_allocator";
        case NodeIdentitySource::kExplicitOverride:
            return "explicit_override";
        case NodeIdentitySource::kUnknown:
        default:
            return "unknown";
        }
    }

    const char *ToString(const NodeIdentityMembershipState state)
    {
        switch (state)
        {
        case NodeIdentityMembershipState::kUnknown:
            return "unknown";
        case NodeIdentityMembershipState::kNonRaft:
            return "non_raft";
        case NodeIdentityMembershipState::kJoining:
            return "joining";
        case NodeIdentityMembershipState::kCandidate:
            return "candidate";
        case NodeIdentityMembershipState::kLearner:
            return "learner";
        case NodeIdentityMembershipState::kVoter:
            return "voter";
        default:
            return "unknown";
        }
    }

    const char *ToString(const NodeIdentityStatusCode code)
    {
        switch (code)
        {
        case NodeIdentityStatusCode::kOk:
            return "ok";
        case NodeIdentityStatusCode::kNotFound:
            return "not_found";
        case NodeIdentityStatusCode::kInvalidArgument:
            return "invalid_argument";
        case NodeIdentityStatusCode::kConflict:
            return "conflict";
        case NodeIdentityStatusCode::kCorrupt:
            return "corrupt";
        case NodeIdentityStatusCode::kUnsupported:
            return "unsupported";
        case NodeIdentityStatusCode::kIoError:
            return "io_error";
        case NodeIdentityStatusCode::kDurabilityError:
            return "durability_error";
        case NodeIdentityStatusCode::kInternalError:
            return "internal_error";
        default:
            return "unknown";
        }
    }

    const char *ToString(const NodeIdentityIssueCode code)
    {
        switch (code)
        {
        case NodeIdentityIssueCode::kMissingClusterId:
            return "missing_cluster_id";
        case NodeIdentityIssueCode::kMissingNodeId:
            return "missing_node_id";
        case NodeIdentityIssueCode::kInvalidNodeType:
            return "invalid_node_type";
        case NodeIdentityIssueCode::kMissingRaftId:
            return "missing_raft_id";
        case NodeIdentityIssueCode::kUnexpectedRaftId:
            return "unexpected_raft_id";
        case NodeIdentityIssueCode::kInvalidIdentityVersion:
            return "invalid_identity_version";
        case NodeIdentityIssueCode::kUnsupportedIdentityVersion:
            return "unsupported_identity_version";
        case NodeIdentityIssueCode::kIdentityFileNotFound:
            return "identity_file_not_found";
        case NodeIdentityIssueCode::kIdentityFileCorrupt:
            return "identity_file_corrupt";
        case NodeIdentityIssueCode::kClusterIdMismatch:
            return "cluster_id_mismatch";
        case NodeIdentityIssueCode::kNodeTypeMismatch:
            return "node_type_mismatch";
        case NodeIdentityIssueCode::kNodeIdMismatch:
            return "node_id_mismatch";
        case NodeIdentityIssueCode::kRaftIdMismatch:
            return "raft_id_mismatch";
        case NodeIdentityIssueCode::kSourceMismatch:
            return "source_mismatch";
        case NodeIdentityIssueCode::kExistingIdentityConflict:
            return "existing_identity_conflict";
        case NodeIdentityIssueCode::kUnsupportedDurabilityMode:
            return "unsupported_durability_mode";
        case NodeIdentityIssueCode::kDurabilityPublishFailed:
            return "durability_publish_failed";
        case NodeIdentityIssueCode::kIoFailure:
            return "io_failure";
        case NodeIdentityIssueCode::kInvalidMembershipState:
            return "invalid_membership_state";
        case NodeIdentityIssueCode::kMembershipStateMismatch:
            return "membership_state_mismatch";
        case NodeIdentityIssueCode::kInvalidPersistentGeneration:
            return "invalid_persistent_generation";
        case NodeIdentityIssueCode::kUnknown:
        default:
            return "unknown";
        }
    }

    const char *ToString(const NodeIdentityDurabilityMode mode)
    {
        switch (mode)
        {
        case NodeIdentityDurabilityMode::kRequired:
            return "required";
        case NodeIdentityDurabilityMode::kBestEffortForTests:
            return "best_effort_for_tests";
        default:
            return "unknown";
        }
    }

    const char *ToString(const NodeIdentityStoreMode mode)
    {
        switch (mode)
        {
        case NodeIdentityStoreMode::kCreateNewOnly:
            return "create_new_only";
        case NodeIdentityStoreMode::kReplaceOnlyIfMatchesExpected:
            return "replace_only_if_matches_expected";
        default:
            return "unknown";
        }
    }

    std::string DescribeNodeIdentityIssue(const NodeIdentityIssue &issue)
    {
        std::ostringstream oss;
        oss << ToString(issue.code);
        if (!issue.field_path.empty())
        {
            oss << " field=" << issue.field_path;
        }
        if (!issue.message.empty())
        {
            oss << " message=" << issue.message;
        }
        if (issue.node_type != ClusterNodeType::kUnknown)
        {
            oss << " node_type=" << ToString(issue.node_type);
        }
        if (!issue.node_id.empty())
        {
            oss << " node_id=" << issue.node_id;
        }
        if (issue.raft_id.has_value())
        {
            oss << " raft_id=" << *issue.raft_id;
        }
        if (!issue.path.empty())
        {
            oss << " path=" << PathToDiagnosticString(issue.path);
        }
        return oss.str();
    }

} // namespace clusterdemo
