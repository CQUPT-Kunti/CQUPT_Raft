#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "store/common/store_types.h"

namespace storedemo
{
    struct ChunkIndexResult
    {
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        std::string error_detail;
        std::uint64_t retry_after_ms{0};

        [[nodiscard]] bool ok() const
        {
            return status == StorageNodeStatusCode::kOk;
        }
    };

    struct ChunkIndexListOptions
    {
        std::optional<ChunkState> state_filter;
        std::string prefix_filter;
        std::string page_token;
        std::size_t page_size{0};
        bool include_quarantine{true};
    };

    struct ChunkIndexConfig
    {
        std::size_t shard_count{64};
        std::size_t lock_stripe_count{256};
        std::size_t default_page_size{128};
        std::size_t max_page_size{4096};
    };

    class ChunkLockGuard
    {
    public:
        ChunkLockGuard() = default;
        ChunkLockGuard(const ChunkLockGuard &) = delete;
        ChunkLockGuard &operator=(const ChunkLockGuard &) = delete;
        ChunkLockGuard(ChunkLockGuard &&) noexcept = default;
        ChunkLockGuard &operator=(ChunkLockGuard &&) noexcept = default;
        ~ChunkLockGuard() = default;

        [[nodiscard]] bool owns_lock() const
        {
            return lock_.owns_lock();
        }

        [[nodiscard]] explicit operator bool() const
        {
            return owns_lock();
        }

        [[nodiscard]] std::string_view chunk_id() const
        {
            return chunk_id_;
        }

        [[nodiscard]] std::size_t stripe_index() const
        {
            return stripe_index_;
        }

    private:
        friend class ShardedChunkIndex;

        ChunkLockGuard(std::string chunk_id,
                       const std::size_t stripe_index,
                       std::unique_lock<std::mutex> lock)
            : chunk_id_(std::move(chunk_id))
            , stripe_index_(stripe_index)
            , lock_(std::move(lock))
        {
        }

        std::string chunk_id_;
        std::size_t stripe_index_{0};
        std::unique_lock<std::mutex> lock_;
    };

    struct ChunkIndexLockResponse : ChunkIndexResult
    {
        ChunkLockGuard guard;
        bool acquired{false};
    };

    struct ChunkIndexInsertResponse : ChunkIndexResult
    {
        ChunkIndexEntry entry;
        bool inserted{false};
    };

    struct ChunkIndexUpdateResponse : ChunkIndexResult
    {
        ChunkIndexEntry entry;
        bool updated{false};
    };

    struct ChunkIndexFindResponse : ChunkIndexResult
    {
        ChunkIndexEntry entry;
        bool found{false};
    };

    struct ChunkIndexRemoveResponse : ChunkIndexResult
    {
        ChunkIndexEntry entry;
        bool removed{false};
    };

    struct ChunkIndexListResponse : ChunkIndexResult
    {
        std::vector<ChunkIndexEntry> entries;
        std::string next_page_token;
        std::uint64_t snapshot_epoch{0};
    };

    class ChunkIndex
    {
    public:
        virtual ~ChunkIndex();

        virtual ChunkIndexInsertResponse Insert(const ChunkIndexEntry &entry) = 0;
        virtual ChunkIndexUpdateResponse Update(const ChunkIndexEntry &entry) = 0;
        virtual ChunkIndexFindResponse Find(std::string_view chunk_id) const = 0;
        virtual ChunkIndexRemoveResponse Remove(std::string_view chunk_id) = 0;
        virtual ChunkIndexListResponse List(const ChunkIndexListOptions &options) const = 0;
        virtual ChunkIndexLockResponse AcquireChunkLock(std::string_view chunk_id) = 0;

        ChunkIndexLockResponse AcquireChunkLock(const ChunkIdentity &identity)
        {
            return AcquireChunkLock(identity.chunk_id);
        }
    };

    class ShardedChunkIndex : public ChunkIndex
    {
    public:
        explicit ShardedChunkIndex(ChunkIndexConfig config = {});
        ~ShardedChunkIndex() override;

        ChunkIndexInsertResponse Insert(const ChunkIndexEntry &entry) override;
        ChunkIndexUpdateResponse Update(const ChunkIndexEntry &entry) override;
        ChunkIndexFindResponse Find(std::string_view chunk_id) const override;
        ChunkIndexRemoveResponse Remove(std::string_view chunk_id) override;
        ChunkIndexListResponse List(const ChunkIndexListOptions &options) const override;
        ChunkIndexLockResponse AcquireChunkLock(std::string_view chunk_id) override;

        [[nodiscard]] const ChunkIndexConfig &config() const;

    private:
        struct Shard;
        struct LockStripe;

        [[nodiscard]] std::size_t ComputeShardIndex(std::string_view chunk_id) const;
        [[nodiscard]] std::size_t ComputeLockStripeIndex(std::string_view chunk_id) const;

        ChunkIndexConfig config_;
        std::atomic<std::uint64_t> mutation_epoch_{0};
        std::vector<std::unique_ptr<Shard>> shards_;
        std::vector<std::unique_ptr<LockStripe>> lock_stripes_;
    };
}
