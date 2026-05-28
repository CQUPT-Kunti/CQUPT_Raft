#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#include "store/common/store_types.h"

namespace storedemo::test
{
    inline std::string SanitizeStoreTestName(std::string_view name)
    {
        std::string sanitized;
        sanitized.reserve(name.size());
        for (const unsigned char ch : name)
        {
            if (std::isalnum(ch) != 0)
            {
                sanitized.push_back(static_cast<char>(ch));
            }
            else
            {
                sanitized.push_back('_');
            }
        }
        return sanitized.empty() ? "store_test" : sanitized;
    }

    inline std::filesystem::path StoreTestTempRoot()
    {
        std::error_code ec;
        std::filesystem::path root = std::filesystem::temp_directory_path(ec);
        if (ec)
        {
            root = std::filesystem::current_path() / "tmp";
        }
        return root / "cqupt_raft_store_tests";
    }

    inline std::filesystem::path MakeUniqueStoreTestDir(std::string_view test_name)
    {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t unique_id =
            static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
            counter.fetch_add(1, std::memory_order_relaxed);

        return StoreTestTempRoot() /
               (SanitizeStoreTestName(test_name) + "_" + std::to_string(unique_id));
    }

    class ScopedStoreTestDir
    {
    public:
        explicit ScopedStoreTestDir(std::string_view test_name)
            : root_(MakeUniqueStoreTestDir(test_name))
        {
            std::error_code ec;
            std::filesystem::remove_all(root_, ec);
            std::filesystem::create_directories(root_, ec);
        }

        ~ScopedStoreTestDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(root_, ec);
        }

        ScopedStoreTestDir(const ScopedStoreTestDir &) = delete;
        ScopedStoreTestDir &operator=(const ScopedStoreTestDir &) = delete;

        ScopedStoreTestDir(ScopedStoreTestDir &&other) noexcept
            : root_(std::move(other.root_))
        {
            other.root_.clear();
        }

        ScopedStoreTestDir &operator=(ScopedStoreTestDir &&other) noexcept
        {
            if (this == &other)
            {
                return *this;
            }

            std::error_code ec;
            std::filesystem::remove_all(root_, ec);
            root_ = std::move(other.root_);
            other.root_.clear();
            return *this;
        }

        [[nodiscard]] const std::filesystem::path &root() const
        {
            return root_;
        }

        [[nodiscard]] std::filesystem::path Path(std::string_view relative) const
        {
            return root_ / std::filesystem::path(std::string(relative));
        }

    private:
        std::filesystem::path root_;
    };

    inline StorageNodeId MakeStorageNodeIdFixture(const std::size_t index)
    {
        return "store-node-" + std::to_string(index);
    }

    inline ChunkId MakeChunkIdFixture(const std::size_t index)
    {
        return "chunk-" + std::to_string(index);
    }

    inline ChunkLocation MakeChunkLocationFixture(const std::size_t index)
    {
        return ChunkLocation{
            .node_id = MakeStorageNodeIdFixture(index),
            .chunk_id = MakeChunkIdFixture(index)};
    }

    inline std::string MakeChunkPayload(const std::size_t size,
                                        std::string_view seed = "chunk-payload")
    {
        if (size == 0)
        {
            return {};
        }

        if (seed.empty())
        {
            seed = "x";
        }

        std::string payload;
        payload.reserve(size);
        while (payload.size() < size)
        {
            const std::size_t remaining = size - payload.size();
            payload.append(seed.data(), std::min(seed.size(), remaining));
        }
        return payload;
    }

    inline std::string MakeChecksumFixture(std::string_view payload)
    {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const unsigned char ch : payload)
        {
            hash ^= static_cast<std::uint64_t>(ch);
            hash *= 1099511628211ULL;
        }

        std::ostringstream out;
        out << "fixture-fnv1a:" << std::hex << std::setfill('0') << std::setw(16)
            << hash;
        return out.str();
    }
} // namespace storedemo::test
