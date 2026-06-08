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
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    struct ClientConfig
    {
        std::filesystem::path config_path;
        std::string cluster_id;
        std::string view_endpoint;
        std::uint64_t chunk_size{4ULL * 1024ULL * 1024ULL};
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
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::filesystem::path source_path;
        std::filesystem::path destination_path;
        std::optional<std::uint64_t> version;
        std::string request_id;
        std::optional<std::uint64_t> chunk_size;
        std::optional<std::uint32_t> replica_count;
        std::optional<std::uint32_t> minimum_successful_writes;
        std::optional<std::uint32_t> concurrency;
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

            if (const auto value = ExtractUnsignedConfigValue(content, "chunk_size_bytes");
                value.has_value() && *value > 0)
            {
                config.chunk_size = *value;
            }
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
            << "  storage_client upload --config <path> --bucket <bucket>"
            << " --object <key> --file <source>"
            << " [--object-id <id>] [--request-id <id>]"
            << " [--chunk-size <bytes>] [--replicas <n>] [--min-writes <n>]"
            << " [--concurrency <n>]\n"
            << "  storage_client download --config <path> --bucket <bucket>"
            << " --object <key> --out <destination>"
            << " [--object-id <id>] [--version <n>] [--request-id <id>]"
            << " [--concurrency <n>]\n";
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
        if (args.command != "upload" && args.command != "download")
        {
            ExitUsageError("unsupported command: " + args.command +
                           " (T037 only implements upload/download)");
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
                args.destination_path = require_value("--out");
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
            else if (flag == "--chunk-size")
            {
                args.chunk_size =
                    ParseUnsignedOrDie(require_value("--chunk-size"),
                                       "--chunk-size");
            }
            else if (flag == "--replicas")
            {
                args.replica_count = static_cast<std::uint32_t>(
                    ParseUnsignedOrDie(require_value("--replicas"),
                                       "--replicas"));
            }
            else if (flag == "--min-writes")
            {
                args.minimum_successful_writes = static_cast<std::uint32_t>(
                    ParseUnsignedOrDie(require_value("--min-writes"),
                                       "--min-writes"));
            }
            else if (flag == "--concurrency")
            {
                args.concurrency = static_cast<std::uint32_t>(
                    ParseUnsignedOrDie(require_value("--concurrency"),
                                       "--concurrency"));
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

        if (args.config_path.empty())
        {
            ExitUsageError("--config is required");
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

    [[nodiscard]] std::shared_ptr<viewdemo::ViewNodeClient> CreateViewClient(
        const ClientConfig &config)
    {
        viewdemo::ViewNodeClientConfig client_config;
        client_config.discovery_timeout = config.discovery_timeout;
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
} // namespace

int main(int argc, char **argv)
{
    try
    {
        const ParsedArgs args = ParseArgs(argc, argv);
        const ClientConfig config = LoadClientConfig(args.config_path);

        if (args.command == "upload")
        {
            return RunUpload(args, config);
        }
        if (args.command == "download")
        {
            return RunDownload(args, config);
        }

        std::cerr << "unsupported command: " << args.command << '\n';
        return static_cast<int>(CliExitCode::kInvalidArgument);
    }
    catch (const ClientConfigError &ex)
    {
        std::cerr << "storage_client config error: " << ex.what() << '\n';
        return static_cast<int>(CliExitCode::kConfigError);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "storage_client error: " << ex.what() << '\n';
        return static_cast<int>(CliExitCode::kInternalError);
    }
}
