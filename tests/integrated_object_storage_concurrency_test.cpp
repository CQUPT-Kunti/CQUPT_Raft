#include "store/common/store_types.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    constexpr std::size_t kStressObjectCount = 50;
    constexpr std::size_t kStressOperationCount = kStressObjectCount * 2;
    constexpr std::uint32_t kBoundedWorkerLimit = 8;
    constexpr std::uint64_t kMaxFixtureBytes = 512ULL * 1024ULL;

    enum class StressOperationKind : std::uint8_t
    {
        kUpload = 0,
        kDownload = 1,
    };

    enum class StressFailureClass : std::uint8_t
    {
        kMetadataCommit = 0,
        kStorageWrite = 1,
        kStorageRead = 2,
        kChecksum = 3,
        kDiscovery = 4,
        kLeaderHint = 5,
        kCapacity = 6,
        kConcurrencyControl = 7,
    };

    struct StressOperationSpec
    {
        StressOperationKind kind{StressOperationKind::kUpload};
        std::size_t object_index{0};
        std::string request_id;
        std::string bucket;
        std::string object_key;
        std::filesystem::path source_path;
        std::filesystem::path destination_path;
        std::string expected_sha256;
        std::uint64_t payload_size{0};
    };

    class ScopedStressWorkspace
    {
    public:
        ScopedStressWorkspace() = default;

        explicit ScopedStressWorkspace(std::filesystem::path root)
            : root_(std::move(root))
        {
        }

        ScopedStressWorkspace(const ScopedStressWorkspace &) = delete;
        ScopedStressWorkspace &operator=(const ScopedStressWorkspace &) = delete;

        ScopedStressWorkspace(ScopedStressWorkspace &&other) noexcept
            : root_(std::move(other.root_))
        {
            other.root_.clear();
        }

        ScopedStressWorkspace &operator=(ScopedStressWorkspace &&other) noexcept
        {
            if (this == &other)
            {
                return *this;
            }

            Cleanup();
            root_ = std::move(other.root_);
            other.root_.clear();
            return *this;
        }

        ~ScopedStressWorkspace()
        {
            Cleanup();
        }

        [[nodiscard]] const std::filesystem::path &root() const
        {
            return root_;
        }

    private:
        void Cleanup()
        {
            if (root_.empty())
            {
                return;
            }

            std::error_code ec;
            std::filesystem::remove_all(root_, ec);
            root_.clear();
        }

        std::filesystem::path root_;
    };

    struct StressPlan
    {
        ScopedStressWorkspace workspace;
        std::vector<StressOperationSpec> operations;
        std::uint32_t concurrency_limit{0};
    };

    struct StressOperationOutcome
    {
        std::string request_id;
        StressOperationKind kind{StressOperationKind::kUpload};
        std::size_t object_index{0};
        bool committed{false};
        bool checksum_verified{false};
        std::optional<StressFailureClass> failure_class;
        std::string failure_detail;
    };

    struct StressValidationSummary
    {
        std::size_t total_operations{0};
        std::size_t committed_uploads{0};
        std::size_t verified_downloads{0};
        std::size_t failed_operations{0};
        std::vector<std::string> issues;

        [[nodiscard]] bool ok() const
        {
            return issues.empty();
        }
    };

    [[nodiscard]] const char *ToString(const StressOperationKind kind)
    {
        switch (kind)
        {
        case StressOperationKind::kUpload:
            return "upload";
        case StressOperationKind::kDownload:
            return "download";
        }
        return "unknown";
    }

    [[nodiscard]] const char *ToString(const StressFailureClass failure_class)
    {
        switch (failure_class)
        {
        case StressFailureClass::kMetadataCommit:
            return "metadata-commit";
        case StressFailureClass::kStorageWrite:
            return "storage-write";
        case StressFailureClass::kStorageRead:
            return "storage-read";
        case StressFailureClass::kChecksum:
            return "checksum";
        case StressFailureClass::kDiscovery:
            return "discovery";
        case StressFailureClass::kLeaderHint:
            return "leader-hint";
        case StressFailureClass::kCapacity:
            return "capacity";
        case StressFailureClass::kConcurrencyControl:
            return "concurrency-control";
        }
        return "unknown";
    }

    [[nodiscard]] std::string MakePayloadForObject(const std::size_t object_index)
    {
        const std::size_t payload_size = 1024 + object_index * 97;
        std::string payload;
        payload.reserve(payload_size);
        for (std::size_t byte_index = 0; byte_index < payload_size; ++byte_index)
        {
            payload.push_back(static_cast<char>(
                ((object_index + 1) * 31 + byte_index * 17) % 251));
        }
        return payload;
    }

    void WriteBinaryFileOrThrow(const std::filesystem::path &path,
                                const std::string &content)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            throw std::runtime_error("failed to create directories for " +
                                     path.string() + ": " + ec.message());
        }

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            throw std::runtime_error("failed to open " + path.string());
        }

        output.write(content.data(),
                     static_cast<std::streamsize>(content.size()));
        output.close();
        if (!output.good())
        {
            throw std::runtime_error("failed to flush " + path.string());
        }
    }

    [[nodiscard]] std::string ComputeSha256ForPayloadOrThrow(
        const std::string_view payload)
    {
        storedemo::ChunkChecksum checksum;
        std::string error_detail;
        const auto status =
            storedemo::ComputeChunkChecksum(payload, &checksum, &error_detail);
        if (status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to compute SHA-256: " +
                                     error_detail);
        }

        return checksum.value;
    }

    [[nodiscard]] std::filesystem::path MakeWorkspaceRoot()
    {
        const auto now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        return std::filesystem::temp_directory_path() /
               "cqupt_integrated_object_storage_concurrency" /
               ("t078-" + std::to_string(now_ns));
    }

    [[nodiscard]] StressPlan MakeStressPlan(const std::size_t object_count,
                                            const std::uint32_t concurrency_limit)
    {
        StressPlan plan;
        plan.workspace = ScopedStressWorkspace(MakeWorkspaceRoot());
        plan.concurrency_limit = concurrency_limit;
        plan.operations.reserve(object_count * 2);

        for (std::size_t object_index = 0; object_index < object_count;
             ++object_index)
        {
            const std::string payload = MakePayloadForObject(object_index);
            const std::filesystem::path source_path =
                plan.workspace.root() / "input" /
                ("object-" + std::to_string(object_index) + ".bin");
            const std::filesystem::path destination_path =
                plan.workspace.root() / "output" /
                ("object-" + std::to_string(object_index) + ".download.bin");
            WriteBinaryFileOrThrow(source_path, payload);

            const std::string object_key =
                "objects/stress-" + std::to_string(object_index) + ".bin";
            const std::string expected_sha256 =
                ComputeSha256ForPayloadOrThrow(payload);

            plan.operations.push_back(StressOperationSpec{
                .kind = StressOperationKind::kUpload,
                .object_index = object_index,
                .request_id = "t078-upload-" + std::to_string(object_index),
                .bucket = "stress-bucket",
                .object_key = object_key,
                .source_path = source_path,
                .destination_path = {},
                .expected_sha256 = expected_sha256,
                .payload_size = static_cast<std::uint64_t>(payload.size()),
            });
            plan.operations.push_back(StressOperationSpec{
                .kind = StressOperationKind::kDownload,
                .object_index = object_index,
                .request_id = "t078-download-" + std::to_string(object_index),
                .bucket = "stress-bucket",
                .object_key = object_key,
                .source_path = {},
                .destination_path = destination_path,
                .expected_sha256 = expected_sha256,
                .payload_size = static_cast<std::uint64_t>(payload.size()),
            });
        }

        return plan;
    }

    [[nodiscard]] StressValidationSummary ValidateStressOutcomes(
        const std::vector<StressOperationSpec> &operations,
        const std::vector<StressOperationOutcome> &outcomes)
    {
        StressValidationSummary summary;
        summary.total_operations = operations.size();

        std::unordered_map<std::string, StressOperationSpec> operations_by_request_id;
        operations_by_request_id.reserve(operations.size());
        for (const auto &operation : operations)
        {
            operations_by_request_id.emplace(operation.request_id, operation);
        }

        std::unordered_set<std::string> seen_requests;
        std::unordered_set<std::size_t> committed_upload_indices;
        std::unordered_set<std::size_t> verified_download_indices;

        for (const auto &outcome : outcomes)
        {
            const auto operation_it =
                operations_by_request_id.find(outcome.request_id);
            if (operation_it == operations_by_request_id.end())
            {
                summary.issues.push_back("unknown request_id=" + outcome.request_id);
                continue;
            }
            if (!seen_requests.insert(outcome.request_id).second)
            {
                summary.issues.push_back("duplicate outcome for request_id=" +
                                         outcome.request_id);
                continue;
            }

            const StressOperationSpec &operation = operation_it->second;
            if (operation.kind != outcome.kind)
            {
                summary.issues.push_back("request_id=" + outcome.request_id +
                                         " returned mismatched kind=" +
                                         ToString(outcome.kind));
                continue;
            }
            if (operation.object_index != outcome.object_index)
            {
                summary.issues.push_back("request_id=" + outcome.request_id +
                                         " returned mismatched object_index");
                continue;
            }

            if (outcome.failure_class.has_value() && outcome.failure_detail.empty())
            {
                summary.issues.push_back("request_id=" + outcome.request_id +
                                         " is missing failure_detail");
            }

            if (outcome.kind == StressOperationKind::kUpload)
            {
                if (outcome.committed)
                {
                    ++summary.committed_uploads;
                    committed_upload_indices.insert(outcome.object_index);
                    if (outcome.failure_class.has_value())
                    {
                        summary.issues.push_back(
                            "request_id=" + outcome.request_id +
                            " reported committed upload and failure_class=" +
                            ToString(*outcome.failure_class));
                    }
                }
                else
                {
                    ++summary.failed_operations;
                    if (!outcome.failure_class.has_value())
                    {
                        summary.issues.push_back(
                            "request_id=" + outcome.request_id +
                            " failed upload is missing failure classification");
                    }
                }
            }
            else
            {
                if (outcome.checksum_verified)
                {
                    ++summary.verified_downloads;
                    verified_download_indices.insert(outcome.object_index);
                    if (outcome.failure_class.has_value())
                    {
                        summary.issues.push_back(
                            "request_id=" + outcome.request_id +
                            " reported checksum_verified download and failure_class=" +
                            ToString(*outcome.failure_class));
                    }
                }
                else
                {
                    ++summary.failed_operations;
                    if (!outcome.failure_class.has_value())
                    {
                        summary.issues.push_back(
                            "request_id=" + outcome.request_id +
                            " failed download is missing failure classification");
                    }
                }
            }
        }

        if (outcomes.size() != operations.size())
        {
            summary.issues.push_back("expected " + std::to_string(operations.size()) +
                                     " operation outcomes, got " +
                                     std::to_string(outcomes.size()));
        }

        for (const auto &operation : operations)
        {
            if (!seen_requests.contains(operation.request_id))
            {
                summary.issues.push_back("missing outcome for request_id=" +
                                         operation.request_id);
            }
        }

        for (const auto object_index : committed_upload_indices)
        {
            if (!verified_download_indices.contains(object_index))
            {
                summary.issues.push_back(
                    "object_index=" + std::to_string(object_index) +
                    " committed successfully but has no checksum-verified download");
            }
        }

        return summary;
    }
} // namespace

TEST(IntegratedObjectStorageConcurrencyTest,
     T078StressPlanPreparesHundredOperationsWithBoundedResourcesAndSha256Fixtures)
{
    const StressPlan plan = MakeStressPlan(kStressObjectCount,
                                           kBoundedWorkerLimit);

    ASSERT_EQ(plan.operations.size(), kStressOperationCount);
    EXPECT_EQ(plan.concurrency_limit, kBoundedWorkerLimit);
    EXPECT_GT(plan.concurrency_limit, 0U);
    EXPECT_LT(plan.concurrency_limit, plan.operations.size());

    std::unordered_set<std::string> request_ids;
    std::unordered_set<std::string> upload_object_keys;
    std::unordered_set<std::string> source_paths;
    std::unordered_set<std::string> destination_paths;
    std::unordered_map<std::string, std::size_t> operation_count_by_key;
    std::uint64_t total_fixture_bytes = 0;

    for (const auto &operation : plan.operations)
    {
        EXPECT_TRUE(request_ids.insert(operation.request_id).second);
        EXPECT_EQ(operation.expected_sha256.size(),
                  storedemo::kSha256DigestHexChars);
        EXPECT_FALSE(operation.expected_sha256.empty());
        operation_count_by_key[operation.object_key] += 1;

        if (operation.kind == StressOperationKind::kUpload)
        {
            EXPECT_TRUE(upload_object_keys.insert(operation.object_key).second);
            EXPECT_TRUE(std::filesystem::exists(operation.source_path));
            EXPECT_TRUE(source_paths.insert(operation.source_path.string()).second);
            EXPECT_FALSE(std::filesystem::exists(operation.destination_path));
            EXPECT_EQ(std::filesystem::file_size(operation.source_path),
                      static_cast<std::uintmax_t>(operation.payload_size));
            total_fixture_bytes += operation.payload_size;
        }
        else
        {
            EXPECT_TRUE(destination_paths.insert(
                            operation.destination_path.string())
                            .second);
            EXPECT_FALSE(std::filesystem::exists(operation.destination_path));
            EXPECT_TRUE(operation.source_path.empty());
        }
    }

    EXPECT_EQ(upload_object_keys.size(), kStressObjectCount);
    EXPECT_EQ(source_paths.size(), kStressObjectCount);
    EXPECT_EQ(destination_paths.size(), kStressObjectCount);
    EXPECT_LT(total_fixture_bytes, kMaxFixtureBytes);

    for (const auto &[object_key, count] : operation_count_by_key)
    {
        EXPECT_EQ(count, 2U) << "object_key=" << object_key
                             << " should have exactly one upload and one download op";
    }

    // T078 先把测试侧的资源边界锁定下来：
    // 100 个客户端操作只生成 50 个小型 fixture 文件，并把 worker 上限固定为 8，
    // 不允许测试自身通过无界线程或大文件膨胀来“制造并发”。
}

TEST(IntegratedObjectStorageConcurrencyTest,
     T078OutcomeValidationAcceptsCommittedUploadsOnlyWhenMatchingDownloadVerifiesSha256)
{
    const StressPlan plan = MakeStressPlan(2, 2);
    const auto summary = ValidateStressOutcomes(
        plan.operations,
        {
            StressOperationOutcome{
                .request_id = "t078-upload-0",
                .kind = StressOperationKind::kUpload,
                .object_index = 0,
                .committed = true,
            },
            StressOperationOutcome{
                .request_id = "t078-download-0",
                .kind = StressOperationKind::kDownload,
                .object_index = 0,
                .checksum_verified = true,
            },
            StressOperationOutcome{
                .request_id = "t078-upload-1",
                .kind = StressOperationKind::kUpload,
                .object_index = 1,
                .failure_class = StressFailureClass::kCapacity,
                .failure_detail = "no healthy StorageNode satisfied required capacity",
            },
            StressOperationOutcome{
                .request_id = "t078-download-1",
                .kind = StressOperationKind::kDownload,
                .object_index = 1,
                .failure_class = StressFailureClass::kMetadataCommit,
                .failure_detail = "paired upload never reached COMMITTED visibility",
            },
        });

    EXPECT_TRUE(summary.ok()) << [&summary]()
    {
        std::ostringstream oss;
        for (const auto &issue : summary.issues)
        {
            oss << issue << '\n';
        }
        return oss.str();
    }();
    EXPECT_EQ(summary.total_operations, 4U);
    EXPECT_EQ(summary.committed_uploads, 1U);
    EXPECT_EQ(summary.verified_downloads, 1U);
    EXPECT_EQ(summary.failed_operations, 2U);
}

TEST(IntegratedObjectStorageConcurrencyTest,
     T078OutcomeValidationRejectsMissingFailureClassificationOrVerifiedDownload)
{
    const StressPlan plan = MakeStressPlan(1, 1);
    const auto summary = ValidateStressOutcomes(
        plan.operations,
        {
            StressOperationOutcome{
                .request_id = "t078-upload-0",
                .kind = StressOperationKind::kUpload,
                .object_index = 0,
                .committed = true,
            },
            StressOperationOutcome{
                .request_id = "t078-download-0",
                .kind = StressOperationKind::kDownload,
                .object_index = 0,
            },
        });

    EXPECT_FALSE(summary.ok());
    EXPECT_EQ(summary.committed_uploads, 1U);
    EXPECT_EQ(summary.verified_downloads, 0U);
    EXPECT_EQ(summary.failed_operations, 1U);
    ASSERT_EQ(summary.issues.size(), 2U);
    EXPECT_NE(summary.issues[0].find("missing failure classification"),
              std::string::npos);
    EXPECT_NE(summary.issues[1].find("no checksum-verified download"),
              std::string::npos);
}

TEST(IntegratedObjectStorageConcurrencyTest,
     DISABLED_T078ConcurrentUploadDownloadHundredOperationsRoundTripWithFinalSha256)
{
    const StressPlan plan = MakeStressPlan(kStressObjectCount,
                                           kBoundedWorkerLimit);

    ASSERT_EQ(plan.operations.size(), kStressOperationCount);
    ASSERT_EQ(plan.concurrency_limit, kBoundedWorkerLimit);

    GTEST_SKIP()
        << "T078 当前先落测试骨架，不越权实现生产并发控制。"
        << "启用这个 100-op 压测前需要两类前置条件："
        << "一是上传主链路必须真正完成 chunk write + CommitObject，"
        << "而当前 ObjectTransfer upload 仍停在 CreateWritePlan/discovery 诊断边界；"
        << "二是 T083 需要补齐生产 bounded concurrency controls。"
        << "前置条件满足后，此用例应以 worker_limit="
        << kBoundedWorkerLimit
        << " 执行 50 次 upload + 50 次 download，"
        << "并断言每个成功 COMMITTED 的对象都能下载且最终 SHA-256 完整匹配。";
}
