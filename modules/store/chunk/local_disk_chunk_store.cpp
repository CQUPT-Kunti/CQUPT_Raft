#include "store/chunk/local_disk_chunk_store.h"

#include <chrono>
#include <iomanip>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

#include "store/index/chunk_index.h"
#include "store/io/durable_file.h"

namespace storedemo
{
    namespace
    {
        constexpr const char *kChunksDirectoryRelativePath = "chunks";
        constexpr const char *kLiveDirectoryRelativePath = "chunks/live";
        constexpr const char *kStagingDirectoryRelativePath = "chunks/staging";

        std::uint64_t CurrentUnixTimeMillis()
        {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
        }

        std::string BuildHexToken(const std::string_view input)
        {
            std::uint64_t hash = 1469598103934665603ULL;
            for (const unsigned char ch : input)
            {
                hash ^= static_cast<std::uint64_t>(ch);
                hash *= 1099511628211ULL;
            }

            std::ostringstream stream;
            stream << "rq" << std::hex << std::setfill('0') << std::setw(16) << hash;
            return stream.str();
        }

        bool HasExpectedChecksumConstraint(const ChunkChecksum &checksum)
        {
            return checksum.algorithm != ChunkChecksumAlgorithm::kUnknown ||
                   !checksum.value.empty() ||
                   checksum.size_bytes != 0 ||
                   checksum.computed_at != 0;
        }

        bool HasRequiredDurableBoundary(const DurableFileResult &result)
        {
            return result.ok() && result.durable_boundary_reached;
        }

        StorageNodeStatusCode MapFilesystemErrorToStatus(const std::error_code &error)
        {
            if (!error)
            {
                return StorageNodeStatusCode::kOk;
            }

            if (error == std::errc::no_space_on_device ||
                error == std::errc::file_too_large)
            {
                return StorageNodeStatusCode::kDiskFull;
            }

            if (error == std::errc::permission_denied ||
                error == std::errc::operation_not_permitted ||
                error == std::errc::read_only_file_system)
            {
                return StorageNodeStatusCode::kPermissionDenied;
            }

            if (error == std::errc::invalid_argument ||
                error == std::errc::no_such_file_or_directory ||
                error == std::errc::not_a_directory ||
                error == std::errc::filename_too_long ||
                error == std::errc::too_many_symbolic_link_levels)
            {
                return StorageNodeStatusCode::kInvalidArgument;
            }

            return StorageNodeStatusCode::kIoError;
        }

        std::string BuildFilesystemErrorDetail(const char *operation,
                                               const std::filesystem::path &path,
                                               const std::error_code &error)
        {
            return std::string(operation) + " failed for " + path.string() +
                   ": " + error.message();
        }

        StorageNodeStatusCode EnsureDirectoryExists(const std::filesystem::path &path,
                                                    std::string *error_detail)
        {
            std::error_code exists_error;
            const bool exists = std::filesystem::exists(path, exists_error);
            if (exists_error)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        BuildFilesystemErrorDetail("exists", path, exists_error);
                }
                return MapFilesystemErrorToStatus(exists_error);
            }

            if (exists)
            {
                std::error_code directory_error;
                const bool is_directory =
                    std::filesystem::is_directory(path, directory_error);
                if (directory_error)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = BuildFilesystemErrorDetail(
                            "is_directory", path, directory_error);
                    }
                    return MapFilesystemErrorToStatus(directory_error);
                }

                if (!is_directory)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = "path exists but is not a directory: " +
                                        path.string();
                    }
                    return StorageNodeStatusCode::kInvalidArgument;
                }

                return StorageNodeStatusCode::kOk;
            }

            std::error_code create_error;
            std::filesystem::create_directories(path, create_error);
            if (create_error)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = BuildFilesystemErrorDetail(
                        "create_directories", path, create_error);
                }
                return MapFilesystemErrorToStatus(create_error);
            }

            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode ResolveStorePath(const std::filesystem::path &data_root,
                                               const std::filesystem::path &relative_path,
                                               std::filesystem::path *resolved_path,
                                               std::string *error_detail)
        {
            return ResolveDurablePathUnderRoot(data_root,
                                               relative_path,
                                               resolved_path,
                                               error_detail);
        }

        std::shared_ptr<DurableFile> CreateDefaultDurableFile(
            const std::filesystem::path &data_root)
        {
#if defined(__linux__)
            return std::make_shared<LinuxDurableFile>(data_root);
#elif defined(_WIN32)
            return std::make_shared<WindowsDurableFile>(data_root);
#else
            return {};
#endif
        }

        ChunkMetadata BuildChunkMetadata(const LocalDiskChunkStoreConfig &config,
                                         const ChunkIdentity &identity,
                                         const std::uint64_t size,
                                         const ChunkChecksum &checksum,
                                         const ChunkState state,
                                         const std::string &write_request_id,
                                         const std::uint64_t timestamp_ms)
        {
            ChunkMetadata metadata;
            metadata.identity = identity;
            metadata.node_id = config.node_id;
            metadata.size = size;
            metadata.checksum = checksum;
            metadata.state = state;
            metadata.write_request_id = write_request_id;
            metadata.created_at = timestamp_ms;
            metadata.published_at = timestamp_ms;
            metadata.last_error = StorageNodeStatusCode::kOk;
            return metadata;
        }

        ChunkMetadata BuildChunkMetadataFromIndexEntry(
            const LocalDiskChunkStoreConfig &config,
            const ChunkIndexEntry &entry,
            const std::string &write_request_id)
        {
            ChunkMetadata metadata;
            metadata.identity = entry.identity;
            metadata.node_id = config.node_id;
            metadata.size = entry.size;
            metadata.checksum = entry.checksum;
            metadata.state = entry.state;
            metadata.write_request_id = write_request_id;
            metadata.created_at = entry.updated_at;
            metadata.published_at = entry.updated_at;
            metadata.last_error = StorageNodeStatusCode::kOk;
            return metadata;
        }

        ChunkIndexEntry BuildChunkIndexEntry(const ChunkMetadata &metadata,
                                             const std::filesystem::path &final_path,
                                             const std::uint64_t timestamp_ms)
        {
            ChunkIndexEntry entry;
            entry.identity = metadata.identity;
            entry.state = metadata.state;
            entry.size = metadata.size;
            entry.checksum = metadata.checksum;
            entry.final_path = final_path;
            entry.updated_at = timestamp_ms;
            return entry;
        }

        StorageNodeStatusCode PrepareWriteIdentity(const WriteChunkRequest &request,
                                                   ChunkIdentity *out_identity,
                                                   std::string *error_detail)
        {
            if (out_identity == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "write identity output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (request.identity.chunk_id.empty())
            {
                if (request.identity.object_id.empty() || request.identity.version == 0)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail =
                            "write request must include chunk_id or full chunk identity";
                    }
                    return StorageNodeStatusCode::kInvalidArgument;
                }

                ChunkId chunk_id;
                const auto make_status = MakeChunkId(request.identity.object_id,
                                                     request.identity.version,
                                                     request.identity.chunk_index,
                                                     &chunk_id,
                                                     error_detail);
                if (make_status != StorageNodeStatusCode::kOk)
                {
                    return make_status;
                }

                out_identity->chunk_id = std::move(chunk_id);
                out_identity->object_id = request.identity.object_id;
                out_identity->version = request.identity.version;
                out_identity->chunk_index = request.identity.chunk_index;
                out_identity->offset = request.identity.offset;
                return StorageNodeStatusCode::kOk;
            }

            ChunkIdentity parsed_identity;
            const auto parse_status =
                ParseChunkId(request.identity.chunk_id, &parsed_identity, error_detail);
            if (parse_status != StorageNodeStatusCode::kOk)
            {
                return parse_status;
            }

            if (!request.identity.object_id.empty() &&
                request.identity.object_id != parsed_identity.object_id)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "chunk_id does not match object_id";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (request.identity.version != 0 &&
                request.identity.version != parsed_identity.version)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "chunk_id does not match version";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (request.identity.chunk_index != 0 &&
                request.identity.chunk_index != parsed_identity.chunk_index)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "chunk_id does not match chunk_index";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            parsed_identity.offset = request.identity.offset;
            *out_identity = std::move(parsed_identity);
            return StorageNodeStatusCode::kOk;
        }

        template <typename Response>
        Response MakeUnsupportedResponse(const char *operation)
        {
            Response response;
            response.status = StorageNodeStatusCode::kUnsupported;
            response.error_detail =
                std::string("LocalDiskChunkStore::") + operation +
                " is not implemented in the current LocalDiskChunkStore stage";
            return response;
        }
    }

    bool LocalDiskChunkStorePaths::IsInitialized() const
    {
        return !data_root.empty() &&
               !chunks_root.empty() &&
               !live_root.empty() &&
               !staging_root.empty();
    }

    LocalDiskChunkStore::LocalDiskChunkStore(LocalDiskChunkStoreConfig config)
        : config_(std::move(config))
    {
    }

    LocalDiskChunkStore::~LocalDiskChunkStore() = default;

    LocalDiskChunkStoreInitResult LocalDiskChunkStore::Initialize()
    {
        LocalDiskChunkStoreInitResult result;
        if (initialized_)
        {
            result.paths = paths_;
            result.initialized = true;
            return result;
        }

        if (config_.node_id.empty())
        {
            result.status = StorageNodeStatusCode::kInvalidArgument;
            result.error_detail = "LocalDiskChunkStore node_id must not be empty";
            return result;
        }

        if (config_.data_dir.empty())
        {
            result.status = StorageNodeStatusCode::kInvalidArgument;
            result.error_detail = "LocalDiskChunkStore data_dir must not be empty";
            return result;
        }

        LocalDiskChunkStorePaths candidate_paths;
        std::error_code absolute_error;
        candidate_paths.data_root =
            std::filesystem::absolute(config_.data_dir, absolute_error).lexically_normal();
        if (absolute_error)
        {
            result.status = MapFilesystemErrorToStatus(absolute_error);
            result.error_detail = BuildFilesystemErrorDetail(
                "absolute", config_.data_dir, absolute_error);
            return result;
        }
        if (candidate_paths.data_root.empty())
        {
            result.status = StorageNodeStatusCode::kInvalidArgument;
            result.error_detail = "LocalDiskChunkStore data_dir resolved to empty path";
            return result;
        }

        auto status = ResolveStorePath(candidate_paths.data_root,
                                       kChunksDirectoryRelativePath,
                                       &candidate_paths.chunks_root,
                                       &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            result.status = status;
            return result;
        }

        status = ResolveStorePath(candidate_paths.data_root,
                                  kLiveDirectoryRelativePath,
                                  &candidate_paths.live_root,
                                  &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            result.status = status;
            return result;
        }

        status = ResolveStorePath(candidate_paths.data_root,
                                  kStagingDirectoryRelativePath,
                                  &candidate_paths.staging_root,
                                  &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            result.status = status;
            return result;
        }

        if (config_.chunk_index == nullptr)
        {
            config_.chunk_index = std::make_shared<ShardedChunkIndex>();
        }

        if (config_.durable_file == nullptr)
        {
            config_.durable_file = CreateDefaultDurableFile(candidate_paths.data_root);
            if (config_.durable_file == nullptr)
            {
                result.status = StorageNodeStatusCode::kUnsupported;
                result.error_detail =
                    "LocalDiskChunkStore has no default durable file implementation for this platform";
                return result;
            }
        }

        status = EnsureDirectoryExists(candidate_paths.data_root, &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            result.status = status;
            return result;
        }

        status = EnsureDirectoryExists(candidate_paths.chunks_root, &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            result.status = status;
            return result;
        }

        status = EnsureDirectoryExists(candidate_paths.live_root, &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            result.status = status;
            return result;
        }

        status = EnsureDirectoryExists(candidate_paths.staging_root, &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            result.status = status;
            return result;
        }

        paths_ = std::move(candidate_paths);
        initialized_ = true;

        result.paths = paths_;
        result.initialized = true;
        return result;
    }

    const LocalDiskChunkStoreConfig &LocalDiskChunkStore::config() const
    {
        return config_;
    }

    const LocalDiskChunkStorePaths &LocalDiskChunkStore::paths() const
    {
        return paths_;
    }

    bool LocalDiskChunkStore::initialized() const
    {
        return initialized_;
    }

    DurableFile *LocalDiskChunkStore::durable_file() const
    {
        return config_.durable_file.get();
    }

    ChunkIndex *LocalDiskChunkStore::chunk_index() const
    {
        return config_.chunk_index.get();
    }

    BoundedStorageExecutor *LocalDiskChunkStore::executor() const
    {
        return config_.executor.get();
    }

    WriteChunkResponse LocalDiskChunkStore::WriteChunk(const WriteChunkRequest &request)
    {
        WriteChunkResponse response;

        if (!initialized_)
        {
            const auto init_result = Initialize();
            if (!init_result.ok())
            {
                response.status = init_result.status;
                response.error_detail = init_result.error_detail;
                response.retry_after_ms = init_result.retry_after_ms;
                return response;
            }
        }

        if (request.request_id.empty())
        {
            response.status = StorageNodeStatusCode::kInvalidArgument;
            response.error_detail = "WriteChunk request_id must not be empty";
            return response;
        }

        ChunkIdentity identity;
        response.status = PrepareWriteIdentity(request, &identity, &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        if (request.expected_size.has_value() &&
            *request.expected_size != static_cast<std::uint64_t>(request.payload.size()))
        {
            response.status = StorageNodeStatusCode::kInvalidArgument;
            response.error_detail = "WriteChunk payload size does not match expected_size";
            return response;
        }

        ChunkChecksum actual_checksum;
        if (HasExpectedChecksumConstraint(request.expected_checksum))
        {
            response.status = VerifyChunkChecksum(request.payload,
                                                  request.expected_checksum,
                                                  &actual_checksum,
                                                  &response.error_detail);
        }
        else
        {
            response.status = ComputeChunkChecksum(request.payload,
                                                   &actual_checksum,
                                                   &response.error_detail);
        }
        if (!response.ok())
        {
            return response;
        }

        if (config_.chunk_index == nullptr)
        {
            response.status = StorageNodeStatusCode::kUnsupported;
            response.error_detail = "WriteChunk requires a valid ChunkIndex";
            return response;
        }

        if (config_.durable_file == nullptr)
        {
            response.status = StorageNodeStatusCode::kUnsupported;
            response.error_detail = "WriteChunk requires a valid DurableFile";
            return response;
        }

        auto lock_response = config_.chunk_index->AcquireChunkLock(identity.chunk_id);
        if (!lock_response.ok() || !lock_response.acquired)
        {
            response.status = lock_response.status;
            response.error_detail = lock_response.error_detail;
            response.retry_after_ms = lock_response.retry_after_ms;
            return response;
        }

        const auto existing_entry = config_.chunk_index->Find(identity.chunk_id);
        if (existing_entry.ok() && existing_entry.found)
        {
            response.metadata =
                BuildChunkMetadataFromIndexEntry(config_, existing_entry.entry, request.request_id);
            if (existing_entry.entry.state == ChunkState::kLive &&
                existing_entry.entry.size ==
                    static_cast<std::uint64_t>(request.payload.size()) &&
                existing_entry.entry.checksum.algorithm == actual_checksum.algorithm &&
                existing_entry.entry.checksum.value == actual_checksum.value &&
                existing_entry.entry.checksum.size_bytes == actual_checksum.size_bytes)
            {
                response.status = StorageNodeStatusCode::kOk;
                response.durable = true;
                response.already_exists = true;
                return response;
            }

            response.status = StorageNodeStatusCode::kConflict;
            response.error_detail =
                "WriteChunk conflicts with existing local chunk content or state";
            return response;
        }

        if (existing_entry.status != StorageNodeStatusCode::kNotFound)
        {
            response.status = existing_entry.status;
            response.error_detail = existing_entry.error_detail;
            response.retry_after_ms = existing_entry.retry_after_ms;
            return response;
        }

        ChunkPathLayout layout;
        response.status = BuildChunkPathLayout(identity.chunk_id,
                                               BuildHexToken(request.request_id),
                                               &layout,
                                               &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        std::filesystem::path final_path;
        response.status = ResolveStorePath(paths_.data_root,
                                           layout.final_relative_path,
                                           &final_path,
                                           &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        auto open_response = config_.durable_file->OpenStagingWriter(OpenStagingWriterRequest{
            .relative_path = layout.staging_relative_path,
            .expected_size = static_cast<std::uint64_t>(request.payload.size()),
            .context = {}});
        if (!open_response.ok())
        {
            response.status = open_response.status_code();
            response.error_detail = open_response.error_detail;
            response.retry_after_ms = open_response.retry_after_ms;
            return response;
        }

        if (open_response.writer == nullptr)
        {
            response.status = StorageNodeStatusCode::kIoError;
            response.error_detail = "OpenStagingWriter succeeded without returning a writer";
            return response;
        }

        const auto payload_bytes =
            std::as_bytes(std::span(request.payload.data(), request.payload.size()));
        const auto append_result = open_response.writer->Append(DurableAppendRequest{
            .buffer = payload_bytes,
            .context = {}});
        if (!append_result.ok())
        {
            response.status = append_result.status_code();
            response.error_detail = append_result.error_detail;
            response.retry_after_ms = append_result.retry_after_ms;
            return response;
        }

        const auto flush_result = open_response.writer->Flush(DurableFlushRequest{
            .mode = DurableFlushMode::kDataOnly,
            .context = {}});
        if (!HasRequiredDurableBoundary(flush_result))
        {
            response.status = flush_result.ok() ? StorageNodeStatusCode::kIoError
                                                : flush_result.status_code();
            response.error_detail = flush_result.ok()
                                        ? "WriteChunk flush did not reach required durable boundary"
                                        : flush_result.error_detail;
            response.retry_after_ms = flush_result.retry_after_ms;
            return response;
        }

        const auto close_result = open_response.writer->Close(DurableCloseRequest{});
        if (!close_result.ok())
        {
            response.status = close_result.status_code();
            response.error_detail = close_result.error_detail;
            response.retry_after_ms = close_result.retry_after_ms;
            return response;
        }

        const auto publish_result = config_.durable_file->PublishStagedFile(
            PublishDurableFileRequest{
                .staging_path = layout.staging_relative_path,
                .final_path = layout.final_relative_path,
                .mode = DurablePublishMode::kExclusive,
                .context = {}});
        if (!HasRequiredDurableBoundary(publish_result))
        {
            response.status = publish_result.ok() ? StorageNodeStatusCode::kIoError
                                                  : publish_result.status_code();
            response.error_detail = publish_result.ok()
                                        ? "WriteChunk publish did not reach required durable boundary"
                                        : publish_result.error_detail;
            response.retry_after_ms = publish_result.retry_after_ms;
            return response;
        }

        const auto sync_result = config_.durable_file->SyncDirectory(
            SyncDurableDirectoryRequest{
                .directory_path = layout.final_relative_path.parent_path(),
                .context = {}});
        if (!HasRequiredDurableBoundary(sync_result))
        {
            response.status = sync_result.ok() ? StorageNodeStatusCode::kIoError
                                               : sync_result.status_code();
            response.error_detail = sync_result.ok()
                                        ? "WriteChunk directory sync did not reach required durable boundary"
                                        : sync_result.error_detail;
            response.retry_after_ms = sync_result.retry_after_ms;
            return response;
        }

        const std::uint64_t timestamp_ms = CurrentUnixTimeMillis();
        response.metadata = BuildChunkMetadata(config_,
                                              identity,
                                              static_cast<std::uint64_t>(request.payload.size()),
                                              actual_checksum,
                                              ChunkState::kLive,
                                              request.request_id,
                                              timestamp_ms);
        ChunkIndexEntry new_entry =
            BuildChunkIndexEntry(response.metadata, final_path, timestamp_ms);

        const auto insert_response = config_.chunk_index->Insert(new_entry);
        if (!insert_response.ok())
        {
            if (insert_response.status == StorageNodeStatusCode::kAlreadyExists &&
                insert_response.entry.state == ChunkState::kLive &&
                insert_response.entry.size == new_entry.size &&
                insert_response.entry.checksum.algorithm == new_entry.checksum.algorithm &&
                insert_response.entry.checksum.value == new_entry.checksum.value &&
                insert_response.entry.checksum.size_bytes == new_entry.checksum.size_bytes)
            {
                response.metadata = BuildChunkMetadataFromIndexEntry(
                    config_, insert_response.entry, request.request_id);
                response.status = StorageNodeStatusCode::kOk;
                response.durable = true;
                response.already_exists = true;
                return response;
            }

            response.status = insert_response.status;
            response.error_detail = insert_response.error_detail;
            response.retry_after_ms = insert_response.retry_after_ms;
            return response;
        }

        response.metadata.identity = insert_response.entry.identity;
        response.metadata.size = insert_response.entry.size;
        response.metadata.checksum = insert_response.entry.checksum;
        response.metadata.state = insert_response.entry.state;
        response.durable = true;
        return response;
    }

    ReadChunkResponse LocalDiskChunkStore::ReadChunk(const ReadChunkRequest &)
    {
        return MakeUnsupportedResponse<ReadChunkResponse>("ReadChunk");
    }

    DeleteChunkResponse LocalDiskChunkStore::DeleteChunk(const DeleteChunkRequest &)
    {
        return MakeUnsupportedResponse<DeleteChunkResponse>("DeleteChunk");
    }

    StatChunkResponse LocalDiskChunkStore::StatChunk(const StatChunkRequest &)
    {
        return MakeUnsupportedResponse<StatChunkResponse>("StatChunk");
    }

    ListChunksResponse LocalDiskChunkStore::ListChunks(const ListChunksRequest &)
    {
        return MakeUnsupportedResponse<ListChunksResponse>("ListChunks");
    }
}
