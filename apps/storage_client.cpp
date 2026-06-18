#include "cluster/cluster_config.h"
#include "store/transfer/object_transfer.h"

#include "store/transfer/metadata_transfer_client.h"
#include "store/transfer/storage_transfer_client.h"
#include "view/view_client.h"

#include <grpcpp/create_channel.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    struct ClientConfig
    {
        std::filesystem::path config_path;
        std::string cluster_id;
        std::string view_endpoint;
        std::uint64_t chunk_size{storedemo::kProductionChunkSizeBytes};
        std::uint32_t replica_count{1};
        std::uint32_t minimum_successful_writes{1};
        std::chrono::milliseconds discovery_timeout{3000};
        std::chrono::milliseconds metadata_timeout{3000};
        std::chrono::milliseconds storage_timeout{3000};
        std::chrono::milliseconds commit_deadline{5000};
    };

    class ClientConfigError final : public std::runtime_error
    {
    public:
        explicit ClientConfigError(const std::string &message)
            : std::runtime_error(message)
        {
        }
    };

    struct ParsedArgs
    {
        std::string command;
        std::filesystem::path config_path;
        std::filesystem::path output_path;
        std::filesystem::path base_dir;
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::string cluster_id;
        std::string bind_host;
        std::string advertise_host;
        std::filesystem::path source_path;
        std::filesystem::path destination_path;
        std::optional<std::uint64_t> version;
        std::string request_id;
        std::optional<std::uint64_t> chunk_size;
        std::optional<std::uint32_t> replica_count;
        std::optional<std::uint32_t> minimum_successful_writes;
        std::optional<std::uint32_t> concurrency;
        std::optional<std::size_t> view_node_count;
        std::optional<std::size_t> metadata_node_count;
        std::optional<std::size_t> metadata_voter_count;
        std::optional<std::size_t> storage_node_count;
        std::optional<std::uint16_t> view_port_base;
        std::optional<std::uint16_t> metadata_port_base;
        std::optional<std::uint16_t> storage_port_base;
        std::optional<std::uint64_t> storage_capacity_bytes;
        std::optional<std::uint64_t> discovery_timeout_ms;
        std::optional<std::uint64_t> metadata_timeout_ms;
        std::optional<std::uint64_t> storage_timeout_ms;
        std::optional<std::uint64_t> heartbeat_interval_ms;
        std::optional<std::uint64_t> registration_timeout_ms;
        std::optional<std::uint64_t> commit_deadline_ms;
        std::optional<std::uint64_t> liveness_stale_timeout_ms;
        std::optional<std::uint64_t> liveness_dead_timeout_ms;
        std::optional<std::uint64_t> generation_seed;
    };

    enum class CliExitCode : int
    {
        kOk = 0,
        kInvalidArgument = 2,
        kConfigError = 3,
        kTransferFailure = 4,
        kUnsupported = 5,
        kInternalError = 10,
    };

    std::string Trim(std::string value)
    {
        auto not_space = [](const unsigned char ch)
        {
            return std::isspace(ch) == 0;
        };
        value.erase(value.begin(),
                    std::find_if(value.begin(), value.end(), not_space));
        value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                    value.end());
        return value;
    }

    [[nodiscard]] std::optional<std::string> ExtractKeyValueLine(
        const std::string &content,
        const std::string_view key)
    {
        std::istringstream input(content);
        std::string line;
        while (std::getline(input, line))
        {
            const auto comment_pos = line.find('#');
            if (comment_pos != std::string::npos)
            {
                line = line.substr(0, comment_pos);
            }
            line = Trim(line);
            if (line.empty())
            {
                continue;
            }
            const auto eq_pos = line.find('=');
            if (eq_pos == std::string::npos)
            {
                continue;
            }
            const std::string current_key = Trim(line.substr(0, eq_pos));
            if (current_key != key)
            {
                continue;
            }
            return Trim(line.substr(eq_pos + 1));
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> ExtractJsonStringField(
        const std::string &content,
        const std::string_view key,
        const std::size_t search_from = 0)
    {
        const std::string needle = "\"" + std::string(key) + "\"";
        const std::size_t key_pos = content.find(needle, search_from);
        if (key_pos == std::string::npos)
        {
            return std::nullopt;
        }

        const std::size_t colon_pos = content.find(':', key_pos + needle.size());
        if (colon_pos == std::string::npos)
        {
            return std::nullopt;
        }

        std::size_t value_begin = colon_pos + 1;
        while (value_begin < content.size() &&
               std::isspace(static_cast<unsigned char>(content[value_begin])) != 0)
        {
            ++value_begin;
        }
        if (value_begin >= content.size() || content[value_begin] != '"')
        {
            return std::nullopt;
        }

        ++value_begin;
        std::string value;
        bool escaped = false;
        for (std::size_t pos = value_begin; pos < content.size(); ++pos)
        {
            const char ch = content[pos];
            if (escaped)
            {
                value.push_back(ch);
                escaped = false;
                continue;
            }
            if (ch == '\\')
            {
                escaped = true;
                continue;
            }
            if (ch == '"')
            {
                return value;
            }
            value.push_back(ch);
        }

        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::uint64_t> ExtractJsonUnsignedField(
        const std::string &content,
        const std::string_view key)
    {
        const std::string needle = "\"" + std::string(key) + "\"";
        const std::size_t key_pos = content.find(needle);
        if (key_pos == std::string::npos)
        {
            return std::nullopt;
        }

        const std::size_t colon_pos = content.find(':', key_pos + needle.size());
        if (colon_pos == std::string::npos)
        {
            return std::nullopt;
        }

        std::size_t value_begin = colon_pos + 1;
        while (value_begin < content.size() &&
               std::isspace(static_cast<unsigned char>(content[value_begin])) != 0)
        {
            ++value_begin;
        }

        std::size_t value_end = value_begin;
        while (value_end < content.size() &&
               std::isdigit(static_cast<unsigned char>(content[value_end])) != 0)
        {
            ++value_end;
        }
        if (value_end == value_begin)
        {
            return std::nullopt;
        }

        return std::stoull(content.substr(value_begin, value_end - value_begin));
    }

    [[nodiscard]] std::optional<std::string> ExtractViewEndpoint(
        const std::string &content)
    {
        if (const auto explicit_value = ExtractKeyValueLine(content, "view_endpoint");
            explicit_value.has_value())
        {
            return explicit_value;
        }
        if (const auto explicit_json = ExtractJsonStringField(content, "view_endpoint");
            explicit_json.has_value())
        {
            return explicit_json;
        }

        const std::size_t view_nodes_pos = content.find("\"view_nodes\"");
        if (view_nodes_pos == std::string::npos)
        {
            return std::nullopt;
        }
        return ExtractJsonStringField(content, "endpoint", view_nodes_pos);
    }

    [[nodiscard]] std::optional<std::string> ExtractClusterId(
        const std::string &content)
    {
        if (const auto value = ExtractKeyValueLine(content, "cluster_id");
            value.has_value())
        {
            return value;
        }
        return ExtractJsonStringField(content, "cluster_id");
    }

    [[nodiscard]] std::optional<std::uint64_t> ExtractUnsignedConfigValue(
        const std::string &content,
        const std::string_view key)
    {
        if (const auto kv = ExtractKeyValueLine(content, key); kv.has_value())
        {
            return std::stoull(*kv);
        }
        return ExtractJsonUnsignedField(content, key);
    }

    [[nodiscard]] std::string ReadWholeFile(
        const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
        {
            throw ClientConfigError("failed to open config file: " + path.string());
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

    [[nodiscard]] ClientConfig LoadClientConfig(
        const std::filesystem::path &path)
    {
        try
        {
            ClientConfig config;
            config.config_path = path;

            const std::string content = ReadWholeFile(path);
            const auto cluster_id = ExtractClusterId(content);
            if (!cluster_id.has_value() || cluster_id->empty())
            {
                throw ClientConfigError(
                    "config is missing cluster_id for storage_client");
            }
            const auto view_endpoint = ExtractViewEndpoint(content);
            if (!view_endpoint.has_value() || view_endpoint->empty())
            {
                throw ClientConfigError(
                    "config is missing a ViewNode endpoint for storage_client");
            }

            config.cluster_id = *cluster_id;
            config.view_endpoint = *view_endpoint;

            // Production upload chunk size is intentionally code-driven.
            // `chunk_size_bytes` may still exist in cluster/app config for
            // broader config compatibility, but it must not override the
            // storage_client upload default.
            if (const auto value = ExtractUnsignedConfigValue(content, "replica_count");
                value.has_value() && *value > 0)
            {
                config.replica_count = static_cast<std::uint32_t>(*value);
            }
            if (const auto value =
                    ExtractUnsignedConfigValue(content, "minimum_successful_writes");
                value.has_value() && *value > 0)
            {
                config.minimum_successful_writes =
                    static_cast<std::uint32_t>(*value);
            }
            if (const auto value =
                    ExtractUnsignedConfigValue(content, "discovery_rpc_timeout_ms");
                value.has_value())
            {
                config.discovery_timeout = std::chrono::milliseconds(*value);
            }
            else if (const auto value =
                         ExtractUnsignedConfigValue(content, "discovery_rpc_timeout");
                     value.has_value())
            {
                config.discovery_timeout = std::chrono::milliseconds(*value);
            }
            if (const auto value =
                    ExtractUnsignedConfigValue(content, "metadata_rpc_timeout_ms");
                value.has_value())
            {
                config.metadata_timeout = std::chrono::milliseconds(*value);
            }
            else if (const auto value =
                         ExtractUnsignedConfigValue(content, "metadata_rpc_timeout");
                     value.has_value())
            {
                config.metadata_timeout = std::chrono::milliseconds(*value);
            }
            if (const auto value =
                    ExtractUnsignedConfigValue(content, "storage_rpc_timeout_ms");
                value.has_value())
            {
                config.storage_timeout = std::chrono::milliseconds(*value);
            }
            else if (const auto value =
                         ExtractUnsignedConfigValue(content, "storage_rpc_timeout");
                     value.has_value())
            {
                config.storage_timeout = std::chrono::milliseconds(*value);
            }
            if (const auto value = ExtractUnsignedConfigValue(content, "commit_deadline_ms");
                value.has_value())
            {
                config.commit_deadline = std::chrono::milliseconds(*value);
            }
            else if (const auto value =
                         ExtractUnsignedConfigValue(content, "commit_deadline");
                     value.has_value())
            {
                config.commit_deadline = std::chrono::milliseconds(*value);
            }

            if (config.minimum_successful_writes > config.replica_count)
            {
                throw ClientConfigError(
                    "config minimum_successful_writes exceeds replica_count");
            }
            return config;
        }
        catch (const ClientConfigError &)
        {
            throw;
        }
        catch (const std::exception &ex)
        {
            throw ClientConfigError("invalid storage_client config: " +
                                    std::string(ex.what()));
        }
    }

    [[nodiscard]] std::uint64_t ParseUnsignedOrDie(
        const std::string &value,
        const char *name)
    {
        try
        {
            std::size_t consumed = 0;
            const std::uint64_t parsed = std::stoull(value, &consumed, 10);
            if (consumed != value.size())
            {
                throw std::invalid_argument("trailing characters");
            }
            return parsed;
        }
        catch (const std::exception &)
        {
            std::cerr << "invalid numeric value for " << name << ": " << value
                      << '\n';
            std::exit(static_cast<int>(CliExitCode::kInvalidArgument));
        }
    }

    [[nodiscard]] std::uint32_t ParseUint32OrDie(const std::string &value,
                                                 const char *name)
    {
        const std::uint64_t parsed = ParseUnsignedOrDie(value, name);
        if (parsed > static_cast<std::uint64_t>(
                         std::numeric_limits<std::uint32_t>::max()))
        {
            std::cerr << "numeric value out of range for " << name << ": "
                      << value << '\n';
            std::exit(static_cast<int>(CliExitCode::kInvalidArgument));
        }
        return static_cast<std::uint32_t>(parsed);
    }

    [[nodiscard]] std::uint16_t ParseUint16OrDie(const std::string &value,
                                                 const char *name)
    {
        const std::uint64_t parsed = ParseUnsignedOrDie(value, name);
        if (parsed > static_cast<std::uint64_t>(
                         std::numeric_limits<std::uint16_t>::max()))
        {
            std::cerr << "numeric value out of range for " << name << ": "
                      << value << '\n';
            std::exit(static_cast<int>(CliExitCode::kInvalidArgument));
        }
        return static_cast<std::uint16_t>(parsed);
    }

    [[nodiscard]] std::size_t ParseSizeOrDie(const std::string &value,
                                             const char *name)
    {
        const std::uint64_t parsed = ParseUnsignedOrDie(value, name);
        if (parsed >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        {
            std::cerr << "numeric value out of range for " << name << ": "
                      << value << '\n';
            std::exit(static_cast<int>(CliExitCode::kInvalidArgument));
        }
        return static_cast<std::size_t>(parsed);
    }

    [[nodiscard]] std::string SanitizeToken(std::string_view value)
    {
        std::string sanitized;
        sanitized.reserve(value.size());
        for (const unsigned char ch : value)
        {
            if (std::isalnum(ch) != 0 || ch == '-' || ch == '_')
            {
                sanitized.push_back(static_cast<char>(ch));
            }
            else
            {
                sanitized.push_back('_');
            }
        }
        if (sanitized.empty())
        {
            sanitized = "object";
        }
        return sanitized;
    }

    [[nodiscard]] std::string GenerateRequestId(std::string_view command,
                                                std::string_view object_key)
    {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        std::ostringstream oss;
        oss << "storage-client-" << command << '-' << now << '-'
            << SanitizeToken(object_key);
        return oss.str();
    }

    void PrintCommandFailure(std::string_view command,
                             std::string_view request_id,
                             std::string_view status,
                             std::string_view message)
    {
        std::cerr << command << " FAILED";
        if (!request_id.empty())
        {
            std::cerr << " request_id=" << request_id;
        }
        std::cerr << " status=" << status;
        if (!message.empty())
        {
            std::cerr << " message=" << message;
        }
        std::cerr << '\n';
    }

    void PrintCommandSuccess(std::string_view command,
                             std::string_view request_id)
    {
        std::cout << command << " OK";
        if (!request_id.empty())
        {
            std::cout << " request_id=" << request_id;
        }
    }

    [[nodiscard]] std::string DeriveObjectId(const std::string_view bucket,
                                             const std::string_view object_key)
    {
        constexpr std::uint64_t kOffsetBasis = 1469598103934665603ULL;
        constexpr std::uint64_t kPrime = 1099511628211ULL;

        std::uint64_t hash = kOffsetBasis;
        for (const unsigned char ch : std::string(bucket))
        {
            hash ^= static_cast<std::uint64_t>(ch);
            hash *= kPrime;
        }
        hash ^= static_cast<std::uint64_t>('\n');
        hash *= kPrime;
        for (const unsigned char ch : std::string(object_key))
        {
            hash ^= static_cast<std::uint64_t>(ch);
            hash *= kPrime;
        }

        std::ostringstream oss;
        oss << "obj-" << std::hex << std::nouppercase << hash;
        return oss.str();
    }

    const char *ToString(const storedemo::ObjectTransferStatusCode status)
    {
        using storedemo::ObjectTransferStatusCode;
        switch (status)
        {
        case ObjectTransferStatusCode::kOk:
            return "OK";
        case ObjectTransferStatusCode::kInvalidArgument:
            return "INVALID_ARGUMENT";
        case ObjectTransferStatusCode::kNotFound:
            return "NOT_FOUND";
        case ObjectTransferStatusCode::kConflict:
            return "CONFLICT";
        case ObjectTransferStatusCode::kDiscoveryUnavailable:
            return "DISCOVERY_UNAVAILABLE";
        case ObjectTransferStatusCode::kMetadataNotLeader:
            return "METADATA_NOT_LEADER";
        case ObjectTransferStatusCode::kMetadataRejected:
            return "METADATA_REJECTED";
        case ObjectTransferStatusCode::kStorageRejected:
            return "STORAGE_REJECTED";
        case ObjectTransferStatusCode::kChecksumMismatch:
            return "CHECKSUM_MISMATCH";
        case ObjectTransferStatusCode::kIoError:
            return "IO_ERROR";
        case ObjectTransferStatusCode::kTimeout:
            return "TIMEOUT";
        case ObjectTransferStatusCode::kCancelled:
            return "CANCELLED";
        case ObjectTransferStatusCode::kUnsupported:
            return "UNSUPPORTED";
        case ObjectTransferStatusCode::kInternalError:
        default:
            return "INTERNAL_ERROR";
        }
    }

    [[nodiscard]] int ExitCodeForTransferStatus(
        const storedemo::ObjectTransferStatusCode status)
    {
        using storedemo::ObjectTransferStatusCode;
        switch (status)
        {
        case ObjectTransferStatusCode::kOk:
            return static_cast<int>(CliExitCode::kOk);
        case ObjectTransferStatusCode::kInvalidArgument:
            return static_cast<int>(CliExitCode::kInvalidArgument);
        case ObjectTransferStatusCode::kUnsupported:
            return static_cast<int>(CliExitCode::kUnsupported);
        case ObjectTransferStatusCode::kInternalError:
            return static_cast<int>(CliExitCode::kInternalError);
        case ObjectTransferStatusCode::kNotFound:
        case ObjectTransferStatusCode::kConflict:
        case ObjectTransferStatusCode::kDiscoveryUnavailable:
        case ObjectTransferStatusCode::kMetadataNotLeader:
        case ObjectTransferStatusCode::kMetadataRejected:
        case ObjectTransferStatusCode::kStorageRejected:
        case ObjectTransferStatusCode::kChecksumMismatch:
        case ObjectTransferStatusCode::kIoError:
        case ObjectTransferStatusCode::kTimeout:
        case ObjectTransferStatusCode::kCancelled:
        default:
            return static_cast<int>(CliExitCode::kTransferFailure);
        }
    }

    void PrintUsage()
    {
        std::cerr
            << "Usage:\n"
            << "  storage_client generate-config --out <path> --base-dir <dir>"
            << " [--cluster-id <id>] [--bind-host <host>]"
            << " [--advertise-host <host>] [--view-count <n>]"
            << " [--metadata-count <n>] [--metadata-voters <n>]"
            << " [--storage-count <n>] [--view-port-base <port>]"
            << " [--metadata-port-base <port>] [--storage-port-base <port>]"
            << " [--storage-capacity <bytes>] [--chunk-size <bytes>]"
            << " [--replicas <n>] [--min-writes <n>]"
            << " [--discovery-timeout-ms <ms>]"
            << " [--metadata-timeout-ms <ms>]"
            << " [--storage-timeout-ms <ms>]"
            << " [--heartbeat-interval-ms <ms>]"
            << " [--registration-timeout-ms <ms>]"
            << " [--commit-deadline-ms <ms>]"
            << " [--liveness-stale-timeout-ms <ms>]"
            << " [--liveness-dead-timeout-ms <ms>]"
            << " [--generation-seed <n>]\n"
            << "  storage_client upload --config <path> --bucket <bucket>"
            << " --object <key> --file <source>"
            << " [--object-id <id>] [--request-id <id>]"
            << " [--chunk-size <bytes>] [--replicas <n>] [--min-writes <n>]"
            << " [--concurrency <n>]\n"
            << "  storage_client download --config <path> --bucket <bucket>"
            << " --object <key> --out <destination>"
            << " [--object-id <id>] [--version <n>] [--request-id <id>]"
            << " [--concurrency <n>]\n"
            << "  storage_client status --config <path>"
            << " [--request-id <id>]\n";
    }

    [[noreturn]] void ExitUsageError(const std::string &message)
    {
        if (!message.empty())
        {
            std::cerr << message << '\n';
        }
        PrintUsage();
        std::exit(static_cast<int>(CliExitCode::kInvalidArgument));
    }

    [[nodiscard]] ParsedArgs ParseArgs(int argc, char **argv)
    {
        if (argc < 2)
        {
            PrintUsage();
            std::exit(static_cast<int>(CliExitCode::kInvalidArgument));
        }

        ParsedArgs args;
        args.command = argv[1];
        if (args.command == "--help" || args.command == "-h")
        {
            PrintUsage();
            std::exit(static_cast<int>(CliExitCode::kOk));
        }
        if (args.command != "generate-config" && args.command != "upload" &&
            args.command != "download" && args.command != "status")
        {
            ExitUsageError("unsupported command: " + args.command);
        }

        for (int index = 2; index < argc; ++index)
        {
            const std::string flag = argv[index];
            auto require_value = [&](const char *name) -> std::string
            {
                if (index + 1 >= argc)
                {
                    ExitUsageError(std::string("missing value for ") + name);
                }
                ++index;
                return argv[index];
            };

            if (flag == "--config")
            {
                args.config_path = require_value("--config");
            }
            else if (flag == "--base-dir")
            {
                args.base_dir = require_value("--base-dir");
            }
            else if (flag == "--cluster-id")
            {
                args.cluster_id = require_value("--cluster-id");
            }
            else if (flag == "--bind-host")
            {
                args.bind_host = require_value("--bind-host");
            }
            else if (flag == "--advertise-host")
            {
                args.advertise_host = require_value("--advertise-host");
            }
            else if (flag == "--bucket")
            {
                args.bucket = require_value("--bucket");
            }
            else if (flag == "--object")
            {
                args.object_key = require_value("--object");
            }
            else if (flag == "--file")
            {
                args.source_path = require_value("--file");
            }
            else if (flag == "--out")
            {
                if (args.command == "generate-config")
                {
                    args.output_path = require_value("--out");
                }
                else
                {
                    args.destination_path = require_value("--out");
                }
            }
            else if (flag == "--object-id")
            {
                args.object_id = require_value("--object-id");
            }
            else if (flag == "--request-id")
            {
                args.request_id = require_value("--request-id");
            }
            else if (flag == "--version")
            {
                args.version =
                    ParseUnsignedOrDie(require_value("--version"), "--version");
            }
            else if (flag == "--view-count")
            {
                args.view_node_count =
                    ParseSizeOrDie(require_value("--view-count"), "--view-count");
            }
            else if (flag == "--metadata-count")
            {
                args.metadata_node_count = ParseSizeOrDie(
                    require_value("--metadata-count"), "--metadata-count");
            }
            else if (flag == "--metadata-voters")
            {
                args.metadata_voter_count = ParseSizeOrDie(
                    require_value("--metadata-voters"), "--metadata-voters");
            }
            else if (flag == "--storage-count")
            {
                args.storage_node_count = ParseSizeOrDie(
                    require_value("--storage-count"), "--storage-count");
            }
            else if (flag == "--view-port-base")
            {
                args.view_port_base = ParseUint16OrDie(
                    require_value("--view-port-base"), "--view-port-base");
            }
            else if (flag == "--metadata-port-base")
            {
                args.metadata_port_base = ParseUint16OrDie(
                    require_value("--metadata-port-base"),
                    "--metadata-port-base");
            }
            else if (flag == "--storage-port-base")
            {
                args.storage_port_base = ParseUint16OrDie(
                    require_value("--storage-port-base"), "--storage-port-base");
            }
            else if (flag == "--storage-capacity")
            {
                args.storage_capacity_bytes = ParseUnsignedOrDie(
                    require_value("--storage-capacity"), "--storage-capacity");
            }
            else if (flag == "--chunk-size")
            {
                args.chunk_size =
                    ParseUnsignedOrDie(require_value("--chunk-size"),
                                       "--chunk-size");
            }
            else if (flag == "--replicas")
            {
                args.replica_count =
                    ParseUint32OrDie(require_value("--replicas"), "--replicas");
            }
            else if (flag == "--min-writes")
            {
                args.minimum_successful_writes = ParseUint32OrDie(
                    require_value("--min-writes"), "--min-writes");
            }
            else if (flag == "--concurrency")
            {
                args.concurrency = ParseUint32OrDie(
                    require_value("--concurrency"), "--concurrency");
            }
            else if (flag == "--discovery-timeout-ms")
            {
                args.discovery_timeout_ms = ParseUnsignedOrDie(
                    require_value("--discovery-timeout-ms"),
                    "--discovery-timeout-ms");
            }
            else if (flag == "--metadata-timeout-ms")
            {
                args.metadata_timeout_ms = ParseUnsignedOrDie(
                    require_value("--metadata-timeout-ms"),
                    "--metadata-timeout-ms");
            }
            else if (flag == "--storage-timeout-ms")
            {
                args.storage_timeout_ms = ParseUnsignedOrDie(
                    require_value("--storage-timeout-ms"),
                    "--storage-timeout-ms");
            }
            else if (flag == "--heartbeat-interval-ms")
            {
                args.heartbeat_interval_ms = ParseUnsignedOrDie(
                    require_value("--heartbeat-interval-ms"),
                    "--heartbeat-interval-ms");
            }
            else if (flag == "--registration-timeout-ms")
            {
                args.registration_timeout_ms = ParseUnsignedOrDie(
                    require_value("--registration-timeout-ms"),
                    "--registration-timeout-ms");
            }
            else if (flag == "--commit-deadline-ms")
            {
                args.commit_deadline_ms = ParseUnsignedOrDie(
                    require_value("--commit-deadline-ms"),
                    "--commit-deadline-ms");
            }
            else if (flag == "--liveness-stale-timeout-ms")
            {
                args.liveness_stale_timeout_ms = ParseUnsignedOrDie(
                    require_value("--liveness-stale-timeout-ms"),
                    "--liveness-stale-timeout-ms");
            }
            else if (flag == "--liveness-dead-timeout-ms")
            {
                args.liveness_dead_timeout_ms = ParseUnsignedOrDie(
                    require_value("--liveness-dead-timeout-ms"),
                    "--liveness-dead-timeout-ms");
            }
            else if (flag == "--generation-seed")
            {
                args.generation_seed = ParseUnsignedOrDie(
                    require_value("--generation-seed"), "--generation-seed");
            }
            else if (flag == "--help" || flag == "-h")
            {
                PrintUsage();
                std::exit(static_cast<int>(CliExitCode::kOk));
            }
            else
            {
                ExitUsageError("unknown argument: " + flag);
            }
        }

        if (args.command == "generate-config")
        {
            if (args.output_path.empty())
            {
                ExitUsageError("generate-config requires --out");
            }
            if (args.base_dir.empty())
            {
                ExitUsageError("generate-config requires --base-dir");
            }
            if (!args.config_path.empty())
            {
                ExitUsageError("generate-config does not accept --config");
            }
            if (!args.bucket.empty() || !args.object_key.empty() ||
                !args.object_id.empty() || !args.request_id.empty() ||
                !args.source_path.empty() || !args.destination_path.empty() ||
                args.version.has_value() || args.concurrency.has_value())
            {
                ExitUsageError(
                    "generate-config does not accept upload/download-specific arguments");
            }
            return args;
        }

        if (args.config_path.empty())
        {
            ExitUsageError("--config is required");
        }
        if (!args.output_path.empty() || !args.base_dir.empty() ||
            !args.cluster_id.empty() || !args.bind_host.empty() ||
            !args.advertise_host.empty() || args.view_node_count.has_value() ||
            args.metadata_node_count.has_value() ||
            args.metadata_voter_count.has_value() ||
            args.storage_node_count.has_value() ||
            args.view_port_base.has_value() ||
            args.metadata_port_base.has_value() ||
            args.storage_port_base.has_value() ||
            args.storage_capacity_bytes.has_value() ||
            args.discovery_timeout_ms.has_value() ||
            args.metadata_timeout_ms.has_value() ||
            args.storage_timeout_ms.has_value() ||
            args.heartbeat_interval_ms.has_value() ||
            args.registration_timeout_ms.has_value() ||
            args.commit_deadline_ms.has_value() ||
            args.liveness_stale_timeout_ms.has_value() ||
            args.liveness_dead_timeout_ms.has_value() ||
            args.generation_seed.has_value())
        {
            ExitUsageError(
                "upload/download does not accept generate-config-specific arguments");
        }
        if (args.command == "status")
        {
            if (!args.bucket.empty() || !args.object_key.empty() ||
                !args.object_id.empty() || !args.source_path.empty() ||
                !args.destination_path.empty() || args.version.has_value() ||
                args.chunk_size.has_value() || args.replica_count.has_value() ||
                args.minimum_successful_writes.has_value() ||
                args.concurrency.has_value())
            {
                ExitUsageError(
                    "status does not accept upload/download-specific arguments");
            }
            return args;
        }
        if (args.bucket.empty())
        {
            ExitUsageError("--bucket is required");
        }
        if (args.object_key.empty())
        {
            ExitUsageError("--object is required");
        }

        if (args.command == "upload")
        {
            if (args.source_path.empty())
            {
                ExitUsageError("upload requires --file");
            }
            if (args.version.has_value())
            {
                ExitUsageError("upload does not accept --version");
            }
        }
        else if (args.command == "download")
        {
            if (args.destination_path.empty())
            {
                ExitUsageError("download requires --out");
            }
            if (args.chunk_size.has_value() || args.replica_count.has_value() ||
                args.minimum_successful_writes.has_value())
            {
                ExitUsageError(
                    "download does not accept --chunk-size/--replicas/--min-writes");
            }
        }

        if (!args.object_id.empty())
        {
            std::string validation_error;
            const auto status =
                storedemo::ValidateChunkObjectId(args.object_id, &validation_error);
            if (status != storedemo::StorageNodeStatusCode::kOk)
            {
                ExitUsageError("invalid --object-id: " + validation_error);
            }
        }
        return args;
    }

    void PrintDiagnostics(
        const std::vector<storedemo::ObjectTransferDiagnostic> &diagnostics)
    {
        for (const auto &diagnostic : diagnostics)
        {
            std::cerr << "diagnostic"
                      << " request_id=" << diagnostic.request_id
                      << " status=" << ToString(diagnostic.status);
            if (!diagnostic.node_id.empty())
            {
                std::cerr << " node_id=" << diagnostic.node_id;
            }
            if (!diagnostic.endpoint.empty())
            {
                std::cerr << " endpoint=" << diagnostic.endpoint;
            }
            if (!diagnostic.chunk_id.empty())
            {
                std::cerr << " chunk_id=" << diagnostic.chunk_id;
            }
            if (diagnostic.chunk_index != 0 || diagnostic.offset != 0)
            {
                std::cerr << " chunk_index=" << diagnostic.chunk_index
                          << " offset=" << diagnostic.offset;
            }
            if (diagnostic.retryable)
            {
                std::cerr << " retryable=true";
            }
            if (!diagnostic.message.empty())
            {
                std::cerr << " message=" << diagnostic.message;
            }
            std::cerr << '\n';
        }
    }

    void PrintClusterConfigIssues(
        const std::string_view request_id,
        const clusterdemo::ClusterConfigValidationResult &validation)
    {
        for (const auto &issue : validation.issues)
        {
            std::cerr << "diagnostic";
            if (!request_id.empty())
            {
                std::cerr << " request_id=" << request_id;
            }
            std::cerr << " status="
                      << clusterdemo::ToString(
                             clusterdemo::ClusterConfigStatusCode::kInvalidArgument)
                      << " issue="
                      << clusterdemo::DescribeClusterConfigIssue(issue) << '\n';
        }
    }

    [[nodiscard]] clusterdemo::ChunkPolicyConfig MakeChunkPolicy(
        const ParsedArgs &args)
    {
        clusterdemo::ChunkPolicyConfig policy;
        policy.chunk_size_bytes =
            args.chunk_size.value_or(storedemo::kProductionChunkSizeBytes);
        policy.replica_count = args.replica_count.value_or(3U);
        policy.minimum_successful_writes =
            args.minimum_successful_writes.value_or(2U);
        policy.checksum_algorithm = clusterdemo::ClusterChecksumAlgorithm::kSha256;
        return policy;
    }

    [[nodiscard]] clusterdemo::ClusterTimeoutConfig MakeClusterTimeoutConfig(
        const ParsedArgs &args)
    {
        clusterdemo::ClusterTimeoutConfig timeouts;
        timeouts.discovery_rpc_timeout = std::chrono::milliseconds(
            args.discovery_timeout_ms.value_or(3000ULL));
        timeouts.metadata_rpc_timeout = std::chrono::milliseconds(
            args.metadata_timeout_ms.value_or(3000ULL));
        timeouts.storage_rpc_timeout = std::chrono::milliseconds(
            args.storage_timeout_ms.value_or(3000ULL));
        timeouts.heartbeat_interval = std::chrono::milliseconds(
            args.heartbeat_interval_ms.value_or(1000ULL));
        timeouts.registration_timeout = std::chrono::milliseconds(
            args.registration_timeout_ms.value_or(3000ULL));
        timeouts.commit_deadline = std::chrono::milliseconds(
            args.commit_deadline_ms.value_or(5000ULL));
        timeouts.liveness_stale_timeout = std::chrono::milliseconds(
            args.liveness_stale_timeout_ms.value_or(5000ULL));
        timeouts.liveness_dead_timeout = std::chrono::milliseconds(
            args.liveness_dead_timeout_ms.value_or(15000ULL));
        return timeouts;
    }

    [[nodiscard]] clusterdemo::ClusterConfigGenerationRequest
    MakeGenerationRequest(const ParsedArgs &args)
    {
        const std::size_t metadata_node_count =
            args.metadata_node_count.value_or(3U);

        clusterdemo::ClusterConfigGenerationRequest request;
        request.cluster_id =
            args.cluster_id.empty() ? "cluster-008-local" : args.cluster_id;
        request.base_dir = args.base_dir;
        request.bind_host =
            args.bind_host.empty() ? "127.0.0.1" : args.bind_host;
        request.advertise_host = args.advertise_host;
        request.view_node_count = args.view_node_count.value_or(1U);
        request.metadata_node_count = metadata_node_count;
        request.metadata_voter_count =
            args.metadata_voter_count.value_or(metadata_node_count);
        request.storage_node_count = args.storage_node_count.value_or(3U);
        request.view_port_base = args.view_port_base.value_or(7001U);
        request.metadata_port_base = args.metadata_port_base.value_or(7101U);
        request.storage_port_base = args.storage_port_base.value_or(7201U);
        request.default_storage_capacity_bytes =
            args.storage_capacity_bytes.value_or(64ULL * 1024ULL * 1024ULL *
                                                 1024ULL);
        request.chunk_policy = MakeChunkPolicy(args);
        request.timeouts = MakeClusterTimeoutConfig(args);
        request.generation_seed = args.generation_seed;
        return request;
    }

    void WriteTextFile(const std::filesystem::path &path,
                       const std::string &content)
    {
        // CLI 只负责跨平台路径创建和文本写盘，不接管 cluster/config 业务逻辑。
        if (!path.has_parent_path())
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output.is_open())
            {
                throw std::runtime_error("failed to open output file: " +
                                         path.string());
            }
            output << content;
            if (!output.good())
            {
                throw std::runtime_error("failed to write output file: " +
                                         path.string());
            }
            return;
        }

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            throw std::runtime_error("failed to create output directory: " +
                                     path.parent_path().string() +
                                     " reason=" + ec.message());
        }

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            throw std::runtime_error("failed to open output file: " +
                                     path.string());
        }
        output << content;
        if (!output.good())
        {
            throw std::runtime_error("failed to write output file: " +
                                     path.string());
        }
    }

    [[nodiscard]] int ExitCodeForClusterConfigStatus(
        const clusterdemo::ClusterConfigStatusCode status)
    {
        switch (status)
        {
        case clusterdemo::ClusterConfigStatusCode::kOk:
            return static_cast<int>(CliExitCode::kOk);
        case clusterdemo::ClusterConfigStatusCode::kUnsupported:
            return static_cast<int>(CliExitCode::kUnsupported);
        case clusterdemo::ClusterConfigStatusCode::kInvalidArgument:
        case clusterdemo::ClusterConfigStatusCode::kConflict:
            return static_cast<int>(CliExitCode::kInvalidArgument);
        case clusterdemo::ClusterConfigStatusCode::kInternalError:
        default:
            return static_cast<int>(CliExitCode::kInternalError);
        }
    }

    [[nodiscard]] int RunGenerateConfig(const ParsedArgs &args)
    {
        const auto request = MakeGenerationRequest(args);
        const std::string request_id =
            args.request_id.empty()
                ? GenerateRequestId("generate-config", request.cluster_id)
                : args.request_id;
        const auto result =
            clusterdemo::GenerateDeterministicClusterConfig(request);
        if (!result.ok())
        {
            PrintCommandFailure("generate-config",
                                request_id,
                                clusterdemo::ToString(result.status),
                                result.error_detail);
            PrintClusterConfigIssues(request_id, result.validation);
            return ExitCodeForClusterConfigStatus(result.status);
        }

        const std::string content =
            clusterdemo::SerializeClusterConfigToJson(result.config);
        try
        {
            WriteTextFile(args.output_path, content);
        }
        catch (const std::exception &ex)
        {
            PrintCommandFailure("generate-config",
                                request_id,
                                "FILE_WRITE_ERROR",
                                ex.what());
            return static_cast<int>(CliExitCode::kConfigError);
        }

        PrintCommandSuccess("generate-config", request_id);
        std::cout << " cluster_id=" << result.config.cluster_id
                  << " output=" << args.output_path.string()
                  << " view_nodes=" << result.config.view_nodes.size()
                  << " metadata_nodes=" << result.config.metadata_nodes.size()
                  << " storage_nodes=" << result.config.storage_nodes.size()
                  << " metadata_voters="
                  << result.config.initial_raft_membership.voter_raft_ids.size()
                  << " quorum="
                  << clusterdemo::ComputeInitialRaftQuorumSize(
                         result.config.initial_raft_membership)
                  << '\n';
        if (!result.config.view_nodes.empty())
        {
            std::cout << "leader_discovery_seed"
                      << " endpoint=" << result.config.view_nodes.front().endpoint
                      << '\n';
        }
        return static_cast<int>(CliExitCode::kOk);
    }

    [[nodiscard]] std::shared_ptr<viewdemo::ViewNodeClient> CreateViewClient(
        const ClientConfig &config)
    {
        viewdemo::ViewNodeClientConfig client_config;
        client_config.discovery_timeout = config.discovery_timeout;
        client_config.cluster_view_timeout = config.discovery_timeout;
        return std::make_shared<viewdemo::ViewNodeClient>(
            grpc::CreateChannel(config.view_endpoint,
                                grpc::InsecureChannelCredentials()),
            config.view_endpoint,
            client_config);
    }

    [[nodiscard]] std::shared_ptr<storedemo::MetadataTransferClient>
    CreateMetadataClientSeed(const ClientConfig &config)
    {
        // transfer 内部会通过 ViewNode 重新发现真正的 MetadataNode；
        // 这里的 seed 只用于注入 timeout 等 adapter 配置。
        storedemo::MetadataTransferClientConfig client_config;
        client_config.create_write_plan_timeout = config.metadata_timeout;
        client_config.commit_object_timeout = config.commit_deadline;
        client_config.head_object_timeout = config.metadata_timeout;
        client_config.get_manifest_timeout = config.metadata_timeout;
        return storedemo::CreateGrpcMetadataTransferClient(config.view_endpoint,
                                                           std::move(client_config));
    }

    [[nodiscard]] storedemo::ObjectTransfer CreateObjectTransfer(
        const ClientConfig &config)
    {
        return storedemo::ObjectTransfer(CreateMetadataClientSeed(config),
                                         storedemo::CreateGrpcStorageTransferClient(),
                                         CreateViewClient(config));
    }

    [[nodiscard]] int RunUpload(const ParsedArgs &args,
                                const ClientConfig &config)
    {
        storedemo::ObjectTransfer transfer = CreateObjectTransfer(config);
        auto session = transfer.StartUploadSession(
            {.request_id = args.request_id.empty()
                               ? GenerateRequestId("upload", args.object_key)
                               : args.request_id,
             .cluster_id = config.cluster_id,
             .bucket = args.bucket,
             .object_key = args.object_key,
             .object_id = args.object_id.empty()
                              ? DeriveObjectId(args.bucket, args.object_key)
                              : args.object_id,
             .source_path = args.source_path,
             .chunk_size = args.chunk_size.value_or(config.chunk_size),
             .concurrency = args.concurrency.value_or(1),
             .desired_replica_count =
                 args.replica_count.value_or(config.replica_count),
             .minimum_successful_writes =
                 args.minimum_successful_writes.value_or(
                     config.minimum_successful_writes),
             .client_time_unix_ms = static_cast<std::uint64_t>(
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count())});

        auto reader = storedemo::CreateFileTransferChunkReader();
        auto checksum_state = storedemo::CreateTransferChecksumState();
        const storedemo::UploadObjectResult result =
            session->Execute(*reader, *checksum_state);

        // app 层不能把“仅拿到 write plan”误报为上传成功；
        // 只有 transfer 明确给出 committed=true 才能返回 0。
        if (!result.ok() || !result.committed)
        {
            const auto effective_status =
                result.ok() && !result.committed
                    ? storedemo::ObjectTransferStatusCode::kUnsupported
                    : result.status;
            const std::string effective_error =
                !result.error_detail.empty()
                    ? result.error_detail
                    : "upload session returned without a COMMITTED object; app must not report success before transfer finishes chunk writes and CommitObject";

            std::cerr << "upload FAILED"
                      << " request_id=" << result.session.request_id
                      << " status=" << ToString(effective_status)
                      << " message=" << effective_error << '\n';
            if (result.write_plan.has_value())
            {
                std::cerr << "partial_result"
                          << " object_id=" << result.write_plan->object_id
                          << " version=" << result.write_plan->version
                          << " size=" << result.write_plan->object_checksum.size
                          << " chunk_count=" << result.prepared_chunks.size()
                          << '\n';
            }
            PrintDiagnostics(result.diagnostics);
            return ExitCodeForTransferStatus(effective_status);
        }

        const auto &manifest = result.committed_manifest.value();
        std::cout << "upload OK"
                  << " request_id=" << result.session.request_id
                  << " object_id=" << manifest.object_id
                  << " version=" << manifest.version
                  << " size=" << manifest.object_checksum.size
                  << " checksum=" << manifest.object_checksum.checksum.value
                  << " chunk_count=" << manifest.chunks.size() << '\n';
        PrintDiagnostics(result.diagnostics);
        return static_cast<int>(CliExitCode::kOk);
    }

    [[nodiscard]] int RunDownload(const ParsedArgs &args,
                                  const ClientConfig &config)
    {
        storedemo::ObjectTransfer transfer = CreateObjectTransfer(config);
        auto session = transfer.StartDownloadSession(
            {.request_id = args.request_id.empty()
                               ? GenerateRequestId("download", args.object_key)
                               : args.request_id,
             .cluster_id = config.cluster_id,
             .bucket = args.bucket,
             .object_key = args.object_key,
             .object_id = args.object_id,
             .version = args.version,
             .destination_path = args.destination_path,
             .concurrency = args.concurrency.value_or(1)});

        auto checksum_state = storedemo::CreateTransferChecksumState();
        const storedemo::DownloadObjectResult result =
            session->Execute(*checksum_state);

        // 只有最终对象 checksum 已验证通过，CLI 才能输出 PASS。
        if (!result.ok() || !result.checksum_verified)
        {
            const auto effective_status =
                result.ok() && !result.checksum_verified
                    ? storedemo::ObjectTransferStatusCode::kChecksumMismatch
                    : result.status;
            const std::string effective_error =
                !result.error_detail.empty()
                    ? result.error_detail
                    : "download session completed without final checksum verification";
            std::cerr << "download FAILED"
                      << " request_id=" << result.session.request_id
                      << " status=" << ToString(effective_status)
                      << " message=" << effective_error << '\n';
            PrintDiagnostics(result.diagnostics);
            return ExitCodeForTransferStatus(effective_status);
        }

        const auto &manifest = result.manifest.value();
        std::cout << "download OK"
                  << " request_id=" << result.session.request_id
                  << " object_id=" << manifest.object_id
                  << " version=" << manifest.version
                  << " size=" << result.downloaded_object_checksum.size
                  << " checksum="
                  << result.downloaded_object_checksum.checksum.value
                  << " integrity=PASS"
                  << " output=" << args.destination_path.string() << '\n';
        PrintDiagnostics(result.diagnostics);
        return static_cast<int>(CliExitCode::kOk);
    }

    [[nodiscard]] int ExitCodeForViewRegistryStatus(
        const viewdemo::ViewRegistryStatusCode status)
    {
        switch (status)
        {
        case viewdemo::ViewRegistryStatusCode::kOk:
        case viewdemo::ViewRegistryStatusCode::kIdempotentReplay:
        case viewdemo::ViewRegistryStatusCode::kStaleIgnored:
            return static_cast<int>(CliExitCode::kOk);
        case viewdemo::ViewRegistryStatusCode::kInvalidArgument:
        case viewdemo::ViewRegistryStatusCode::kNotFound:
        case viewdemo::ViewRegistryStatusCode::kConflict:
            return static_cast<int>(CliExitCode::kConfigError);
        case viewdemo::ViewRegistryStatusCode::kUnsupported:
            return static_cast<int>(CliExitCode::kUnsupported);
        case viewdemo::ViewRegistryStatusCode::kTimeout:
        case viewdemo::ViewRegistryStatusCode::kOverloaded:
        case viewdemo::ViewRegistryStatusCode::kServiceUnavailable:
            return static_cast<int>(CliExitCode::kTransferFailure);
        case viewdemo::ViewRegistryStatusCode::kInternalError:
        default:
            return static_cast<int>(CliExitCode::kInternalError);
        }
    }

    void PrintViewDiagnostics(
        const std::vector<viewdemo::ViewRegistryDiagnostic> &diagnostics)
    {
        for (const auto &diagnostic : diagnostics)
        {
            std::cerr << "diagnostic"
                      << " request_id=" << diagnostic.request_id
                      << " status=" << viewdemo::ToString(diagnostic.code);
            if (!diagnostic.cluster_id.empty())
            {
                std::cerr << " cluster_id=" << diagnostic.cluster_id;
            }
            if (!diagnostic.node_id.empty())
            {
                std::cerr << " node_id=" << diagnostic.node_id;
            }
            if (!diagnostic.endpoint.empty())
            {
                std::cerr << " endpoint=" << diagnostic.endpoint;
            }
            if (diagnostic.sequence != 0)
            {
                std::cerr << " sequence=" << diagnostic.sequence;
            }
            if (!diagnostic.message.empty())
            {
                std::cerr << " message=" << diagnostic.message;
            }
            std::cerr << '\n';
        }
    }

    void AppendLeaderHintFields(
        std::ostream &out,
        const std::optional<viewdemo::MetadataLeaderHint> &leader_hint)
    {
        if (!leader_hint.has_value())
        {
            return;
        }

        out << " leader_hint.node_id=" << leader_hint->node_id;
        if (leader_hint->raft_id.has_value())
        {
            out << " leader_hint.raft_id=" << *leader_hint->raft_id;
        }
        if (!leader_hint->endpoint.empty())
        {
            out << " leader_hint.endpoint=" << leader_hint->endpoint;
        }
        if (leader_hint->observed_term != 0)
        {
            out << " leader_hint.term=" << leader_hint->observed_term;
        }
        if (leader_hint->observed_at_unix_ms != 0)
        {
            out << " leader_hint.observed_at_unix_ms="
                << leader_hint->observed_at_unix_ms;
        }
    }

    void PrintViewNodeSnapshot(std::string_view prefix,
                               const viewdemo::ViewNodeSnapshot &node)
    {
        std::cout << prefix
                  << " node_id=" << node.node_id
                  << " node_type=" << viewdemo::ToString(node.node_type)
                  << " endpoint=" << node.endpoint
                  << " liveness=" << viewdemo::ToString(node.liveness)
                  << " health=" << viewdemo::ToString(node.health.health)
                  << " disk_pressure="
                  << viewdemo::ToString(node.health.disk_pressure)
                  << " last_seen_unix_ms=" << node.last_seen_unix_ms
                  << " last_sequence=" << node.last_sequence;

        if (!node.control_plane_endpoint.empty())
        {
            std::cout << " control_plane_endpoint="
                      << node.control_plane_endpoint;
        }
        if (!node.data_plane_endpoint.empty())
        {
            std::cout << " data_plane_endpoint="
                      << node.data_plane_endpoint;
        }
        if (!node.failure_domain.zone.empty())
        {
            std::cout << " zone=" << node.failure_domain.zone;
        }
        if (!node.failure_domain.rack.empty())
        {
            std::cout << " rack=" << node.failure_domain.rack;
        }
        std::cout << '\n';
    }

    void PrintMetadataNodeSnapshot(const viewdemo::ViewNodeSnapshot &node)
    {
        std::cout << "metadata_node"
                  << " node_id=" << node.node_id
                  << " endpoint=" << node.endpoint
                  << " liveness=" << viewdemo::ToString(node.liveness)
                  << " health=" << viewdemo::ToString(node.health.health)
                  << " disk_pressure="
                  << viewdemo::ToString(node.health.disk_pressure)
                  << " last_seen_unix_ms=" << node.last_seen_unix_ms
                  << " last_sequence=" << node.last_sequence;

        if (node.metadata.has_value())
        {
            const auto &metadata = *node.metadata;
            if (metadata.raft_id.has_value())
            {
                std::cout << " raft_id=" << *metadata.raft_id;
            }
            std::cout << " raft_role="
                      << viewdemo::ToString(metadata.raft_role)
                      << " membership_observation="
                      << viewdemo::ToString(metadata.membership_state);
            if (metadata.observed_term != 0)
            {
                std::cout << " observed_term=" << metadata.observed_term;
            }
            if (metadata.commit_index != 0)
            {
                std::cout << " commit_index=" << metadata.commit_index;
            }
            if (metadata.membership_epoch != 0)
            {
                std::cout << " membership_epoch="
                          << metadata.membership_epoch;
            }
            AppendLeaderHintFields(std::cout, metadata.leader_hint);
        }

        std::cout << '\n';
    }

    void PrintStorageNodeSnapshot(const viewdemo::ViewNodeSnapshot &node)
    {
        std::cout << "storage_node"
                  << " node_id=" << node.node_id
                  << " endpoint=" << node.endpoint
                  << " liveness=" << viewdemo::ToString(node.liveness)
                  << " health=" << viewdemo::ToString(node.health.health)
                  << " disk_pressure="
                  << viewdemo::ToString(node.health.disk_pressure)
                  << " total_capacity_bytes="
                  << node.capacity.total_capacity_bytes
                  << " used_capacity_bytes="
                  << node.capacity.used_capacity_bytes
                  << " available_capacity_bytes="
                  << node.capacity.available_capacity_bytes
                  << " chunk_count=" << node.capacity.chunk_count
                  << " active_reads=" << node.load.active_reads
                  << " active_writes=" << node.load.active_writes
                  << " queued_ops=" << node.load.queued_ops
                  << " write_admission_overloaded="
                  << (node.load.write_admission_overloaded ? "true" : "false")
                  << " read_admission_overloaded="
                  << (node.load.read_admission_overloaded ? "true" : "false")
                  << " last_seen_unix_ms=" << node.last_seen_unix_ms
                  << " last_sequence=" << node.last_sequence;

        if (!node.failure_domain.zone.empty())
        {
            std::cout << " zone=" << node.failure_domain.zone;
        }
        if (!node.failure_domain.rack.empty())
        {
            std::cout << " rack=" << node.failure_domain.rack;
        }
        std::cout << '\n';
    }

    [[nodiscard]] int RunStatus(const ParsedArgs &args,
                                const ClientConfig &config)
    {
        const std::string request_id =
            args.request_id.empty()
                ? GenerateRequestId("status", config.cluster_id)
                : args.request_id;

        auto client = CreateViewClient(config);
        // status 只读取 ViewNode cluster view 做 discovery/observation 诊断，
        // 不能把这些观测结果解释为 Raft membership 或 object manifest authority。
        const auto result = client->GetClusterView(
            viewdemo::GetClusterViewRequest{
                .request_id = request_id,
                .cluster_id = config.cluster_id,
                .include_dead_nodes = true,
                .include_warnings = true,
            });

        if (!result.transport_ok())
        {
            std::cerr << "status FAILED"
                      << " request_id=" << request_id
                      << " target_endpoint=" << config.view_endpoint
                      << " status=GRPC_TRANSPORT_ERROR"
                      << " grpc_code="
                      << static_cast<int>(result.rpc.grpc_status_code)
                      << " retryable="
                      << (result.rpc.retryable ? "true" : "false")
                      << " message=" << result.rpc.grpc_error_message
                      << '\n';
            return static_cast<int>(CliExitCode::kTransferFailure);
        }

        if (!result.result.ok())
        {
            std::cerr << "status FAILED"
                      << " request_id=" << request_id
                      << " cluster_id=" << config.cluster_id
                      << " target_endpoint=" << config.view_endpoint
                      << " status="
                      << viewdemo::ToString(result.result.summary.status)
                      << " message=" << result.result.summary.message
                      << '\n';
            PrintViewDiagnostics(result.result.snapshot.diagnostics);
            return ExitCodeForViewRegistryStatus(
                result.result.summary.status);
        }

        const auto &snapshot = result.result.snapshot;
        if (snapshot.view_nodes.empty() && snapshot.metadata_nodes.empty() &&
            snapshot.storage_nodes.empty())
        {
            std::cerr << "status FAILED"
                      << " request_id=" << request_id
                      << " cluster_id=" << config.cluster_id
                      << " target_endpoint=" << config.view_endpoint
                      << " status=EMPTY_CLUSTER_VIEW"
                      << " message=ViewNode returned an empty observed cluster view"
                      << '\n';
            PrintViewDiagnostics(snapshot.diagnostics);
            return static_cast<int>(CliExitCode::kTransferFailure);
        }

        std::cout << "status OK"
                  << " request_id=" << request_id
                  << " cluster_id=" << config.cluster_id
                  << " target_endpoint=" << config.view_endpoint
                  << " observed_at_unix_ms=" << snapshot.observed_at_unix_ms
                  << " view_nodes=" << snapshot.view_nodes.size()
                  << " metadata_nodes=" << snapshot.metadata_nodes.size()
                  << " storage_nodes=" << snapshot.storage_nodes.size()
                  << '\n';

        if (result.result.summary.retry_after_ms != 0)
        {
            std::cout << "status_hint"
                      << " request_id=" << request_id
                      << " retry_after_ms="
                      << result.result.summary.retry_after_ms << '\n';
        }

        if (snapshot.leader_hint.has_value())
        {
            std::cout << "leader_hint";
            AppendLeaderHintFields(std::cout, snapshot.leader_hint);
            std::cout << '\n';
        }

        std::cout << "non_authority_boundary"
                  << " membership_observation_source=view_node"
                  << " raft_membership_authority=false"
                  << " object_manifest_authority=false"
                  << '\n';

        for (const auto &node : snapshot.view_nodes)
        {
            PrintViewNodeSnapshot("view_node", node);
        }
        for (const auto &node : snapshot.metadata_nodes)
        {
            PrintMetadataNodeSnapshot(node);
        }
        for (const auto &node : snapshot.storage_nodes)
        {
            PrintStorageNodeSnapshot(node);
        }

        PrintViewDiagnostics(snapshot.diagnostics);
        return static_cast<int>(CliExitCode::kOk);
    }
} // namespace

int main(int argc, char **argv)
{
    ParsedArgs args;
    bool parsed_args = false;
    try
    {
        args = ParseArgs(argc, argv);
        parsed_args = true;
        if (args.command == "generate-config")
        {
            return RunGenerateConfig(args);
        }

        const ClientConfig config = LoadClientConfig(args.config_path);

        if (args.command == "upload")
        {
            return RunUpload(args, config);
        }
        if (args.command == "download")
        {
            return RunDownload(args, config);
        }
        if (args.command == "status")
        {
            return RunStatus(args, config);
        }

        std::cerr << "unsupported command: " << args.command << '\n';
        return static_cast<int>(CliExitCode::kInvalidArgument);
    }
    catch (const ClientConfigError &ex)
    {
        PrintCommandFailure(parsed_args ? args.command : "storage_client",
                            parsed_args ? args.request_id : "",
                            "CONFIG_ERROR",
                            ex.what());
        return static_cast<int>(CliExitCode::kConfigError);
    }
    catch (const std::exception &ex)
    {
        PrintCommandFailure(parsed_args ? args.command : "storage_client",
                            parsed_args ? args.request_id : "",
                            "INTERNAL_ERROR",
                            ex.what());
        return static_cast<int>(CliExitCode::kInternalError);
    }
}
