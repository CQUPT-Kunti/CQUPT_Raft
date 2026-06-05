#include "store/maintenance/gc_task_store.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

namespace storedemo
{
    namespace
    {
        constexpr std::string_view kSnapshotMagic = "GC_TASK_STORE_V1";

        std::shared_ptr<DurableFile> CreateDefaultDurableFile(
            const std::filesystem::path &root_path)
        {
#if defined(_WIN32)
            return std::make_shared<WindowsDurableFile>(root_path);
#else
            return std::make_shared<LinuxDurableFile>(root_path);
#endif
        }

        std::string EncodeHex(std::string_view input)
        {
            if (input.empty())
            {
                return "-";
            }
            static constexpr char kDigits[] = "0123456789abcdef";
            std::string encoded;
            encoded.reserve(input.size() * 2);
            for (const unsigned char ch : input)
            {
                encoded.push_back(kDigits[ch >> 4]);
                encoded.push_back(kDigits[ch & 0x0f]);
            }
            return encoded;
        }

        bool DecodeHexDigit(const char ch, std::uint8_t *value)
        {
            if (value == nullptr)
            {
                return false;
            }

            if (ch >= '0' && ch <= '9')
            {
                *value = static_cast<std::uint8_t>(ch - '0');
                return true;
            }
            if (ch >= 'a' && ch <= 'f')
            {
                *value = static_cast<std::uint8_t>(10 + (ch - 'a'));
                return true;
            }
            if (ch >= 'A' && ch <= 'F')
            {
                *value = static_cast<std::uint8_t>(10 + (ch - 'A'));
                return true;
            }
            return false;
        }

        bool DecodeHex(std::string_view input,
                       std::string *output,
                       std::string *error_detail)
        {
            if (output == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "hex decode output must not be null";
                }
                return false;
            }
            if (input == "-")
            {
                output->clear();
                return true;
            }
            if ((input.size() % 2) != 0)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "hex string must have even length";
                }
                return false;
            }

            output->clear();
            output->reserve(input.size() / 2);
            for (std::size_t index = 0; index < input.size(); index += 2)
            {
                std::uint8_t high = 0;
                std::uint8_t low = 0;
                if (!DecodeHexDigit(input[index], &high) ||
                    !DecodeHexDigit(input[index + 1], &low))
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = "hex string contains invalid digit";
                    }
                    return false;
                }
                output->push_back(
                    static_cast<char>((high << 4) | low));
            }
            return true;
        }

        template <typename Integer>
        bool ParseUnsignedToken(const std::string &token,
                                Integer *value,
                                std::string *error_detail)
        {
            if (value == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "unsigned parse output must not be null";
                }
                return false;
            }
            if (token.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "unsigned token must not be empty";
                }
                return false;
            }
            for (const unsigned char ch : token)
            {
                if (std::isdigit(ch) == 0)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = "unsigned token contains non-digit characters";
                    }
                    return false;
                }
            }

            std::uint64_t parsed = 0;
            try
            {
                parsed = std::stoull(token);
            }
            catch (const std::exception &)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "failed to parse unsigned token";
                }
                return false;
            }
            *value = static_cast<Integer>(parsed);
            if (static_cast<std::uint64_t>(*value) != parsed)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "unsigned token overflows target type";
                }
                return false;
            }
            return true;
        }

        bool ParseReason(const std::string &token,
                         GarbageCollectionReason *reason,
                         std::string *error_detail)
        {
            std::uint64_t raw = 0;
            if (!ParseUnsignedToken(token, &raw, error_detail))
            {
                return false;
            }
            switch (static_cast<GarbageCollectionReason>(raw))
            {
            case GarbageCollectionReason::kUnspecified:
            case GarbageCollectionReason::kDeletedObjectCleanup:
            case GarbageCollectionReason::kOrphanChunkCleanup:
            case GarbageCollectionReason::kFailedUploadCleanup:
            case GarbageCollectionReason::kAbortCleanup:
                *reason = static_cast<GarbageCollectionReason>(raw);
                return true;
            }
            if (error_detail != nullptr)
            {
                *error_detail = "unknown garbage collection reason";
            }
            return false;
        }

        bool ParseTaskState(const std::string &token,
                            GarbageCollectorTaskState *state,
                            std::string *error_detail)
        {
            std::uint64_t raw = 0;
            if (!ParseUnsignedToken(token, &raw, error_detail))
            {
                return false;
            }
            switch (static_cast<GarbageCollectorTaskState>(raw))
            {
            case GarbageCollectorTaskState::kQueued:
            case GarbageCollectorTaskState::kRunning:
            case GarbageCollectorTaskState::kRetryPending:
            case GarbageCollectorTaskState::kCompleted:
            case GarbageCollectorTaskState::kFailed:
            case GarbageCollectorTaskState::kCancelled:
                *state = static_cast<GarbageCollectorTaskState>(raw);
                return true;
            }
            if (error_detail != nullptr)
            {
                *error_detail = "unknown garbage collector task state";
            }
            return false;
        }

        bool ParseStatus(const std::string &token,
                         StorageNodeStatusCode *status,
                         std::string *error_detail)
        {
            std::uint64_t raw = 0;
            if (!ParseUnsignedToken(token, &raw, error_detail))
            {
                return false;
            }
            switch (static_cast<StorageNodeStatusCode>(raw))
            {
            case StorageNodeStatusCode::kOk:
            case StorageNodeStatusCode::kInvalidArgument:
            case StorageNodeStatusCode::kAlreadyExists:
            case StorageNodeStatusCode::kNotFound:
            case StorageNodeStatusCode::kConflict:
            case StorageNodeStatusCode::kDiskFull:
            case StorageNodeStatusCode::kPermissionDenied:
            case StorageNodeStatusCode::kTimeout:
            case StorageNodeStatusCode::kCancelled:
            case StorageNodeStatusCode::kOverloaded:
            case StorageNodeStatusCode::kNodeUnavailable:
            case StorageNodeStatusCode::kChecksumMismatch:
            case StorageNodeStatusCode::kCorrupted:
            case StorageNodeStatusCode::kUnsupported:
            case StorageNodeStatusCode::kIoError:
                *status = static_cast<StorageNodeStatusCode>(raw);
                return true;
            }
            if (error_detail != nullptr)
            {
                *error_detail = "unknown storage node status code";
            }
            return false;
        }

        std::vector<GarbageCollectorTask> BuildSortedTasks(
            const std::vector<GarbageCollectorTask> &tasks)
        {
            std::vector<GarbageCollectorTask> sorted_tasks = tasks;
            std::sort(sorted_tasks.begin(),
                      sorted_tasks.end(),
                      [](const GarbageCollectorTask &lhs,
                         const GarbageCollectorTask &rhs)
                      {
                          return lhs.task_id < rhs.task_id;
                      });
            return sorted_tasks;
        }

        std::string SerializeTaskLine(const GarbageCollectorTask &task)
        {
            std::ostringstream output;
            output << "task "
                   << EncodeHex(task.task_id) << ' '
                   << EncodeHex(task.chunk_id) << ' '
                   << EncodeHex(task.object_id) << ' '
                   << task.version << ' '
                   << task.chunk_index << ' '
                   << static_cast<std::uint32_t>(task.reason) << ' '
                   << EncodeHex(task.metadata_boundary) << ' '
                   << task.attempts << ' '
                   << task.max_attempts << ' '
                   << static_cast<std::uint32_t>(task.last_error) << ' '
                   << EncodeHex(task.last_error_detail) << ' '
                   << static_cast<std::uint32_t>(task.state) << ' '
                   << (task.retryable ? 1 : 0) << ' '
                   << task.next_retry_after_ms << "\n";
            return output.str();
        }

        DurableFileResult AppendStringToWriter(DurableFileWriter *writer,
                                               const std::string_view payload)
        {
            DurableFileResult result;
            if (writer == nullptr)
            {
                result.error = DurableFileErrorCode::kIoError;
                result.error_detail = "durable writer must not be null";
                return result;
            }

            const auto *payload_bytes =
                reinterpret_cast<const std::byte *>(payload.data());
            return writer->Append(DurableAppendRequest{
                .buffer = std::span<const std::byte>(payload_bytes, payload.size())});
        }

        bool ParseTaskLine(const std::string &line,
                           GarbageCollectorTask *task,
                           std::string *error_detail)
        {
            if (task == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "parsed task output must not be null";
                }
                return false;
            }

            std::istringstream input(line);
            std::string prefix;
            std::string task_id_hex;
            std::string chunk_id_hex;
            std::string object_id_hex;
            std::string version_token;
            std::string chunk_index_token;
            std::string reason_token;
            std::string metadata_boundary_hex;
            std::string attempts_token;
            std::string max_attempts_token;
            std::string last_error_token;
            std::string last_error_detail_hex;
            std::string state_token;
            std::string retryable_token;
            std::string next_retry_after_ms_token;

            if (!(input >> prefix >> task_id_hex >> chunk_id_hex >> object_id_hex >>
                  version_token >> chunk_index_token >> reason_token >>
                  metadata_boundary_hex >> attempts_token >> max_attempts_token >>
                  last_error_token >> last_error_detail_hex >> state_token >>
                  retryable_token >> next_retry_after_ms_token))
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "task snapshot line is missing required fields";
                }
                return false;
            }
            if (prefix != "task")
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "task snapshot line must start with task";
                }
                return false;
            }

            std::string task_id;
            std::string chunk_id;
            std::string object_id;
            std::string metadata_boundary;
            std::string last_error_detail;
            if (!DecodeHex(task_id_hex, &task_id, error_detail) ||
                !DecodeHex(chunk_id_hex, &chunk_id, error_detail) ||
                !DecodeHex(object_id_hex, &object_id, error_detail) ||
                !DecodeHex(metadata_boundary_hex, &metadata_boundary, error_detail) ||
                !DecodeHex(last_error_detail_hex, &last_error_detail, error_detail))
            {
                return false;
            }

            GarbageCollectorTask parsed;
            parsed.task_id = std::move(task_id);
            parsed.chunk_id = std::move(chunk_id);
            parsed.object_id = std::move(object_id);
            parsed.metadata_boundary = std::move(metadata_boundary);
            parsed.last_error_detail = std::move(last_error_detail);

            if (!ParseUnsignedToken(version_token, &parsed.version, error_detail) ||
                !ParseUnsignedToken(chunk_index_token, &parsed.chunk_index, error_detail) ||
                !ParseReason(reason_token, &parsed.reason, error_detail) ||
                !ParseUnsignedToken(attempts_token, &parsed.attempts, error_detail) ||
                !ParseUnsignedToken(max_attempts_token, &parsed.max_attempts, error_detail) ||
                !ParseStatus(last_error_token, &parsed.last_error, error_detail) ||
                !ParseTaskState(state_token, &parsed.state, error_detail) ||
                !ParseUnsignedToken(next_retry_after_ms_token,
                                    &parsed.next_retry_after_ms,
                                    error_detail))
            {
                return false;
            }

            std::uint32_t retryable_raw = 0;
            if (!ParseUnsignedToken(retryable_token, &retryable_raw, error_detail))
            {
                return false;
            }
            if (retryable_raw > 1)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "retryable flag must be 0 or 1";
                }
                return false;
            }
            parsed.retryable = retryable_raw == 1;
            *task = std::move(parsed);
            return true;
        }

        std::filesystem::path SnapshotRelativePath()
        {
            return std::filesystem::path(std::string(kGarbageCollectorTaskSnapshotRelativePath));
        }

        std::filesystem::path SnapshotStagingRelativePath()
        {
            static std::atomic<std::uint64_t> counter{0};
            return std::filesystem::path("gc") /
                   ("tasks.snapshot." +
                    std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) +
                    ".tmp");
        }
    }

    GarbageCollectorTaskStore::GarbageCollectorTaskStore(
        GarbageCollectorTaskStoreConfig config)
        : config_(std::move(config))
    {
        if (config_.durable_file == nullptr)
        {
            config_.durable_file = CreateDefaultDurableFile(config_.root_path);
        }
    }

    DurableFileResult GarbageCollectorTaskStore::SaveSnapshot(
        const std::vector<GarbageCollectorTask> &tasks) const
    {
        DurableFileResult result;
        if (config_.durable_file == nullptr)
        {
            result.error = DurableFileErrorCode::kUnsupported;
            result.error_detail = "garbage collector task store requires a DurableFile";
            return result;
        }

        const auto sorted_tasks = BuildSortedTasks(tasks);
        const auto staging_relative_path = SnapshotStagingRelativePath();
        const auto final_relative_path = SnapshotRelativePath();

        auto open_response =
            config_.durable_file->OpenStagingWriter(OpenStagingWriterRequest{
                .relative_path = staging_relative_path});
        if (!open_response.ok())
        {
            result = open_response;
            return result;
        }
        if (open_response.writer == nullptr)
        {
            result.error = DurableFileErrorCode::kIoError;
            result.error_detail =
                "OpenStagingWriter succeeded without returning a writer";
            return result;
        }

        std::size_t bytes_written = 0;
        const std::string header = std::string(kSnapshotMagic) + "\n";
        auto append_result = AppendStringToWriter(open_response.writer.get(), header);
        if (!append_result.ok())
        {
            (void)open_response.writer->Close(DurableCloseRequest{});
            return append_result;
        }
        bytes_written += header.size();

        const std::string count_line =
            "count " + std::to_string(sorted_tasks.size()) + "\n";
        append_result = AppendStringToWriter(open_response.writer.get(), count_line);
        if (!append_result.ok())
        {
            (void)open_response.writer->Close(DurableCloseRequest{});
            return append_result;
        }
        bytes_written += count_line.size();

        for (const auto &task : sorted_tasks)
        {
            const std::string task_line = SerializeTaskLine(task);
            append_result = AppendStringToWriter(open_response.writer.get(), task_line);
            if (!append_result.ok())
            {
                (void)open_response.writer->Close(DurableCloseRequest{});
                return append_result;
            }
            bytes_written += task_line.size();
        }

        const auto flush_result = open_response.writer->Flush(DurableFlushRequest{
            .mode = DurableFlushMode::kDataAndMetadata});
        if (!flush_result.ok())
        {
            (void)open_response.writer->Close(DurableCloseRequest{});
            return flush_result;
        }

        const auto close_result = open_response.writer->Close(DurableCloseRequest{});
        if (!close_result.ok())
        {
            return close_result;
        }

        const auto publish_result =
            config_.durable_file->PublishStagedFile(PublishDurableFileRequest{
                .staging_path = staging_relative_path,
                .final_path = final_relative_path,
                .mode = DurablePublishMode::kReplaceExisting});
        if (!publish_result.ok())
        {
            return publish_result;
        }

        const auto sync_result =
            config_.durable_file->SyncDirectory(SyncDurableDirectoryRequest{
                .directory_path = final_relative_path.parent_path()});
        if (!sync_result.ok())
        {
            return sync_result;
        }

        result.bytes_transferred = bytes_written;
        result.durable_boundary_reached = true;
        return result;
    }

    GarbageCollectorTaskStoreLoadResult GarbageCollectorTaskStore::LoadSnapshot() const
    {
        GarbageCollectorTaskStoreLoadResult result;

        std::filesystem::path resolved_snapshot_path;
        std::string resolve_error;
        if (ResolveDurablePathUnderRoot(config_.root_path,
                                        SnapshotRelativePath(),
                                        &resolved_snapshot_path,
                                        &resolve_error) != StorageNodeStatusCode::kOk)
        {
            result.status = StorageNodeStatusCode::kInvalidArgument;
            result.error_detail =
                "failed to resolve garbage collector task snapshot path: " +
                resolve_error;
            return result;
        }

        std::error_code exists_ec;
        if (!std::filesystem::exists(resolved_snapshot_path, exists_ec))
        {
            if (exists_ec)
            {
                result.status = StorageNodeStatusCode::kIoError;
                result.error_detail =
                    "failed to stat garbage collector task snapshot: " +
                    exists_ec.message();
                return result;
            }
            result.snapshot_found = false;
            return result;
        }

        std::ifstream input(resolved_snapshot_path, std::ios::binary);
        if (!input.is_open())
        {
            result.status = StorageNodeStatusCode::kIoError;
            result.error_detail =
                "failed to open garbage collector task snapshot: " +
                resolved_snapshot_path.string();
            return result;
        }

        std::string header;
        if (!std::getline(input, header) || header != kSnapshotMagic)
        {
            result.status = StorageNodeStatusCode::kCorrupted;
            result.error_detail =
                "garbage collector task snapshot has invalid header";
            return result;
        }

        std::string count_line;
        if (!std::getline(input, count_line))
        {
            result.status = StorageNodeStatusCode::kCorrupted;
            result.error_detail =
                "garbage collector task snapshot is missing count line";
            return result;
        }

        std::istringstream count_stream(count_line);
        std::string count_prefix;
        std::string count_token;
        if (!(count_stream >> count_prefix >> count_token) || count_prefix != "count")
        {
            result.status = StorageNodeStatusCode::kCorrupted;
            result.error_detail =
                "garbage collector task snapshot has invalid count line";
            return result;
        }

        std::size_t expected_count = 0;
        if (!ParseUnsignedToken(count_token, &expected_count, &result.error_detail))
        {
            result.status = StorageNodeStatusCode::kCorrupted;
            return result;
        }

        std::vector<GarbageCollectorTask> tasks;
        tasks.reserve(expected_count);
        std::string line;
        while (std::getline(input, line))
        {
            if (line.empty())
            {
                continue;
            }
            GarbageCollectorTask task;
            if (!ParseTaskLine(line, &task, &result.error_detail))
            {
                result.status = StorageNodeStatusCode::kCorrupted;
                result.error_detail =
                    "failed to parse garbage collector task snapshot: " +
                    result.error_detail;
                return result;
            }
            tasks.push_back(std::move(task));
        }

        if (tasks.size() != expected_count)
        {
            result.status = StorageNodeStatusCode::kCorrupted;
            result.error_detail =
                "garbage collector task snapshot count does not match payload";
            return result;
        }

        result.snapshot_found = true;
        result.tasks = std::move(tasks);
        return result;
    }

    const std::filesystem::path &GarbageCollectorTaskStore::root_path() const
    {
        return config_.root_path;
    }

    std::filesystem::path GarbageCollectorTaskStore::snapshot_path() const
    {
        return config_.root_path / SnapshotRelativePath();
    }
}
