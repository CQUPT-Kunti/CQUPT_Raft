#include "store/index/chunk_index.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <utility>

namespace storedemo
{
    namespace
    {
        ChunkIndexConfig SanitizeChunkIndexConfig(ChunkIndexConfig config)
        {
            if (config.shard_count == 0)
            {
                config.shard_count = 1;
            }
            if (config.default_page_size == 0)
            {
                config.default_page_size = 128;
            }
            if (config.max_page_size == 0)
            {
                config.max_page_size = std::max<std::size_t>(config.default_page_size, 4096);
            }
            if (config.max_page_size < config.default_page_size)
            {
                config.max_page_size = config.default_page_size;
            }
            return config;
        }

        bool MatchesChunkIndexListFilter(const ChunkIndexEntry &entry,
                                         const ChunkIndexListOptions &options)
        {
            if (options.state_filter.has_value() &&
                entry.state != *options.state_filter)
            {
                return false;
            }

            if (!options.include_quarantine &&
                entry.state == ChunkState::kQuarantined &&
                (!options.state_filter.has_value() ||
                 *options.state_filter != ChunkState::kQuarantined))
            {
                return false;
            }

            if (!options.prefix_filter.empty() &&
                entry.identity.chunk_id.rfind(options.prefix_filter, 0) != 0)
            {
                return false;
            }

            return options.page_token.empty() ||
                   entry.identity.chunk_id > options.page_token;
        }

        StorageNodeStatusCode ValidateChunkIndexEntry(const ChunkIndexEntry &entry,
                                                      std::string *error_detail)
        {
            if (!entry.identity.HasChunkKey())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "chunk index entry must have a valid chunk identity";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            return ValidateChunkId(entry.identity.chunk_id, error_detail);
        }
    }

    struct ShardedChunkIndex::Shard
    {
        std::unordered_map<ChunkId, ChunkIndexEntry> entries;
    };

    ChunkIndex::~ChunkIndex() = default;

    ShardedChunkIndex::ShardedChunkIndex(ChunkIndexConfig config)
        : config_(SanitizeChunkIndexConfig(config))
    {
        shards_.reserve(config_.shard_count);
        for (std::size_t index = 0; index < config_.shard_count; ++index)
        {
            shards_.push_back(std::make_unique<Shard>());
        }
    }

    ShardedChunkIndex::~ShardedChunkIndex() = default;

    ChunkIndexInsertResponse ShardedChunkIndex::Insert(const ChunkIndexEntry &entry)
    {
        ChunkIndexInsertResponse response;
        response.status = ValidateChunkIndexEntry(entry, &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        const std::size_t shard_index = ComputeShardIndex(entry.identity.chunk_id);
        auto stored_entry = entry;
        stored_entry.lock_shard = shard_index;
        const auto [it, inserted] = shards_[shard_index]->entries.emplace(
            stored_entry.identity.chunk_id,
            std::move(stored_entry));
        response.entry = it->second;
        response.inserted = inserted;
        if (!inserted)
        {
            response.status = StorageNodeStatusCode::kAlreadyExists;
            response.error_detail = "chunk index entry already exists";
            return response;
        }

        ++mutation_epoch_;
        return response;
    }

    ChunkIndexUpdateResponse ShardedChunkIndex::Update(const ChunkIndexEntry &entry)
    {
        ChunkIndexUpdateResponse response;
        response.status = ValidateChunkIndexEntry(entry, &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        const std::size_t shard_index = ComputeShardIndex(entry.identity.chunk_id);
        auto &entries = shards_[shard_index]->entries;
        const auto it = entries.find(entry.identity.chunk_id);
        if (it == entries.end())
        {
            response.status = StorageNodeStatusCode::kNotFound;
            response.error_detail = "chunk index entry not found";
            return response;
        }

        it->second = entry;
        it->second.lock_shard = shard_index;
        response.entry = it->second;
        response.updated = true;
        ++mutation_epoch_;
        return response;
    }

    ChunkIndexFindResponse ShardedChunkIndex::Find(std::string_view chunk_id) const
    {
        ChunkIndexFindResponse response;
        response.status = ValidateChunkId(chunk_id, &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        const std::size_t shard_index = ComputeShardIndex(chunk_id);
        const auto &entries = shards_[shard_index]->entries;
        const auto it = entries.find(std::string(chunk_id));
        if (it == entries.end())
        {
            response.status = StorageNodeStatusCode::kNotFound;
            response.error_detail = "chunk index entry not found";
            return response;
        }

        response.entry = it->second;
        response.found = true;
        return response;
    }

    ChunkIndexRemoveResponse ShardedChunkIndex::Remove(std::string_view chunk_id)
    {
        ChunkIndexRemoveResponse response;
        response.status = ValidateChunkId(chunk_id, &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        const std::size_t shard_index = ComputeShardIndex(chunk_id);
        auto &entries = shards_[shard_index]->entries;
        const auto it = entries.find(std::string(chunk_id));
        if (it == entries.end())
        {
            response.status = StorageNodeStatusCode::kNotFound;
            response.error_detail = "chunk index entry not found";
            return response;
        }

        response.entry = it->second;
        response.removed = true;
        entries.erase(it);
        ++mutation_epoch_;
        return response;
    }

    ChunkIndexListResponse ShardedChunkIndex::List(const ChunkIndexListOptions &options) const
    {
        ChunkIndexListResponse response;

        const std::size_t page_size = options.page_size == 0
                                          ? config_.default_page_size
                                          : std::min(options.page_size, config_.max_page_size);

        std::vector<ChunkIndexEntry> matched_entries;
        for (const auto &shard : shards_)
        {
            for (const auto &[chunk_id, entry] : shard->entries)
            {
                (void)chunk_id;
                if (MatchesChunkIndexListFilter(entry, options))
                {
                    matched_entries.push_back(entry);
                }
            }
        }

        std::sort(matched_entries.begin(),
                  matched_entries.end(),
                  [](const ChunkIndexEntry &lhs, const ChunkIndexEntry &rhs)
                  {
                      return lhs.identity.chunk_id < rhs.identity.chunk_id;
                  });

        if (matched_entries.size() > page_size)
        {
            response.entries.assign(matched_entries.begin(),
                                    matched_entries.begin() +
                                        static_cast<std::ptrdiff_t>(page_size));
            response.next_page_token = response.entries.back().identity.chunk_id;
        }
        else
        {
            response.entries = std::move(matched_entries);
        }

        response.snapshot_epoch = mutation_epoch_;
        return response;
    }

    const ChunkIndexConfig &ShardedChunkIndex::config() const
    {
        return config_;
    }

    std::size_t ShardedChunkIndex::ComputeShardIndex(std::string_view chunk_id) const
    {
        return std::hash<std::string_view>{}(chunk_id) % config_.shard_count;
    }
}
