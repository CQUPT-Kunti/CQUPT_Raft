#include "cluster/cluster_config.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace clusterdemo
{
    namespace
    {
        constexpr std::string_view kViewNodeIdPrefix{"view-"};
        constexpr std::string_view kMetadataNodeIdPrefix{"meta-"};
        constexpr std::string_view kStorageNodeIdPrefix{"store-"};

        bool IsBlank(const std::string_view value)
        {
            for (const char ch : value)
            {
                if (!std::isspace(static_cast<unsigned char>(ch)))
                {
                    return false;
                }
            }

            return true;
        }

        bool IsValidNodeIdCharacter(const char ch)
        {
            return std::isalnum(static_cast<unsigned char>(ch)) != 0 ||
                   ch == '-' || ch == '_' || ch == '.';
        }

        bool IsValidNodeId(const std::string_view value)
        {
            if (value.empty() || IsBlank(value))
            {
                return false;
            }

            return std::all_of(value.begin(),
                               value.end(),
                               [](const char ch) {
                                   return IsValidNodeIdCharacter(ch);
                               });
        }

        std::string NormalizePathKey(const std::filesystem::path &path)
        {
            return path.lexically_normal().generic_string();
        }

        std::string MakeDefaultNodeId(const std::string_view prefix,
                                      const std::size_t ordinal)
        {
            return std::string(prefix) + std::to_string(ordinal);
        }

        std::filesystem::path MakeRoleDataDir(const std::filesystem::path &base_dir,
                                              const std::string_view role_dir,
                                              const std::string &node_id)
        {
            return base_dir / role_dir / node_id;
        }

        std::filesystem::path MakeMetadataDataDir(const std::filesystem::path &base_dir,
                                                  const std::string &node_id)
        {
            return base_dir / "metadata" / node_id / "data";
        }

        std::filesystem::path MakeMetadataSnapshotDir(
            const std::filesystem::path &base_dir,
            const std::string &node_id)
        {
            return base_dir / "metadata" / node_id / "snapshots";
        }

        std::string JoinMessages(const std::vector<std::string> &messages)
        {
            std::ostringstream oss;
            for (std::size_t index = 0; index < messages.size(); ++index)
            {
                if (index != 0)
                {
                    oss << "; ";
                }
                oss << messages[index];
            }
            return oss.str();
        }

        std::string EscapeJsonString(const std::string_view value)
        {
            std::string escaped;
            escaped.reserve(value.size());
            for (const char ch : value)
            {
                switch (ch)
                {
                case '\\':
                    escaped += "\\\\";
                    break;
                case '"':
                    escaped += "\\\"";
                    break;
                case '\b':
                    escaped += "\\b";
                    break;
                case '\f':
                    escaped += "\\f";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                default:
                    escaped.push_back(ch);
                    break;
                }
            }
            return escaped;
        }

        std::string JsonString(const std::string_view value)
        {
            return "\"" + EscapeJsonString(value) + "\"";
        }

        std::string JsonPath(const std::filesystem::path &path)
        {
            return JsonString(path.string());
        }

        std::uint64_t ToMillis(const std::chrono::milliseconds value)
        {
            return value.count() > 0
                       ? static_cast<std::uint64_t>(value.count())
                       : 0ULL;
        }

        void AppendIssue(ClusterConfigValidationResult *result,
                         const ClusterConfigIssueCode code,
                         std::string field_path,
                         std::string message,
                         const ClusterNodeType node_type = ClusterNodeType::kUnknown,
                         ClusterNodeId node_id = {},
                         std::string endpoint = {},
                         std::filesystem::path path = {})
        {
            if (result == nullptr)
            {
                return;
            }

            result->issues.push_back(ClusterConfigValidationIssue{
                .code = code,
                .field_path = std::move(field_path),
                .message = std::move(message),
                .node_type = node_type,
                .node_id = std::move(node_id),
                .endpoint = std::move(endpoint),
                .path = std::move(path),
            });
        }

        bool ParseEndpoint(const std::string_view endpoint,
                           std::string *host_out,
                           std::uint16_t *port_out)
        {
            if (endpoint.empty() || IsBlank(endpoint))
            {
                return false;
            }

            if (std::any_of(endpoint.begin(),
                            endpoint.end(),
                            [](const char ch) {
                                return std::isspace(static_cast<unsigned char>(ch)) != 0;
                            }))
            {
                return false;
            }

            const std::size_t separator = endpoint.rfind(':');
            if (separator == std::string_view::npos || separator == 0 ||
                separator + 1 >= endpoint.size())
            {
                return false;
            }

            const std::string_view host = endpoint.substr(0, separator);
            const std::string_view port_text = endpoint.substr(separator + 1);
            if (host.empty() || IsBlank(host))
            {
                return false;
            }
            if (port_text.empty())
            {
                return false;
            }
            if (!std::all_of(port_text.begin(),
                             port_text.end(),
                             [](const char ch) {
                                 return std::isdigit(static_cast<unsigned char>(ch)) != 0;
                             }))
            {
                return false;
            }

            unsigned long port_value = 0;
            try
            {
                port_value = std::stoul(std::string(port_text));
            }
            catch (...)
            {
                return false;
            }

            if (port_value == 0 ||
                port_value > std::numeric_limits<std::uint16_t>::max())
            {
                return false;
            }

            if (host_out != nullptr)
            {
                *host_out = std::string(host);
            }
            if (port_out != nullptr)
            {
                *port_out = static_cast<std::uint16_t>(port_value);
            }
            return true;
        }

        template <typename T>
        void RegisterUniqueValue(ClusterConfigValidationResult *result,
                                 std::unordered_set<T> *seen_values,
                                 const T &value,
                                 const ClusterConfigIssueCode duplicate_code,
                                 std::string field_path,
                                 std::string message,
                                 const ClusterNodeType node_type,
                                 ClusterNodeId node_id = {},
                                 std::string endpoint = {},
                                 std::filesystem::path path = {})
        {
            if (seen_values == nullptr)
            {
                return;
            }

            if (!seen_values->insert(value).second)
            {
                AppendIssue(result,
                            duplicate_code,
                            std::move(field_path),
                            std::move(message),
                            node_type,
                            std::move(node_id),
                            std::move(endpoint),
                            std::move(path));
            }
        }

        bool HasConflictIssue(const ClusterConfigValidationResult &validation)
        {
            return std::any_of(
                validation.issues.begin(),
                validation.issues.end(),
                [](const ClusterConfigValidationIssue &issue) {
                    switch (issue.code)
                    {
                    case ClusterConfigIssueCode::kDuplicateNodeId:
                    case ClusterConfigIssueCode::kDuplicateEndpoint:
                    case ClusterConfigIssueCode::kSharedDataDir:
                    case ClusterConfigIssueCode::kDuplicateRaftId:
                    case ClusterConfigIssueCode::kIdentityConfigMismatch:
                        return true;
                    default:
                        return false;
                    }
                });
        }

        ClusterConfigStatusCode DeriveGenerationStatus(
            const ClusterConfigValidationResult &validation)
        {
            if (validation.ok())
            {
                return ClusterConfigStatusCode::kOk;
            }
            if (std::any_of(
                    validation.issues.begin(),
                    validation.issues.end(),
                    [](const ClusterConfigValidationIssue &issue) {
                        return issue.code ==
                               ClusterConfigIssueCode::kUnsupportedDurabilityMode;
                    }))
            {
                return ClusterConfigStatusCode::kUnsupported;
            }
            return HasConflictIssue(validation) ? ClusterConfigStatusCode::kConflict
                                                : ClusterConfigStatusCode::kInvalidArgument;
        }

        void ValidateChunkPolicy(const ChunkPolicyConfig &policy,
                                 ClusterConfigValidationResult *result)
        {
            if (policy.chunk_size_bytes == 0)
            {
                AppendIssue(result,
                            ClusterConfigIssueCode::kInvalidChunkPolicy,
                            "chunk_policy.chunk_size_bytes",
                            "chunk_size_bytes must be > 0");
            }
            if (policy.replica_count == 0)
            {
                AppendIssue(result,
                            ClusterConfigIssueCode::kInvalidChunkPolicy,
                            "chunk_policy.replica_count",
                            "replica_count must be > 0");
            }
            if (policy.minimum_successful_writes == 0)
            {
                AppendIssue(result,
                            ClusterConfigIssueCode::kInvalidChunkPolicy,
                            "chunk_policy.minimum_successful_writes",
                            "minimum_successful_writes must be > 0");
            }
            if (policy.replica_count != 0 &&
                policy.minimum_successful_writes > policy.replica_count)
            {
                AppendIssue(result,
                            ClusterConfigIssueCode::kInvalidChunkPolicy,
                            "chunk_policy.minimum_successful_writes",
                            "minimum_successful_writes must not exceed replica_count");
            }
            if (policy.checksum_algorithm == ClusterChecksumAlgorithm::kUnknown)
            {
                AppendIssue(result,
                            ClusterConfigIssueCode::kInvalidChunkPolicy,
                            "chunk_policy.checksum_algorithm",
                            "checksum_algorithm must be specified");
            }
        }

        void ValidateTimeout(const std::chrono::milliseconds value,
                             const std::string_view field_path,
                             ClusterConfigValidationResult *result)
        {
            if (value.count() <= 0)
            {
                AppendIssue(result,
                            ClusterConfigIssueCode::kInvalidTimeoutPolicy,
                            std::string(field_path),
                            "timeout must be > 0ms");
            }
        }

        void ValidateTimeoutPolicy(const ClusterTimeoutConfig &timeouts,
                                   ClusterConfigValidationResult *result)
        {
            ValidateTimeout(timeouts.discovery_rpc_timeout,
                            "timeouts.discovery_rpc_timeout",
                            result);
            ValidateTimeout(timeouts.metadata_rpc_timeout,
                            "timeouts.metadata_rpc_timeout",
                            result);
            ValidateTimeout(timeouts.storage_rpc_timeout,
                            "timeouts.storage_rpc_timeout",
                            result);
            ValidateTimeout(timeouts.heartbeat_interval,
                            "timeouts.heartbeat_interval",
                            result);
            ValidateTimeout(timeouts.registration_timeout,
                            "timeouts.registration_timeout",
                            result);
            ValidateTimeout(timeouts.commit_deadline,
                            "timeouts.commit_deadline",
                            result);
            ValidateTimeout(timeouts.liveness_stale_timeout,
                            "timeouts.liveness_stale_timeout",
                            result);
            ValidateTimeout(timeouts.liveness_dead_timeout,
                            "timeouts.liveness_dead_timeout",
                            result);

            if (timeouts.heartbeat_interval.count() > 0 &&
                timeouts.liveness_stale_timeout <= timeouts.heartbeat_interval)
            {
                AppendIssue(result,
                            ClusterConfigIssueCode::kInvalidTimeoutPolicy,
                            "timeouts.liveness_stale_timeout",
                            "liveness_stale_timeout must be greater than heartbeat_interval");
            }
            if (timeouts.liveness_stale_timeout.count() > 0 &&
                timeouts.liveness_dead_timeout <= timeouts.liveness_stale_timeout)
            {
                AppendIssue(result,
                            ClusterConfigIssueCode::kInvalidTimeoutPolicy,
                            "timeouts.liveness_dead_timeout",
                            "liveness_dead_timeout must be greater than liveness_stale_timeout");
            }
        }

        bool ValidatePortRange(const std::uint16_t port_base,
                               const std::size_t node_count,
                               const std::string_view field_name,
                               std::vector<std::string> *errors)
        {
            if (errors == nullptr)
            {
                return false;
            }

            if (port_base == 0)
            {
                errors->push_back(std::string(field_name) + " must be > 0");
                return false;
            }
            if (node_count == 0)
            {
                return true;
            }

            const std::size_t last_port =
                static_cast<std::size_t>(port_base) + node_count - 1;
            if (last_port > std::numeric_limits<std::uint16_t>::max())
            {
                errors->push_back(std::string(field_name) +
                                  " range exceeds 65535");
                return false;
            }
            return true;
        }

        std::string MakeEndpoint(const std::string &host, const std::uint16_t port)
        {
            return host + ":" + std::to_string(port);
        }

        std::vector<std::int32_t> CollectFixedPositiveRaftIds(
            const std::vector<std::int32_t> &fixed_raft_ids)
        {
            std::vector<std::int32_t> values;
            values.reserve(fixed_raft_ids.size());
            for (const std::int32_t raft_id : fixed_raft_ids)
            {
                if (raft_id > 0)
                {
                    values.push_back(raft_id);
                }
            }
            return values;
        }

        std::int32_t NextGeneratedRaftId(
            const std::unordered_set<std::int32_t> &already_reserved,
            std::int32_t *cursor)
        {
            if (cursor == nullptr)
            {
                return 0;
            }

            while (already_reserved.find(*cursor) != already_reserved.end())
            {
                ++(*cursor);
            }

            const std::int32_t selected = *cursor;
            ++(*cursor);
            return selected;
        }

        std::string BuildValidationErrorDetail(
            const ClusterConfigValidationResult &validation)
        {
            std::vector<std::string> descriptions;
            descriptions.reserve(validation.issues.size());
            for (const ClusterConfigValidationIssue &issue : validation.issues)
            {
                descriptions.push_back(DescribeClusterConfigIssue(issue));
            }
            return JoinMessages(descriptions);
        }

        std::string AllocationFieldPath(const ClusterNodeType node_type,
                                        const std::size_t ordinal)
        {
            switch (node_type)
            {
            case ClusterNodeType::kView:
                return "view_nodes[" + std::to_string(ordinal) + "].endpoint";
            case ClusterNodeType::kMetadata:
                return "metadata_nodes[" + std::to_string(ordinal) + "].endpoint";
            case ClusterNodeType::kStorage:
                return "storage_nodes[" + std::to_string(ordinal) + "].endpoint";
            case ClusterNodeType::kUnknown:
            default:
                return "endpoint";
            }
        }

        ClusterNodeId ResolveRequestedNodeId(const ClusterNodeType node_type,
                                             const ClusterConfigGenerationRequest &request,
                                             const std::size_t ordinal)
        {
            switch (node_type)
            {
            case ClusterNodeType::kView:
                return ordinal < request.fixed_view_node_ids.size()
                           ? request.fixed_view_node_ids[ordinal]
                           : MakeDefaultNodeId(kViewNodeIdPrefix, ordinal + 1);
            case ClusterNodeType::kMetadata:
                return ordinal < request.fixed_metadata_node_ids.size()
                           ? request.fixed_metadata_node_ids[ordinal]
                           : MakeDefaultNodeId(kMetadataNodeIdPrefix, ordinal + 1);
            case ClusterNodeType::kStorage:
                return ordinal < request.fixed_storage_node_ids.size()
                           ? request.fixed_storage_node_ids[ordinal]
                           : MakeDefaultNodeId(kStorageNodeIdPrefix, ordinal + 1);
            case ClusterNodeType::kUnknown:
            default:
                return {};
            }
        }

        const ClusterEndpointAssignment *FindAssignment(
            const std::vector<ClusterEndpointAssignment> &assignments,
            const ClusterNodeType node_type,
            const std::size_t ordinal)
        {
            for (const ClusterEndpointAssignment &assignment : assignments)
            {
                if (assignment.node_type == node_type &&
                    assignment.ordinal == ordinal)
                {
                    return &assignment;
                }
            }
            return nullptr;
        }

        std::vector<std::string> ValidateGenerationRequest(
            const ClusterConfigGenerationRequest &request)
        {
            std::vector<std::string> request_errors;
            if (request.cluster_id.empty() || IsBlank(request.cluster_id))
            {
                request_errors.push_back("cluster_id must not be empty");
            }
            if (request.base_dir.empty())
            {
                request_errors.push_back("base_dir must not be empty");
            }
            if (request.bind_host.empty() || IsBlank(request.bind_host))
            {
                request_errors.push_back("bind_host must not be empty");
            }
            if (!request.advertise_host.empty() &&
                request.advertise_host != request.bind_host)
            {
                // 当前 ClusterConfig 只有单一 endpoint 字段，不能静默分裂监听地址和对外地址。
                request_errors.push_back(
                    "advertise_host must be empty or equal to bind_host until the config model exposes separate bind/advertise endpoints");
            }
            if (request.view_node_count == 0)
            {
                request_errors.push_back("view_node_count must be > 0");
            }
            if (request.metadata_node_count == 0)
            {
                request_errors.push_back("metadata_node_count must be > 0");
            }
            if (request.storage_node_count == 0)
            {
                request_errors.push_back("storage_node_count must be > 0");
            }
            if (request.metadata_voter_count == 0)
            {
                request_errors.push_back("metadata_voter_count must be > 0");
            }
            else if (request.metadata_voter_count > request.metadata_node_count)
            {
                request_errors.push_back(
                    "metadata_voter_count must not exceed metadata_node_count");
            }
            else if ((request.metadata_voter_count % 2U) == 0U)
            {
                request_errors.push_back(
                    "metadata_voter_count must be odd to support 1/3/5/7 voter layouts");
            }
            if (request.fixed_view_node_ids.size() > request.view_node_count)
            {
                request_errors.push_back(
                    "fixed_view_node_ids size must not exceed view_node_count");
            }
            if (request.fixed_metadata_node_ids.size() > request.metadata_node_count)
            {
                request_errors.push_back(
                    "fixed_metadata_node_ids size must not exceed metadata_node_count");
            }
            if (request.fixed_metadata_raft_ids.size() > request.metadata_node_count)
            {
                request_errors.push_back(
                    "fixed_metadata_raft_ids size must not exceed metadata_node_count");
            }
            if (request.fixed_storage_node_ids.size() > request.storage_node_count)
            {
                request_errors.push_back(
                    "fixed_storage_node_ids size must not exceed storage_node_count");
            }
            if (request.storage_capacity_overrides_bytes.size() >
                request.storage_node_count)
            {
                request_errors.push_back(
                    "storage_capacity_overrides_bytes size must not exceed storage_node_count");
            }
            if (request.generation_seed.has_value() && *request.generation_seed == 0)
            {
                request_errors.push_back(
                    "generation_seed must be > 0 when provided");
            }

            ValidatePortRange(request.view_port_base,
                              request.view_node_count,
                              "view_port_base",
                              &request_errors);
            ValidatePortRange(request.metadata_port_base,
                              request.metadata_node_count,
                              "metadata_port_base",
                              &request_errors);
            ValidatePortRange(request.storage_port_base,
                              request.storage_node_count,
                              "storage_port_base",
                              &request_errors);
            return request_errors;
        }

        ClusterConfigValidationResult ValidateInitialQuorumMembershipOnly(
            const InitialRaftMembershipConfig &membership)
        {
            ClusterConfigValidationResult validation;
            if (membership.voter_raft_ids.empty())
            {
                AppendIssue(&validation,
                            ClusterConfigIssueCode::kInvalidRaftVoterCount,
                            "initial_raft_membership.voter_raft_ids",
                            "at least one voter raft_id must be configured");
                return validation;
            }

            if ((membership.voter_raft_ids.size() % 2U) == 0U)
            {
                AppendIssue(&validation,
                            ClusterConfigIssueCode::kInvalidRaftVoterCount,
                            "initial_raft_membership.voter_raft_ids",
                            "voter count must be odd to support 1/3/5/7 style quorum layouts");
            }

            std::unordered_set<std::int32_t> seen_voters;
            std::unordered_set<std::int32_t> seen_learners;

            for (std::size_t index = 0; index < membership.voter_raft_ids.size(); ++index)
            {
                const std::int32_t raft_id = membership.voter_raft_ids[index];
                const std::string field_path =
                    "initial_raft_membership.voter_raft_ids[" + std::to_string(index) + "]";
                if (raft_id <= 0)
                {
                    AppendIssue(&validation,
                                ClusterConfigIssueCode::kInvalidRaftId,
                                field_path,
                                "voter raft_id must be > 0");
                    continue;
                }
                if (!seen_voters.insert(raft_id).second)
                {
                    AppendIssue(&validation,
                                ClusterConfigIssueCode::kInvalidInitialMembership,
                                field_path,
                                "voter raft_id must not be duplicated");
                }
            }

            for (std::size_t index = 0; index < membership.learner_raft_ids.size(); ++index)
            {
                const std::int32_t raft_id = membership.learner_raft_ids[index];
                const std::string field_path =
                    "initial_raft_membership.learner_raft_ids[" + std::to_string(index) + "]";
                if (raft_id <= 0)
                {
                    AppendIssue(&validation,
                                ClusterConfigIssueCode::kInvalidRaftId,
                                field_path,
                                "learner raft_id must be > 0");
                    continue;
                }
                if (!seen_learners.insert(raft_id).second)
                {
                    AppendIssue(&validation,
                                ClusterConfigIssueCode::kInvalidInitialMembership,
                                field_path,
                                "learner raft_id must not be duplicated");
                }
                if (seen_voters.find(raft_id) != seen_voters.end())
                {
                    AppendIssue(&validation,
                                ClusterConfigIssueCode::kInvalidInitialMembership,
                                field_path,
                                "raft_id must not appear in both voter and learner membership");
                }
            }

            return validation;
        }
    } // namespace

    ClusterConfigValidationResult ValidateClusterConfig(const ClusterConfig &config)
    {
        ClusterConfigValidationResult result;

        if (config.cluster_id.empty() || IsBlank(config.cluster_id))
        {
            AppendIssue(&result,
                        ClusterConfigIssueCode::kMissingClusterId,
                        "cluster_id",
                        "cluster_id must not be empty");
        }
        if (config.base_dir.empty())
        {
            AppendIssue(&result,
                        ClusterConfigIssueCode::kMissingDataDir,
                        "base_dir",
                        "base_dir must not be empty");
        }

        if (config.view_nodes.empty())
        {
            AppendIssue(&result,
                        ClusterConfigIssueCode::kInvalidNodeCount,
                        "view_nodes",
                        "at least one ViewNode must be configured");
        }
        if (config.metadata_nodes.empty())
        {
            AppendIssue(&result,
                        ClusterConfigIssueCode::kInvalidNodeCount,
                        "metadata_nodes",
                        "at least one MetadataNode must be configured");
        }
        if (config.storage_nodes.empty())
        {
            AppendIssue(&result,
                        ClusterConfigIssueCode::kInvalidNodeCount,
                        "storage_nodes",
                        "at least one StorageNode must be configured");
        }

        std::unordered_set<std::string> seen_node_ids;
        std::unordered_set<std::string> seen_endpoints;
        std::unordered_set<std::string> seen_paths;
        std::unordered_set<std::int32_t> seen_raft_ids;

        for (std::size_t index = 0; index < config.view_nodes.size(); ++index)
        {
            const ViewNodeConfig &node = config.view_nodes[index];
            const std::string field_prefix =
                "view_nodes[" + std::to_string(index) + "]";

            if (node.node_id.has_value())
            {
                if (!IsValidNodeId(*node.node_id))
                {
                    AppendIssue(&result,
                                ClusterConfigIssueCode::kInvalidNodeId,
                                field_prefix + ".node_id",
                                "ViewNode node_id contains unsupported characters or is blank",
                                ClusterNodeType::kView,
                                node.node_id.value_or(""));
                }
                else
                {
                    RegisterUniqueValue(&result,
                                        &seen_node_ids,
                                        *node.node_id,
                                        ClusterConfigIssueCode::kDuplicateNodeId,
                                        field_prefix + ".node_id",
                                        "duplicate node_id across cluster config",
                                        ClusterNodeType::kView,
                                        *node.node_id);
                }
            }

            if (!ParseEndpoint(node.endpoint, nullptr, nullptr))
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kInvalidEndpoint,
                            field_prefix + ".endpoint",
                            "endpoint must use host:port format with port in 1..65535",
                            ClusterNodeType::kView,
                            node.node_id.value_or(""),
                            node.endpoint);
            }
            else
            {
                RegisterUniqueValue(&result,
                                    &seen_endpoints,
                                    node.endpoint,
                                    ClusterConfigIssueCode::kDuplicateEndpoint,
                                    field_prefix + ".endpoint",
                                    "duplicate endpoint across cluster config",
                                    ClusterNodeType::kView,
                                    node.node_id.value_or(""),
                                    node.endpoint);
            }

            if (node.data_dir.empty())
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kMissingDataDir,
                            field_prefix + ".data_dir",
                            "data_dir must not be empty",
                            ClusterNodeType::kView,
                            node.node_id.value_or(""),
                            node.endpoint);
            }
            else
            {
                RegisterUniqueValue(&result,
                                    &seen_paths,
                                    NormalizePathKey(node.data_dir),
                                    ClusterConfigIssueCode::kSharedDataDir,
                                    field_prefix + ".data_dir",
                                    "data_dir must not be shared by multiple nodes",
                                    ClusterNodeType::kView,
                                    node.node_id.value_or(""),
                                    node.endpoint,
                                    node.data_dir);
            }
        }

        for (std::size_t index = 0; index < config.metadata_nodes.size(); ++index)
        {
            const MetadataNodeConfig &node = config.metadata_nodes[index];
            const std::string field_prefix =
                "metadata_nodes[" + std::to_string(index) + "]";

            if (!IsValidNodeId(node.node_id))
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kInvalidNodeId,
                            field_prefix + ".node_id",
                            "MetadataNode node_id contains unsupported characters or is blank",
                            ClusterNodeType::kMetadata,
                            node.node_id);
            }
            else
            {
                RegisterUniqueValue(&result,
                                    &seen_node_ids,
                                    node.node_id,
                                    ClusterConfigIssueCode::kDuplicateNodeId,
                                    field_prefix + ".node_id",
                                    "duplicate node_id across cluster config",
                                    ClusterNodeType::kMetadata,
                                    node.node_id);
            }

            if (node.raft_id <= 0)
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kInvalidRaftId,
                            field_prefix + ".raft_id",
                            "raft_id must be > 0",
                            ClusterNodeType::kMetadata,
                            node.node_id,
                            node.endpoint);
            }
            else
            {
                RegisterUniqueValue(&result,
                                    &seen_raft_ids,
                                    node.raft_id,
                                    ClusterConfigIssueCode::kDuplicateRaftId,
                                    field_prefix + ".raft_id",
                                    "duplicate raft_id across metadata nodes",
                                    ClusterNodeType::kMetadata,
                                    node.node_id,
                                    node.endpoint);
            }

            if (!ParseEndpoint(node.endpoint, nullptr, nullptr))
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kInvalidEndpoint,
                            field_prefix + ".endpoint",
                            "endpoint must use host:port format with port in 1..65535",
                            ClusterNodeType::kMetadata,
                            node.node_id,
                            node.endpoint);
            }
            else
            {
                RegisterUniqueValue(&result,
                                    &seen_endpoints,
                                    node.endpoint,
                                    ClusterConfigIssueCode::kDuplicateEndpoint,
                                    field_prefix + ".endpoint",
                                    "duplicate endpoint across cluster config",
                                    ClusterNodeType::kMetadata,
                                    node.node_id,
                                    node.endpoint);
            }

            if (node.data_dir.empty())
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kMissingDataDir,
                            field_prefix + ".data_dir",
                            "data_dir must not be empty",
                            ClusterNodeType::kMetadata,
                            node.node_id,
                            node.endpoint);
            }
            else
            {
                RegisterUniqueValue(&result,
                                    &seen_paths,
                                    NormalizePathKey(node.data_dir),
                                    ClusterConfigIssueCode::kSharedDataDir,
                                    field_prefix + ".data_dir",
                                    "data_dir must not be shared by multiple nodes",
                                    ClusterNodeType::kMetadata,
                                    node.node_id,
                                    node.endpoint,
                                    node.data_dir);
            }

            if (node.snapshot_dir.empty())
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kMissingSnapshotDir,
                            field_prefix + ".snapshot_dir",
                            "snapshot_dir must not be empty",
                            ClusterNodeType::kMetadata,
                            node.node_id,
                            node.endpoint);
            }
            else
            {
                RegisterUniqueValue(&result,
                                    &seen_paths,
                                    NormalizePathKey(node.snapshot_dir),
                                    ClusterConfigIssueCode::kSharedDataDir,
                                    field_prefix + ".snapshot_dir",
                                    "snapshot_dir must not overlap any node data/snapshot dir",
                                    ClusterNodeType::kMetadata,
                                    node.node_id,
                                    node.endpoint,
                                    node.snapshot_dir);
            }

            if (node.initial_role == MetadataNodeInitialRole::kUnknown)
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kInvalidInitialMembership,
                            field_prefix + ".initial_role",
                            "initial_role must be voter or learner",
                            ClusterNodeType::kMetadata,
                            node.node_id,
                            node.endpoint);
            }
        }

        for (std::size_t index = 0; index < config.storage_nodes.size(); ++index)
        {
            const StorageNodeConfig &node = config.storage_nodes[index];
            const std::string field_prefix =
                "storage_nodes[" + std::to_string(index) + "]";

            if (node.node_id.has_value())
            {
                if (!IsValidNodeId(*node.node_id))
                {
                    AppendIssue(&result,
                                ClusterConfigIssueCode::kInvalidNodeId,
                                field_prefix + ".node_id",
                                "StorageNode node_id contains unsupported characters or is blank",
                                ClusterNodeType::kStorage,
                                node.node_id.value_or(""));
                }
                else
                {
                    RegisterUniqueValue(&result,
                                        &seen_node_ids,
                                        *node.node_id,
                                        ClusterConfigIssueCode::kDuplicateNodeId,
                                        field_prefix + ".node_id",
                                        "duplicate node_id across cluster config",
                                        ClusterNodeType::kStorage,
                                        *node.node_id);
                }
            }

            if (!ParseEndpoint(node.endpoint, nullptr, nullptr))
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kInvalidEndpoint,
                            field_prefix + ".endpoint",
                            "endpoint must use host:port format with port in 1..65535",
                            ClusterNodeType::kStorage,
                            node.node_id.value_or(""),
                            node.endpoint);
            }
            else
            {
                RegisterUniqueValue(&result,
                                    &seen_endpoints,
                                    node.endpoint,
                                    ClusterConfigIssueCode::kDuplicateEndpoint,
                                    field_prefix + ".endpoint",
                                    "duplicate endpoint across cluster config",
                                    ClusterNodeType::kStorage,
                                    node.node_id.value_or(""),
                                    node.endpoint);
            }

            if (node.data_dir.empty())
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kMissingDataDir,
                            field_prefix + ".data_dir",
                            "data_dir must not be empty",
                            ClusterNodeType::kStorage,
                            node.node_id.value_or(""),
                            node.endpoint);
            }
            else
            {
                RegisterUniqueValue(&result,
                                    &seen_paths,
                                    NormalizePathKey(node.data_dir),
                                    ClusterConfigIssueCode::kSharedDataDir,
                                    field_prefix + ".data_dir",
                                    "data_dir must not be shared by multiple nodes",
                                    ClusterNodeType::kStorage,
                                    node.node_id.value_or(""),
                                    node.endpoint,
                                    node.data_dir);
            }

            if (node.capacity_bytes == 0)
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kInvalidCapacity,
                            field_prefix + ".capacity_bytes",
                            "capacity_bytes must be > 0",
                            ClusterNodeType::kStorage,
                            node.node_id.value_or(""),
                            node.endpoint);
            }
        }

        ValidateChunkPolicy(config.chunk_policy, &result);
        ValidateTimeoutPolicy(config.timeouts, &result);

        const ClusterConfigValidationResult membership_validation =
            ValidateInitialRaftMembership(config);
        result.issues.insert(result.issues.end(),
                             membership_validation.issues.begin(),
                             membership_validation.issues.end());

        return result;
    }

    ClusterConfigValidationResult ValidateInitialRaftMembership(
        const ClusterConfig &config)
    {
        ClusterConfigValidationResult result;

        const InitialRaftMembershipConfig &membership =
            config.initial_raft_membership;
        if (membership.membership_epoch == 0)
        {
            AppendIssue(&result,
                        ClusterConfigIssueCode::kInvalidInitialMembership,
                        "initial_raft_membership.membership_epoch",
                        "membership_epoch must be > 0");
        }

        if (membership.voter_raft_ids.empty())
        {
            AppendIssue(&result,
                        ClusterConfigIssueCode::kInvalidRaftVoterCount,
                        "initial_raft_membership.voter_raft_ids",
                        "at least one voter raft_id must be configured");
        }
        else if ((membership.voter_raft_ids.size() % 2U) == 0U)
        {
            AppendIssue(&result,
                        ClusterConfigIssueCode::kInvalidRaftVoterCount,
                        "initial_raft_membership.voter_raft_ids",
                        "voter count must be odd to support 1/3/5/7 style quorum layouts");
        }

        std::unordered_set<std::int32_t> metadata_raft_ids;
        std::unordered_set<std::int32_t> voter_ids;
        std::unordered_set<std::int32_t> learner_ids;

        for (const MetadataNodeConfig &node : config.metadata_nodes)
        {
            if (node.raft_id > 0)
            {
                metadata_raft_ids.insert(node.raft_id);
            }
        }

        for (std::size_t index = 0; index < membership.voter_raft_ids.size(); ++index)
        {
            const std::int32_t raft_id = membership.voter_raft_ids[index];
            const std::string field_path =
                "initial_raft_membership.voter_raft_ids[" + std::to_string(index) + "]";

            if (raft_id <= 0)
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kInvalidRaftId,
                            field_path,
                            "voter raft_id must be > 0");
                continue;
            }
            if (!voter_ids.insert(raft_id).second)
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kInvalidInitialMembership,
                            field_path,
                            "voter raft_id must not be duplicated");
            }
            if (learner_ids.find(raft_id) != learner_ids.end())
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kInvalidInitialMembership,
                            field_path,
                            "raft_id must not appear in both voter and learner membership");
            }
            if (metadata_raft_ids.find(raft_id) == metadata_raft_ids.end())
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kInvalidInitialMembership,
                            field_path,
                            "voter raft_id does not belong to any configured MetadataNode");
            }
        }

        for (std::size_t index = 0; index < membership.learner_raft_ids.size(); ++index)
        {
            const std::int32_t raft_id = membership.learner_raft_ids[index];
            const std::string field_path =
                "initial_raft_membership.learner_raft_ids[" + std::to_string(index) + "]";

            if (raft_id <= 0)
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kInvalidRaftId,
                            field_path,
                            "learner raft_id must be > 0");
                continue;
            }
            if (!learner_ids.insert(raft_id).second)
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kInvalidInitialMembership,
                            field_path,
                            "learner raft_id must not be duplicated");
            }
            if (voter_ids.find(raft_id) != voter_ids.end())
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kInvalidInitialMembership,
                            field_path,
                            "raft_id must not appear in both voter and learner membership");
            }
            if (metadata_raft_ids.find(raft_id) == metadata_raft_ids.end())
            {
                AppendIssue(&result,
                            ClusterConfigIssueCode::kInvalidInitialMembership,
                            field_path,
                            "learner raft_id does not belong to any configured MetadataNode");
            }
        }

        for (std::size_t index = 0; index < config.metadata_nodes.size(); ++index)
        {
            const MetadataNodeConfig &node = config.metadata_nodes[index];
            const std::string field_prefix =
                "metadata_nodes[" + std::to_string(index) + "]";

            if (node.initial_role == MetadataNodeInitialRole::kVoter)
            {
                if (node.raft_id > 0 &&
                    voter_ids.find(node.raft_id) == voter_ids.end())
                {
                    AppendIssue(&result,
                                ClusterConfigIssueCode::kInvalidInitialMembership,
                                field_prefix + ".initial_role",
                                "MetadataNode configured as voter must appear in voter_raft_ids",
                                ClusterNodeType::kMetadata,
                                node.node_id,
                                node.endpoint);
                }
            }
            else if (node.initial_role == MetadataNodeInitialRole::kLearner)
            {
                if (node.raft_id > 0 &&
                    learner_ids.find(node.raft_id) == learner_ids.end())
                {
                    AppendIssue(&result,
                                ClusterConfigIssueCode::kInvalidInitialMembership,
                                field_prefix + ".initial_role",
                                "MetadataNode configured as learner must appear in learner_raft_ids",
                                ClusterNodeType::kMetadata,
                                node.node_id,
                                node.endpoint);
                }
            }
        }

        if (voter_ids.size() + learner_ids.size() != config.metadata_nodes.size())
        {
            AppendIssue(&result,
                        ClusterConfigIssueCode::kInvalidInitialMembership,
                        "initial_raft_membership",
                        "every MetadataNode must appear exactly once in initial membership");
        }

        return result;
    }

    ClusterEndpointAllocationResult AllocateClusterEndpoints(
        const ClusterConfigGenerationRequest &request)
    {
        ClusterEndpointAllocationResult result;
        const std::vector<std::string> request_errors =
            ValidateGenerationRequest(request);
        if (!request_errors.empty())
        {
            result.status = ClusterConfigStatusCode::kInvalidArgument;
            result.error_detail = JoinMessages(request_errors);
            return result;
        }

        const std::string endpoint_host = request.bind_host;
        result.assignments.reserve(request.view_node_count +
                                   request.metadata_node_count +
                                   request.storage_node_count);

        auto append_assignment = [&](const ClusterNodeType node_type,
                                     const std::size_t ordinal,
                                     const std::uint16_t port_base) {
            const ClusterNodeId node_id =
                ResolveRequestedNodeId(node_type, request, ordinal);
            result.assignments.push_back(ClusterEndpointAssignment{
                .node_type = node_type,
                .node_id = node_id,
                .ordinal = ordinal,
                .endpoint = MakeEndpoint(
                    endpoint_host,
                    static_cast<std::uint16_t>(port_base + ordinal)),
            });
        };

        for (std::size_t index = 0; index < request.view_node_count; ++index)
        {
            append_assignment(ClusterNodeType::kView, index, request.view_port_base);
        }
        for (std::size_t index = 0; index < request.metadata_node_count; ++index)
        {
            append_assignment(ClusterNodeType::kMetadata,
                              index,
                              request.metadata_port_base);
        }
        for (std::size_t index = 0; index < request.storage_node_count; ++index)
        {
            append_assignment(ClusterNodeType::kStorage,
                              index,
                              request.storage_port_base);
        }

        std::unordered_set<std::string> seen_endpoints;
        for (const ClusterEndpointAssignment &assignment : result.assignments)
        {
            if (!ParseEndpoint(assignment.endpoint, nullptr, nullptr))
            {
                AppendIssue(&result.validation,
                            ClusterConfigIssueCode::kInvalidEndpoint,
                            AllocationFieldPath(assignment.node_type, assignment.ordinal),
                            "generated endpoint must use host:port format with port in 1..65535",
                            assignment.node_type,
                            assignment.node_id,
                            assignment.endpoint);
                continue;
            }

            if (!seen_endpoints.insert(assignment.endpoint).second)
            {
                // endpoint allocation 必须显式报告冲突，不能静默跳到其它端口。
                AppendIssue(&result.validation,
                            ClusterConfigIssueCode::kDuplicateEndpoint,
                            AllocationFieldPath(assignment.node_type, assignment.ordinal),
                            "generated endpoint collides with another configured node",
                            assignment.node_type,
                            assignment.node_id,
                            assignment.endpoint);
            }
        }

        result.status = DeriveGenerationStatus(result.validation);
        if (!result.validation.ok())
        {
            result.error_detail = BuildValidationErrorDetail(result.validation);
        }
        return result;
    }

    ClusterConfigGenerationResult GenerateDeterministicClusterConfig(
        const ClusterConfigGenerationRequest &request)
    {
        ClusterConfigGenerationResult result;
        const ClusterEndpointAllocationResult allocation =
            AllocateClusterEndpoints(request);
        if (!allocation.ok())
        {
            result.status = allocation.status;
            result.error_detail = allocation.error_detail;
            result.validation = allocation.validation;
            return result;
        }

        ClusterConfig config;
        config.cluster_id = request.cluster_id;
        config.base_dir = request.base_dir;
        config.chunk_policy = request.chunk_policy;
        config.timeouts = request.timeouts;

        config.view_nodes.reserve(request.view_node_count);
        for (std::size_t index = 0; index < request.view_node_count; ++index)
        {
            const ClusterEndpointAssignment *assignment =
                FindAssignment(allocation.assignments,
                               ClusterNodeType::kView,
                               index);
            if (assignment == nullptr)
            {
                result.status = ClusterConfigStatusCode::kInternalError;
                result.error_detail =
                    "internal error: missing ViewNode endpoint allocation";
                return result;
            }

            config.view_nodes.push_back(ViewNodeConfig{
                .node_id = assignment->node_id,
                .endpoint = assignment->endpoint,
                .data_dir = MakeRoleDataDir(request.base_dir,
                                            "view",
                                            assignment->node_id),
            });
        }

        config.metadata_nodes.reserve(request.metadata_node_count);
        const std::vector<std::int32_t> fixed_positive_raft_ids =
            CollectFixedPositiveRaftIds(request.fixed_metadata_raft_ids);
        std::unordered_set<std::int32_t> reserved_raft_ids(
            fixed_positive_raft_ids.begin(),
            fixed_positive_raft_ids.end());
        std::int32_t next_generated_raft_id = 1;

        for (std::size_t index = 0; index < request.metadata_node_count; ++index)
        {
            const ClusterEndpointAssignment *assignment =
                FindAssignment(allocation.assignments,
                               ClusterNodeType::kMetadata,
                               index);
            if (assignment == nullptr)
            {
                result.status = ClusterConfigStatusCode::kInternalError;
                result.error_detail =
                    "internal error: missing MetadataNode endpoint allocation";
                return result;
            }

            const std::int32_t raft_id =
                index < request.fixed_metadata_raft_ids.size()
                    ? request.fixed_metadata_raft_ids[index]
                    : NextGeneratedRaftId(reserved_raft_ids, &next_generated_raft_id);

            config.metadata_nodes.push_back(MetadataNodeConfig{
                .node_id = assignment->node_id,
                .raft_id = raft_id,
                .endpoint = assignment->endpoint,
                .data_dir = MakeMetadataDataDir(request.base_dir,
                                                assignment->node_id),
                .snapshot_dir = MakeMetadataSnapshotDir(request.base_dir,
                                                        assignment->node_id),
                .initial_role =
                    index < request.metadata_voter_count
                        ? MetadataNodeInitialRole::kVoter
                        : MetadataNodeInitialRole::kLearner,
            });

            if (raft_id > 0)
            {
                reserved_raft_ids.insert(raft_id);
            }
        }

        config.storage_nodes.reserve(request.storage_node_count);
        for (std::size_t index = 0; index < request.storage_node_count; ++index)
        {
            const ClusterEndpointAssignment *assignment =
                FindAssignment(allocation.assignments,
                               ClusterNodeType::kStorage,
                               index);
            if (assignment == nullptr)
            {
                result.status = ClusterConfigStatusCode::kInternalError;
                result.error_detail =
                    "internal error: missing StorageNode endpoint allocation";
                return result;
            }
            const std::uint64_t capacity_bytes =
                index < request.storage_capacity_overrides_bytes.size()
                    ? request.storage_capacity_overrides_bytes[index]
                    : request.default_storage_capacity_bytes;

            config.storage_nodes.push_back(StorageNodeConfig{
                .node_id = assignment->node_id,
                .endpoint = assignment->endpoint,
                .data_dir = MakeRoleDataDir(request.base_dir,
                                            "storage",
                                            assignment->node_id),
                .capacity_bytes = capacity_bytes,
                .failure_domain = {},
            });
        }

        config.initial_raft_membership.membership_epoch =
            request.generation_seed.value_or(1);
        for (const MetadataNodeConfig &node : config.metadata_nodes)
        {
            if (node.initial_role == MetadataNodeInitialRole::kVoter)
            {
                config.initial_raft_membership.voter_raft_ids.push_back(node.raft_id);
            }
            else if (node.initial_role == MetadataNodeInitialRole::kLearner)
            {
                config.initial_raft_membership.learner_raft_ids.push_back(node.raft_id);
            }
        }

        result.config = std::move(config);
        result.validation = ValidateClusterConfig(result.config);
        result.status = DeriveGenerationStatus(result.validation);
        if (!result.validation.ok())
        {
            result.error_detail = BuildValidationErrorDetail(result.validation);
        }

        return result;
    }

    ClusterNodeResolutionResult ResolveClusterNodeConfig(
        const ClusterConfig &config,
        const ClusterNodeType node_type,
        const std::string_view node_id)
    {
        ClusterNodeResolutionResult result;
        result.validation = ValidateClusterConfig(config);
        if (!result.validation.ok())
        {
            result.status = DeriveGenerationStatus(result.validation);
            result.error_detail = BuildValidationErrorDetail(result.validation);
            return result;
        }

        if (node_type == ClusterNodeType::kUnknown)
        {
            AppendIssue(&result.validation,
                        ClusterConfigIssueCode::kInvalidNodeType,
                        "node_type",
                        "node_type must be view, metadata or storage");
            result.status = ClusterConfigStatusCode::kInvalidArgument;
            result.error_detail = BuildValidationErrorDetail(result.validation);
            return result;
        }

        if (!IsValidNodeId(node_id))
        {
            AppendIssue(&result.validation,
                        ClusterConfigIssueCode::kInvalidNodeId,
                        "node_id",
                        "node_id must not be empty and must use safe characters");
            result.status = ClusterConfigStatusCode::kInvalidArgument;
            result.error_detail = BuildValidationErrorDetail(result.validation);
            return result;
        }

        // 按 role + node_id 精确命中，解析失败必须显式报错，不能 fallback。
        if (node_type == ClusterNodeType::kView)
        {
            for (const ViewNodeConfig &node : config.view_nodes)
            {
                if (node.node_id.has_value() && *node.node_id == node_id)
                {
                    result.resolved = ResolvedClusterNodeConfig{
                        .node_type = ClusterNodeType::kView,
                        .node_id = *node.node_id,
                        .endpoint = node.endpoint,
                        .data_dir = node.data_dir,
                        .snapshot_dir = std::nullopt,
                        .raft_id = std::nullopt,
                        .metadata_initial_role = std::nullopt,
                        .capacity_bytes = std::nullopt,
                        .failure_domain = {},
                    };
                    return result;
                }
            }
        }
        else if (node_type == ClusterNodeType::kMetadata)
        {
            for (const MetadataNodeConfig &node : config.metadata_nodes)
            {
                if (node.node_id == node_id)
                {
                    result.resolved = ResolvedClusterNodeConfig{
                        .node_type = ClusterNodeType::kMetadata,
                        .node_id = node.node_id,
                        .endpoint = node.endpoint,
                        .data_dir = node.data_dir,
                        .snapshot_dir = node.snapshot_dir,
                        .raft_id = node.raft_id,
                        .metadata_initial_role = node.initial_role,
                        .capacity_bytes = std::nullopt,
                        .failure_domain = {},
                    };
                    return result;
                }
            }
        }
        else if (node_type == ClusterNodeType::kStorage)
        {
            for (const StorageNodeConfig &node : config.storage_nodes)
            {
                if (node.node_id.has_value() && *node.node_id == node_id)
                {
                    result.resolved = ResolvedClusterNodeConfig{
                        .node_type = ClusterNodeType::kStorage,
                        .node_id = *node.node_id,
                        .endpoint = node.endpoint,
                        .data_dir = node.data_dir,
                        .snapshot_dir = std::nullopt,
                        .raft_id = std::nullopt,
                        .metadata_initial_role = std::nullopt,
                        .capacity_bytes = node.capacity_bytes,
                        .failure_domain = node.failure_domain,
                    };
                    return result;
                }
            }
        }

        ClusterNodeType actual_node_type = ClusterNodeType::kUnknown;
        for (const ViewNodeConfig &node : config.view_nodes)
        {
            if (node.node_id.has_value() && *node.node_id == node_id)
            {
                actual_node_type = ClusterNodeType::kView;
                break;
            }
        }
        if (actual_node_type == ClusterNodeType::kUnknown)
        {
            for (const MetadataNodeConfig &node : config.metadata_nodes)
            {
                if (node.node_id == node_id)
                {
                    actual_node_type = ClusterNodeType::kMetadata;
                    break;
                }
            }
        }
        if (actual_node_type == ClusterNodeType::kUnknown)
        {
            for (const StorageNodeConfig &node : config.storage_nodes)
            {
                if (node.node_id.has_value() && *node.node_id == node_id)
                {
                    actual_node_type = ClusterNodeType::kStorage;
                    break;
                }
            }
        }

        if (actual_node_type != ClusterNodeType::kUnknown)
        {
            AppendIssue(&result.validation,
                        ClusterConfigIssueCode::kInvalidNodeType,
                        "node_type",
                        "node_id exists but belongs to a different role in cluster config",
                        actual_node_type,
                        std::string(node_id));
        }
        else
        {
            AppendIssue(&result.validation,
                        ClusterConfigIssueCode::kInvalidNodeId,
                        "node_id",
                        "node_id does not exist for the requested role",
                        node_type,
                        std::string(node_id));
        }

        result.status = ClusterConfigStatusCode::kInvalidArgument;
        result.error_detail = BuildValidationErrorDetail(result.validation);
        return result;
    }

    InitialRaftQuorumComputationResult ComputeInitialRaftQuorum(
        const InitialRaftMembershipConfig &membership)
    {
        InitialRaftQuorumComputationResult result;
        result.validation = ValidateInitialQuorumMembershipOnly(membership);
        if (!result.validation.ok())
        {
            result.status = DeriveGenerationStatus(result.validation);
            result.error_detail = BuildValidationErrorDetail(result.validation);
            return result;
        }

        const std::size_t quorum_size =
            ComputeInitialRaftQuorumSize(membership.voter_raft_ids.size());
        result.summary = InitialRaftQuorumSummary{
            .voter_count = membership.voter_raft_ids.size(),
            // election quorum 和 commit quorum 都是 initial voters 的 majority。
            .election_quorum = quorum_size,
            .commit_quorum = quorum_size,
            .voter_raft_ids = membership.voter_raft_ids,
        };
        return result;
    }

    InitialRaftQuorumComputationResult ComputeInitialRaftQuorum(
        const ClusterConfig &config)
    {
        InitialRaftQuorumComputationResult result;
        result.validation = ValidateClusterConfig(config);
        if (!result.validation.ok())
        {
            result.status = DeriveGenerationStatus(result.validation);
            result.error_detail = BuildValidationErrorDetail(result.validation);
            return result;
        }

        result = ComputeInitialRaftQuorum(config.initial_raft_membership);
        if (!result.ok())
        {
            return result;
        }

        return result;
    }

    std::size_t ComputeInitialRaftQuorumSize(const std::size_t voter_count)
    {
        if (voter_count == 0)
        {
            return 0;
        }

        return (voter_count / 2U) + 1U;
    }

    std::size_t ComputeInitialRaftQuorumSize(
        const InitialRaftMembershipConfig &membership)
    {
        const InitialRaftQuorumComputationResult result =
            ComputeInitialRaftQuorum(membership);
        return result.ok() ? result.summary->commit_quorum : 0U;
    }

    std::string SerializeClusterConfigToJson(const ClusterConfig &config)
    {
        std::ostringstream oss;
        oss << "{\n";
        oss << "  \"cluster_id\": " << JsonString(config.cluster_id) << ",\n";
        oss << "  \"base_dir\": " << JsonPath(config.base_dir) << ",\n";

        oss << "  \"view_nodes\": [\n";
        for (std::size_t index = 0; index < config.view_nodes.size(); ++index)
        {
            const auto &node = config.view_nodes[index];
            oss << "    {\n";
            if (node.node_id.has_value())
            {
                oss << "      \"node_id\": " << JsonString(*node.node_id) << ",\n";
            }
            else
            {
                oss << "      \"node_id\": null,\n";
            }
            oss << "      \"endpoint\": " << JsonString(node.endpoint) << ",\n";
            oss << "      \"data_dir\": " << JsonPath(node.data_dir) << "\n";
            oss << "    }";
            if (index + 1 != config.view_nodes.size())
            {
                oss << ",";
            }
            oss << "\n";
        }
        oss << "  ],\n";

        oss << "  \"metadata_nodes\": [\n";
        for (std::size_t index = 0; index < config.metadata_nodes.size(); ++index)
        {
            const auto &node = config.metadata_nodes[index];
            oss << "    {\n";
            oss << "      \"node_id\": " << JsonString(node.node_id) << ",\n";
            oss << "      \"raft_id\": " << node.raft_id << ",\n";
            oss << "      \"endpoint\": " << JsonString(node.endpoint) << ",\n";
            oss << "      \"data_dir\": " << JsonPath(node.data_dir) << ",\n";
            oss << "      \"snapshot_dir\": " << JsonPath(node.snapshot_dir)
                << ",\n";
            oss << "      \"initial_role\": "
                << JsonString(ToString(node.initial_role)) << "\n";
            oss << "    }";
            if (index + 1 != config.metadata_nodes.size())
            {
                oss << ",";
            }
            oss << "\n";
        }
        oss << "  ],\n";

        oss << "  \"storage_nodes\": [\n";
        for (std::size_t index = 0; index < config.storage_nodes.size(); ++index)
        {
            const auto &node = config.storage_nodes[index];
            oss << "    {\n";
            if (node.node_id.has_value())
            {
                oss << "      \"node_id\": " << JsonString(*node.node_id) << ",\n";
            }
            else
            {
                oss << "      \"node_id\": null,\n";
            }
            oss << "      \"endpoint\": " << JsonString(node.endpoint) << ",\n";
            oss << "      \"data_dir\": " << JsonPath(node.data_dir) << ",\n";
            oss << "      \"capacity_bytes\": " << node.capacity_bytes << ",\n";
            oss << "      \"failure_domain\": {\n";
            oss << "        \"zone\": "
                << JsonString(node.failure_domain.zone) << ",\n";
            oss << "        \"rack\": "
                << JsonString(node.failure_domain.rack) << "\n";
            oss << "      }\n";
            oss << "    }";
            if (index + 1 != config.storage_nodes.size())
            {
                oss << ",";
            }
            oss << "\n";
        }
        oss << "  ],\n";

        oss << "  \"initial_raft_membership\": {\n";
        oss << "    \"membership_epoch\": "
            << config.initial_raft_membership.membership_epoch << ",\n";
        oss << "    \"voter_raft_ids\": [";
        for (std::size_t index = 0;
             index < config.initial_raft_membership.voter_raft_ids.size();
             ++index)
        {
            if (index != 0)
            {
                oss << ", ";
            }
            oss << config.initial_raft_membership.voter_raft_ids[index];
        }
        oss << "],\n";
        oss << "    \"learner_raft_ids\": [";
        for (std::size_t index = 0;
             index < config.initial_raft_membership.learner_raft_ids.size();
             ++index)
        {
            if (index != 0)
            {
                oss << ", ";
            }
            oss << config.initial_raft_membership.learner_raft_ids[index];
        }
        oss << "]\n";
        oss << "  },\n";

        oss << "  \"chunk_policy\": {\n";
        oss << "    \"chunk_size_bytes\": "
            << config.chunk_policy.chunk_size_bytes << ",\n";
        oss << "    \"replica_count\": " << config.chunk_policy.replica_count
            << ",\n";
        oss << "    \"minimum_successful_writes\": "
            << config.chunk_policy.minimum_successful_writes << ",\n";
        oss << "    \"checksum_algorithm\": "
            << JsonString(ToString(config.chunk_policy.checksum_algorithm))
            << "\n";
        oss << "  },\n";

        oss << "  \"timeouts\": {\n";
        oss << "    \"discovery_rpc_timeout_ms\": "
            << ToMillis(config.timeouts.discovery_rpc_timeout) << ",\n";
        oss << "    \"metadata_rpc_timeout_ms\": "
            << ToMillis(config.timeouts.metadata_rpc_timeout) << ",\n";
        oss << "    \"storage_rpc_timeout_ms\": "
            << ToMillis(config.timeouts.storage_rpc_timeout) << ",\n";
        oss << "    \"heartbeat_interval_ms\": "
            << ToMillis(config.timeouts.heartbeat_interval) << ",\n";
        oss << "    \"registration_timeout_ms\": "
            << ToMillis(config.timeouts.registration_timeout) << ",\n";
        oss << "    \"commit_deadline_ms\": "
            << ToMillis(config.timeouts.commit_deadline) << ",\n";
        oss << "    \"liveness_stale_timeout_ms\": "
            << ToMillis(config.timeouts.liveness_stale_timeout) << ",\n";
        oss << "    \"liveness_dead_timeout_ms\": "
            << ToMillis(config.timeouts.liveness_dead_timeout) << "\n";
        oss << "  }\n";
        oss << "}\n";
        return oss.str();
    }

    const char *ToString(const ClusterNodeType node_type)
    {
        switch (node_type)
        {
        case ClusterNodeType::kView:
            return "view";
        case ClusterNodeType::kMetadata:
            return "metadata";
        case ClusterNodeType::kStorage:
            return "storage";
        case ClusterNodeType::kUnknown:
        default:
            return "unknown";
        }
    }

    const char *ToString(const MetadataNodeInitialRole role)
    {
        switch (role)
        {
        case MetadataNodeInitialRole::kVoter:
            return "voter";
        case MetadataNodeInitialRole::kLearner:
            return "learner";
        case MetadataNodeInitialRole::kUnknown:
        default:
            return "unknown";
        }
    }

    const char *ToString(const ClusterChecksumAlgorithm algorithm)
    {
        switch (algorithm)
        {
        case ClusterChecksumAlgorithm::kSha256:
            return "sha256";
        case ClusterChecksumAlgorithm::kUnknown:
        default:
            return "unknown";
        }
    }

    const char *ToString(const ClusterConfigIssueCode code)
    {
        switch (code)
        {
        case ClusterConfigIssueCode::kMissingClusterId:
            return "missing_cluster_id";
        case ClusterConfigIssueCode::kInvalidNodeCount:
            return "invalid_node_count";
        case ClusterConfigIssueCode::kInvalidNodeType:
            return "invalid_node_type";
        case ClusterConfigIssueCode::kInvalidNodeId:
            return "invalid_node_id";
        case ClusterConfigIssueCode::kDuplicateNodeId:
            return "duplicate_node_id";
        case ClusterConfigIssueCode::kInvalidEndpoint:
            return "invalid_endpoint";
        case ClusterConfigIssueCode::kDuplicateEndpoint:
            return "duplicate_endpoint";
        case ClusterConfigIssueCode::kMissingDataDir:
            return "missing_data_dir";
        case ClusterConfigIssueCode::kMissingSnapshotDir:
            return "missing_snapshot_dir";
        case ClusterConfigIssueCode::kSharedDataDir:
            return "shared_data_dir";
        case ClusterConfigIssueCode::kInvalidCapacity:
            return "invalid_capacity";
        case ClusterConfigIssueCode::kInvalidChunkPolicy:
            return "invalid_chunk_policy";
        case ClusterConfigIssueCode::kInvalidTimeoutPolicy:
            return "invalid_timeout_policy";
        case ClusterConfigIssueCode::kInvalidRaftId:
            return "invalid_raft_id";
        case ClusterConfigIssueCode::kDuplicateRaftId:
            return "duplicate_raft_id";
        case ClusterConfigIssueCode::kInvalidRaftVoterCount:
            return "invalid_raft_voter_count";
        case ClusterConfigIssueCode::kInvalidInitialMembership:
            return "invalid_initial_membership";
        case ClusterConfigIssueCode::kIdentityConfigMismatch:
            return "identity_config_mismatch";
        case ClusterConfigIssueCode::kUnsupportedDurabilityMode:
            return "unsupported_durability_mode";
        case ClusterConfigIssueCode::kUnknown:
        default:
            return "unknown";
        }
    }

    const char *ToString(const ClusterConfigStatusCode code)
    {
        switch (code)
        {
        case ClusterConfigStatusCode::kOk:
            return "ok";
        case ClusterConfigStatusCode::kInvalidArgument:
            return "invalid_argument";
        case ClusterConfigStatusCode::kConflict:
            return "conflict";
        case ClusterConfigStatusCode::kUnsupported:
            return "unsupported";
        case ClusterConfigStatusCode::kInternalError:
            return "internal_error";
        default:
            return "unknown";
        }
    }

    std::string DescribeClusterConfigIssue(
        const ClusterConfigValidationIssue &issue)
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
        if (!issue.endpoint.empty())
        {
            oss << " endpoint=" << issue.endpoint;
        }
        if (!issue.path.empty())
        {
            oss << " path=" << issue.path.generic_string();
        }
        return oss.str();
    }

} // namespace clusterdemo
