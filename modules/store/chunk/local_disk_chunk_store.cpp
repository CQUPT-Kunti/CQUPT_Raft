#include "store/chunk/local_disk_chunk_store.h"

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

        template <typename Response>
        Response MakeUnsupportedResponse(const char *operation)
        {
            Response response;
            response.status = StorageNodeStatusCode::kUnsupported;
            response.error_detail =
                std::string("LocalDiskChunkStore::") + operation +
                " is not implemented in T021";
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

    WriteChunkResponse LocalDiskChunkStore::WriteChunk(const WriteChunkRequest &)
    {
        return MakeUnsupportedResponse<WriteChunkResponse>("WriteChunk");
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
