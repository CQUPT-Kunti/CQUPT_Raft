#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "store/io/durable_file.h"
#include "store/maintenance/garbage_collector.h"

namespace storedemo
{
    inline constexpr std::string_view kGarbageCollectorTaskSnapshotRelativePath =
        "gc/tasks.snapshot";

    struct GarbageCollectorTaskStoreConfig
    {
        std::filesystem::path root_path;
        std::shared_ptr<DurableFile> durable_file;
    };

    struct GarbageCollectorTaskStoreLoadResult
    {
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        std::string error_detail;
        bool snapshot_found{false};
        std::vector<GarbageCollectorTask> tasks;

        [[nodiscard]] bool ok() const
        {
            return status == StorageNodeStatusCode::kOk;
        }
    };

    class GarbageCollectorTaskStore
    {
    public:
        explicit GarbageCollectorTaskStore(GarbageCollectorTaskStoreConfig config);

        [[nodiscard]] DurableFileResult SaveSnapshot(
            const std::vector<GarbageCollectorTask> &tasks) const;
        [[nodiscard]] GarbageCollectorTaskStoreLoadResult LoadSnapshot() const;
        [[nodiscard]] const std::filesystem::path &root_path() const;
        [[nodiscard]] std::filesystem::path snapshot_path() const;

    private:
        GarbageCollectorTaskStoreConfig config_;
    };
}
