#include "store/transfer/object_transfer.h"

#include "store/placement/placement_manager.h"
#include "store/runtime/storage_executor.h"
#include "store/transfer/metadata_transfer_client.h"
#include "store/transfer/storage_transfer_client.h"
#include "view/view_client.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_set>
#include <unordered_map>
#include <utility>

namespace storedemo
{
    namespace
    {
        constexpr std::array<std::uint32_t, 8> kSha256InitialState = {
            0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
        constexpr std::size_t kMaxReplicaFanoutWorkers = 3;
        constexpr std::uint32_t kDefaultReplicaFanoutConcurrency = 2;
        constexpr std::uint64_t kReplicaWriteTimeoutMs = 200;

        constexpr std::array<std::uint32_t, 64> kSha256RoundConstants = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

        struct Sha256State
        {
            std::array<std::uint32_t, 8> words{kSha256InitialState};
            std::array<std::uint8_t, 64> buffer{};
            std::size_t buffer_size{0};
            std::uint64_t total_bytes{0};
        };

        struct ReplicaWriteTaskSharedState
        {
            std::mutex mutex;
            std::condition_variable cv;
            std::vector<std::optional<StorageTransferWriteResult>> results;
            std::size_t completed_tasks{0};
        };

        [[nodiscard]] bool IsFinishedStage(const ObjectTransferStage stage)
        {
            return stage == ObjectTransferStage::kCompleted ||
                   stage == ObjectTransferStage::kFailed ||
                   stage == ObjectTransferStage::kCancelled;
        }

        void SetErrorDetail(std::string *error_detail, const std::string &detail)
        {
            if (error_detail != nullptr)
            {
                *error_detail = detail;
            }
        }

        [[nodiscard]] std::string MakeMetadataOperationRequestId(
            std::string_view base_request_id,
            std::string_view operation_suffix)
        {
            return std::string(base_request_id) + "/" + std::string(operation_suffix);
        }

        [[nodiscard]] std::size_t ResolveReplicaFanoutWorkerCount(
            const std::uint32_t max_inflight_chunks,
            const std::uint32_t effective_replica_fanout_concurrency,
            const std::size_t max_parallel_replica_tasks)
        {
            const std::size_t bounded_parallelism_goal = std::max<std::size_t>(
                max_inflight_chunks,
                effective_replica_fanout_concurrency);
            return std::max<std::size_t>(
                1,
                std::min<std::size_t>(
                    max_parallel_replica_tasks,
                    std::min<std::size_t>(bounded_parallelism_goal,
                                          kMaxReplicaFanoutWorkers)));
        }

        [[nodiscard]] StorageTaskContext ResolveReplicaWriteTaskContext(
            const std::uint64_t replica_write_timeout_ms)
        {
            StorageTaskContext context;
            context.timeout_ms = replica_write_timeout_ms == 0
                                     ? kReplicaWriteTimeoutMs
                                     : replica_write_timeout_ms;
            context.best_effort_cancel = false;
            return context;
        }

        // T006-B：byte-level budget 控制 in-flight payload bytes。
        // 每个 chunk task 必须先在此获取 expected_size 的配额，
        // 然后才能打开文件并读取 payload。
        struct InflightByteBudget
        {
            std::mutex mutex;
            std::condition_variable cv;
            std::uint64_t available{0};
            std::uint64_t max_bytes{0};
        };

        [[nodiscard]] std::shared_ptr<InflightByteBudget> CreateInflightByteBudget(
            const std::uint64_t max_bytes)
        {
            auto budget = std::make_shared<InflightByteBudget>();
            budget->available = max_bytes;
            budget->max_bytes = max_bytes;
            return budget;
        }

        [[nodiscard]] bool AcquirePayloadByteBudget(
            InflightByteBudget *budget,
            const std::uint64_t expected_bytes,
            std::string *error_detail)
        {
            if (budget == nullptr)
            {
                return true;
            }

            if (expected_bytes > budget->max_bytes)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "chunk expected_size=" + std::to_string(expected_bytes) +
                        " exceeds max_inflight_payload_bytes=" +
                        std::to_string(budget->max_bytes) +
                        "; cannot fit a single chunk in the byte budget";
                }
                return false;
            }

            std::unique_lock<std::mutex> lock(budget->mutex);
            budget->cv.wait(lock, [budget, expected_bytes]()
            {
                return budget->available >= expected_bytes;
            });
            budget->available -= expected_bytes;
            return true;
        }

        void ReleasePayloadByteBudget(InflightByteBudget *budget,
                                      const std::uint64_t released_bytes)
        {
            if (budget == nullptr)
            {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(budget->mutex);
                budget->available += released_bytes;
            }
            budget->cv.notify_one();
        }

        struct ScopedPayloadByteBudgetReservation
        {
            InflightByteBudget *budget{nullptr};
            std::uint64_t reserved_bytes{0};
            bool active{false};

            ScopedPayloadByteBudgetReservation(
                InflightByteBudget *budget_in,
                const std::uint64_t reserved_bytes_in)
                : budget(budget_in)
                , reserved_bytes(reserved_bytes_in)
                , active(budget_in != nullptr && reserved_bytes_in != 0)
            {
            }

            ~ScopedPayloadByteBudgetReservation()
            {
                if (active)
                {
                    ReleasePayloadByteBudget(budget, reserved_bytes);
                }
            }
        };

        void RecordReplicaWriteTaskResult(
            ReplicaWriteTaskSharedState *state,
            const std::size_t result_index,
            StorageTransferWriteResult result)
        {
            if (state == nullptr)
            {
                return;
            }

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (result_index < state->results.size())
                {
                    state->results[result_index] = std::move(result);
                }
                ++state->completed_tasks;
            }
            state->cv.notify_all();
        }

        void WaitForReplicaWriteTasks(ReplicaWriteTaskSharedState *state,
                                      const std::size_t expected_tasks)
        {
            if (state == nullptr)
            {
                return;
            }

            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock,
                           [state, expected_tasks]()
                           {
                               return state->completed_tasks >= expected_tasks;
                           });
        }

        [[nodiscard]] bool IsUncertainReplicaWriteResult(
            const StorageTransferWriteResult &write_result)
        {
            return write_result.retryable ||
                   write_result.status == StorageNodeStatusCode::kTimeout;
        }

        [[nodiscard]] StorageNodeId ResolveDurableSuccessNodeId(
            const StorageTransferTarget &target,
            const StorageTransferWriteResult &write_result)
        {
            return write_result.target.node_id.empty()
                       ? target.node_id
                       : write_result.target.node_id;
        }

        void AppendChunkFanoutSummaryDiagnostic(
            const std::string &request_id,
            const ChunkIdentity &identity,
            const std::uint32_t selected_target_count,
            const std::uint32_t durable_success_count,
            const std::uint32_t failed_target_count,
            const std::uint32_t uncertain_target_count,
            const bool commit_eligible,
            std::vector<ObjectTransferDiagnostic> *diagnostics)
        {
            if (diagnostics == nullptr)
            {
                return;
            }

            ObjectTransferDiagnostic diagnostic;
            diagnostic.status =
                commit_eligible ? ObjectTransferStatusCode::kOk
                                : (uncertain_target_count != 0
                                       ? ObjectTransferStatusCode::kTimeout
                                       : ObjectTransferStatusCode::kStorageRejected);
            diagnostic.message =
                "chunk fan-out summary: selected_targets=" +
                std::to_string(selected_target_count) +
                ", durable_successes=" +
                std::to_string(durable_success_count) +
                ", failed_targets=" +
                std::to_string(failed_target_count) +
                ", uncertain_targets=" +
                std::to_string(uncertain_target_count) +
                ", commit_eligible=" +
                std::string(commit_eligible ? "true" : "false");
            diagnostic.request_id = request_id;
            diagnostic.chunk_id = identity.chunk_id;
            diagnostic.chunk_index = identity.chunk_index;
            diagnostic.offset = identity.offset;
            diagnostic.retryable = uncertain_target_count != 0;
            diagnostics->push_back(std::move(diagnostic));
        }

        [[nodiscard]] std::uint32_t RotateRight(const std::uint32_t value,
                                                const std::uint32_t bits)
        {
            return (value >> bits) | (value << (32U - bits));
        }

        [[nodiscard]] std::uint32_t LoadBigEndianWord(const std::uint8_t *bytes)
        {
            return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
                   (static_cast<std::uint32_t>(bytes[1]) << 16U) |
                   (static_cast<std::uint32_t>(bytes[2]) << 8U) |
                   static_cast<std::uint32_t>(bytes[3]);
        }

        void StoreBigEndianWord(const std::uint32_t value, std::uint8_t *out_bytes)
        {
            out_bytes[0] = static_cast<std::uint8_t>(value >> 24U);
            out_bytes[1] = static_cast<std::uint8_t>(value >> 16U);
            out_bytes[2] = static_cast<std::uint8_t>(value >> 8U);
            out_bytes[3] = static_cast<std::uint8_t>(value);
        }

        void ProcessSha256Block(Sha256State *state, const std::uint8_t *block)
        {
            std::array<std::uint32_t, 64> schedule{};
            for (std::size_t index = 0; index < 16; ++index)
            {
                schedule[index] = LoadBigEndianWord(block + index * 4U);
            }

            for (std::size_t index = 16; index < schedule.size(); ++index)
            {
                const std::uint32_t s0 =
                    RotateRight(schedule[index - 15U], 7U) ^
                    RotateRight(schedule[index - 15U], 18U) ^
                    (schedule[index - 15U] >> 3U);
                const std::uint32_t s1 =
                    RotateRight(schedule[index - 2U], 17U) ^
                    RotateRight(schedule[index - 2U], 19U) ^
                    (schedule[index - 2U] >> 10U);
                schedule[index] = schedule[index - 16U] + s0 +
                                  schedule[index - 7U] + s1;
            }

            std::uint32_t a = state->words[0];
            std::uint32_t b = state->words[1];
            std::uint32_t c = state->words[2];
            std::uint32_t d = state->words[3];
            std::uint32_t e = state->words[4];
            std::uint32_t f = state->words[5];
            std::uint32_t g = state->words[6];
            std::uint32_t h = state->words[7];

            for (std::size_t index = 0; index < schedule.size(); ++index)
            {
                const std::uint32_t sigma1 =
                    RotateRight(e, 6U) ^ RotateRight(e, 11U) ^ RotateRight(e, 25U);
                const std::uint32_t choice = (e & f) ^ (~e & g);
                const std::uint32_t temp1 =
                    h + sigma1 + choice + kSha256RoundConstants[index] + schedule[index];
                const std::uint32_t sigma0 =
                    RotateRight(a, 2U) ^ RotateRight(a, 13U) ^ RotateRight(a, 22U);
                const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
                const std::uint32_t temp2 = sigma0 + majority;

                h = g;
                g = f;
                f = e;
                e = d + temp1;
                d = c;
                c = b;
                b = a;
                a = temp1 + temp2;
            }

            state->words[0] += a;
            state->words[1] += b;
            state->words[2] += c;
            state->words[3] += d;
            state->words[4] += e;
            state->words[5] += f;
            state->words[6] += g;
            state->words[7] += h;
        }

        void UpdateSha256(Sha256State *state,
                          const std::uint8_t *data,
                          const std::size_t size)
        {
            if (size == 0)
            {
                return;
            }

            state->total_bytes += size;

            std::size_t offset = 0;
            while (offset < size)
            {
                const std::size_t remaining = size - offset;
                const std::size_t copy_size =
                    std::min<std::size_t>(remaining, state->buffer.size() - state->buffer_size);
                std::copy_n(data + offset,
                            copy_size,
                            state->buffer.begin() +
                                static_cast<std::ptrdiff_t>(state->buffer_size));
                state->buffer_size += copy_size;
                offset += copy_size;

                if (state->buffer_size == state->buffer.size())
                {
                    ProcessSha256Block(state, state->buffer.data());
                    state->buffer_size = 0;
                }
            }
        }

        [[nodiscard]] std::array<std::uint8_t, kSha256DigestBytes> FinalizeSha256(
            Sha256State state)
        {
            const std::uint64_t total_bits = state.total_bytes * 8U;

            state.buffer[state.buffer_size++] = 0x80U;
            if (state.buffer_size > 56U)
            {
                std::fill(state.buffer.begin() +
                              static_cast<std::ptrdiff_t>(state.buffer_size),
                          state.buffer.end(),
                          static_cast<std::uint8_t>(0));
                ProcessSha256Block(&state, state.buffer.data());
                state.buffer_size = 0;
            }

            std::fill(state.buffer.begin() +
                          static_cast<std::ptrdiff_t>(state.buffer_size),
                      state.buffer.begin() + 56,
                      static_cast<std::uint8_t>(0));

            for (std::size_t index = 0; index < 8U; ++index)
            {
                state.buffer[63U - index] =
                    static_cast<std::uint8_t>(total_bits >> (index * 8U));
            }
            ProcessSha256Block(&state, state.buffer.data());

            std::array<std::uint8_t, kSha256DigestBytes> digest{};
            for (std::size_t index = 0; index < state.words.size(); ++index)
            {
                StoreBigEndianWord(state.words[index], digest.data() + index * 4U);
            }
            return digest;
        }

        [[nodiscard]] std::string EncodeLowerHex(const std::uint8_t *bytes,
                                                 const std::size_t size)
        {
            static constexpr char kHexDigits[] = "0123456789abcdef";

            std::string encoded(size * 2U, '\0');
            for (std::size_t index = 0; index < size; ++index)
            {
                encoded[index * 2U] = kHexDigits[(bytes[index] >> 4U) & 0x0fU];
                encoded[index * 2U + 1U] = kHexDigits[bytes[index] & 0x0fU];
            }
            return encoded;
        }

        [[nodiscard]] TransferObjectChecksumFacts MakeObjectChecksumFacts(
            const ChunkChecksum &checksum,
            const std::uint64_t size)
        {
            TransferObjectChecksumFacts facts;
            facts.size = size;
            facts.checksum = checksum;
            facts.etag = checksum.value;
            return facts;
        }

        constexpr std::uint32_t kMaxPerSessionInFlightChunks = 2;
        constexpr std::uint32_t kMaxPerSessionBufferedChunks = 2;
        constexpr std::uint32_t kMaxPerSessionTaskSlots = 2;
        constexpr std::size_t kMaxReplicaFanoutQueueCapacity = 256;

        struct SessionConcurrencyBudget
        {
            std::uint32_t requested_concurrency{0};
            std::uint32_t effective_concurrency{0};
            std::uint32_t requested_replica_fanout_concurrency{0};
            std::uint32_t effective_replica_fanout_concurrency{0};
            std::uint32_t max_inflight_chunks{0};
            std::uint32_t max_buffered_chunks{0};
            std::uint32_t max_task_slots{0};
            std::uint64_t max_inflight_payload_bytes{0};
            bool clamped{false};
        };

        [[nodiscard]] std::uint64_t SaturatingMultiply(const std::uint64_t lhs,
                                                       const std::uint32_t rhs)
        {
            if (lhs == 0 || rhs == 0)
            {
                return 0;
            }
            if (lhs > std::numeric_limits<std::uint64_t>::max() / rhs)
            {
                return std::numeric_limits<std::uint64_t>::max();
            }
            return lhs * rhs;
        }

        [[nodiscard]] std::size_t SaturatingMultiplySizeT(
            const std::size_t lhs,
            const std::size_t rhs)
        {
            if (lhs == 0 || rhs == 0)
            {
                return 0;
            }
            if (lhs > std::numeric_limits<std::size_t>::max() / rhs)
            {
                return std::numeric_limits<std::size_t>::max();
            }
            return lhs * rhs;
        }

        [[nodiscard]] std::size_t ResolveReplicaFanoutQueueCapacity(
            const std::uint32_t max_inflight_chunks,
            const std::size_t max_selected_replica_count)
        {
            const auto computed_capacity = SaturatingMultiplySizeT(
                std::max<std::size_t>(1, max_inflight_chunks),
                std::max<std::size_t>(1, max_selected_replica_count));
            return std::max<std::size_t>(
                1,
                std::min<std::size_t>(computed_capacity,
                                      kMaxReplicaFanoutQueueCapacity));
        }

        // T083 当前先把每个 transfer session 的 payload/任务并发显式限制为
        // “单 chunk in-flight”。这样即使 CLI 传入更大的并发参数，也不会在
        // object_transfer 层放大成无界 buffer、无界任务队列或整文件常驻内存路径。
        // T006-B: 测试可覆盖 byte budget 以构造 oversized chunk 场景。
        inline std::optional<std::uint64_t> g_test_max_inflight_bytes_override;

        [[nodiscard]] SessionConcurrencyBudget ResolveSessionConcurrencyBudget(
            const std::uint32_t requested_concurrency,
            const std::uint64_t bounded_chunk_bytes)
        {
            SessionConcurrencyBudget budget;
            budget.requested_concurrency = requested_concurrency;
            budget.effective_concurrency =
                requested_concurrency == 0 ? 0 : kMaxPerSessionInFlightChunks;
            budget.max_inflight_chunks = budget.effective_concurrency;
            budget.max_buffered_chunks =
                budget.effective_concurrency == 0 ? 0 : kMaxPerSessionBufferedChunks;
            budget.max_task_slots =
                budget.effective_concurrency == 0 ? 0 : kMaxPerSessionTaskSlots;
            budget.max_inflight_payload_bytes = SaturatingMultiply(
                bounded_chunk_bytes,
                budget.max_buffered_chunks);
            if (g_test_max_inflight_bytes_override.has_value())
            {
                budget.max_inflight_payload_bytes =
                    *g_test_max_inflight_bytes_override;
            }
            budget.clamped = requested_concurrency > budget.effective_concurrency;
            return budget;
        }

        [[nodiscard]] SessionConcurrencyBudget ResolveUploadSessionConcurrencyBudget(
            const std::uint32_t requested_concurrency,
            const std::uint64_t bounded_chunk_bytes,
            const std::uint64_t configured_max_inflight_bytes,
            const std::uint32_t requested_replica_fanout_concurrency,
            const std::uint32_t desired_replica_count)
        {
            SessionConcurrencyBudget budget;
            budget.requested_concurrency = requested_concurrency;
            budget.effective_concurrency = requested_concurrency;
            budget.max_inflight_chunks = budget.effective_concurrency;
            budget.max_buffered_chunks = budget.effective_concurrency;
            budget.max_task_slots = budget.effective_concurrency;
            budget.max_inflight_payload_bytes =
                configured_max_inflight_bytes == 0
                    ? SaturatingMultiply(
                          bounded_chunk_bytes,
                          std::max<std::uint32_t>(1, requested_concurrency))
                    : configured_max_inflight_bytes;
            if (g_test_max_inflight_bytes_override.has_value())
            {
                budget.max_inflight_payload_bytes =
                    *g_test_max_inflight_bytes_override;
            }
            budget.requested_replica_fanout_concurrency =
                requested_replica_fanout_concurrency;
            budget.effective_replica_fanout_concurrency =
                requested_replica_fanout_concurrency == 0
                    ? desired_replica_count
                    : requested_replica_fanout_concurrency;
            budget.clamped = false;
            return budget;
        }

        [[nodiscard]] std::uint64_t MaxManifestChunkSize(
            const std::vector<TransferCommittedChunk> &chunks)
        {
            std::uint64_t max_chunk_size = 0;
            for (const auto &chunk : chunks)
            {
                max_chunk_size = std::max(max_chunk_size, chunk.size);
            }
            return max_chunk_size;
        }

        [[nodiscard]] std::uint64_t MaxPreparedChunkSize(
            const std::vector<TransferPreparedChunk> &chunks)
        {
            std::uint64_t max_chunk_size = 0;
            for (const auto &chunk : chunks)
            {
                max_chunk_size = std::max(max_chunk_size, chunk.size);
            }
            return max_chunk_size;
        }

        [[nodiscard]] std::size_t MaxSelectedReplicaCount(
            const std::vector<TransferChunkPlan> &chunks)
        {
            std::size_t max_selected_replica_count = 0;
            for (const auto &chunk : chunks)
            {
                max_selected_replica_count = std::max(
                    max_selected_replica_count,
                    chunk.selected_replica_nodes.size());
            }
            return max_selected_replica_count;
        }

        [[nodiscard]] std::string DescribeConcurrencyBudget(
            const ObjectTransferDirection direction,
            const SessionConcurrencyBudget &budget)
        {
            const char *direction_name =
                direction == ObjectTransferDirection::kUpload ? "upload" : "download";
            std::string message =
                std::string(direction_name) +
                " session bounded concurrency policy: requested_concurrency=" +
                std::to_string(budget.requested_concurrency) +
                ", effective_concurrency=" +
                std::to_string(budget.effective_concurrency) +
                ", max_inflight_chunks=" +
                std::to_string(budget.max_inflight_chunks) +
                ", max_task_slots=" +
                std::to_string(budget.max_task_slots) +
                ", max_buffered_chunks=" +
                std::to_string(budget.max_buffered_chunks) +
                ", max_inflight_payload_bytes=" +
                std::to_string(budget.max_inflight_payload_bytes);
            if (budget.clamped)
            {
                message +=
                    "; requested concurrency was clamped to keep object transfer on "
                    "a single bounded chunk-in-flight path and preserve cleanup/"
                    "checksum diagnostics";
            }
            else
            {
                message +=
                    direction == ObjectTransferDirection::kUpload
                        ? "; upload session uses configured bounded multi-chunk pipeline"
                        : "; object transfer stays on a single bounded chunk-in-flight path";
            }
            if (direction == ObjectTransferDirection::kUpload)
            {
                message +=
                    ", requested_replica_fanout_concurrency=" +
                    std::to_string(
                        budget.requested_replica_fanout_concurrency) +
                    ", effective_replica_fanout_concurrency=" +
                    std::to_string(
                        budget.effective_replica_fanout_concurrency);
            }
            return message;
        }

        [[nodiscard]] ObjectTransferDiagnostic MakeDiagnostic(
            const ObjectTransferStatusCode status,
            const std::string &message,
            const std::string &request_id,
            const std::uint32_t chunk_index = 0,
            const std::uint64_t offset = 0,
            const ChunkId &chunk_id = ChunkId(),
            const bool retryable = false)
        {
            ObjectTransferDiagnostic diagnostic;
            diagnostic.status = status;
            diagnostic.message = message;
            diagnostic.request_id = request_id;
            diagnostic.chunk_index = chunk_index;
            diagnostic.offset = offset;
            diagnostic.chunk_id = chunk_id;
            diagnostic.retryable = retryable;
            return diagnostic;
        }

        [[nodiscard]] ObjectTransferStatusCode MapViewStatus(
            const viewdemo::ViewRegistryStatusCode status)
        {
            switch (status)
            {
            case viewdemo::ViewRegistryStatusCode::kOk:
            case viewdemo::ViewRegistryStatusCode::kIdempotentReplay:
                return ObjectTransferStatusCode::kOk;
            case viewdemo::ViewRegistryStatusCode::kInvalidArgument:
                return ObjectTransferStatusCode::kInvalidArgument;
            case viewdemo::ViewRegistryStatusCode::kNotFound:
                return ObjectTransferStatusCode::kNotFound;
            case viewdemo::ViewRegistryStatusCode::kTimeout:
                return ObjectTransferStatusCode::kTimeout;
            case viewdemo::ViewRegistryStatusCode::kOverloaded:
            case viewdemo::ViewRegistryStatusCode::kServiceUnavailable:
                return ObjectTransferStatusCode::kDiscoveryUnavailable;
            case viewdemo::ViewRegistryStatusCode::kConflict:
                return ObjectTransferStatusCode::kConflict;
            case viewdemo::ViewRegistryStatusCode::kUnsupported:
                return ObjectTransferStatusCode::kUnsupported;
            case viewdemo::ViewRegistryStatusCode::kStaleIgnored:
            case viewdemo::ViewRegistryStatusCode::kInternalError:
            default:
                return ObjectTransferStatusCode::kDiscoveryUnavailable;
            }
        }

        [[nodiscard]] ObjectTransferStatusCode MapMetadataStatus(
            const MetadataTransferStatusCode status)
        {
            switch (status)
            {
            case MetadataTransferStatusCode::kOk:
            case MetadataTransferStatusCode::kIdempotentReplay:
                return ObjectTransferStatusCode::kOk;
            case MetadataTransferStatusCode::kInvalidArgument:
                return ObjectTransferStatusCode::kInvalidArgument;
            case MetadataTransferStatusCode::kNotFound:
                return ObjectTransferStatusCode::kNotFound;
            case MetadataTransferStatusCode::kNotLeader:
                return ObjectTransferStatusCode::kMetadataNotLeader;
            case MetadataTransferStatusCode::kTimeout:
                return ObjectTransferStatusCode::kTimeout;
            case MetadataTransferStatusCode::kUnsupported:
                return ObjectTransferStatusCode::kUnsupported;
            case MetadataTransferStatusCode::kServiceUnavailable:
            case MetadataTransferStatusCode::kOverloaded:
            case MetadataTransferStatusCode::kQuorumUnavailable:
                return ObjectTransferStatusCode::kMetadataRejected;
            case MetadataTransferStatusCode::kIdempotencyConflict:
            case MetadataTransferStatusCode::kStateConflict:
                return ObjectTransferStatusCode::kConflict;
            case MetadataTransferStatusCode::kObjectNotVisible:
                return ObjectTransferStatusCode::kNotFound;
            case MetadataTransferStatusCode::kInternalError:
            default:
                return ObjectTransferStatusCode::kInternalError;
            }
        }

        [[nodiscard]] std::string SelectMetadataEndpoint(
            const viewdemo::DiscoverMetadataResult &discovery)
        {
            if (discovery.leader_hint.has_value() &&
                !discovery.leader_hint->endpoint.empty())
            {
                return discovery.leader_hint->endpoint;
            }

            for (const auto &snapshot : discovery.metadata_nodes)
            {
                if (!snapshot.control_plane_endpoint.empty())
                {
                    return snapshot.control_plane_endpoint;
                }
                if (!snapshot.endpoint.empty())
                {
                    return snapshot.endpoint;
                }
            }

            return {};
        }

        [[nodiscard]] StorageTransferTarget MakeStorageTargetFromSnapshot(
            const viewdemo::ViewNodeSnapshot &snapshot)
        {
            StorageTransferTarget target;
            target.node_id = snapshot.node_id;
            if (!snapshot.data_plane_endpoint.empty())
            {
                target.endpoint = snapshot.data_plane_endpoint;
            }
            else
            {
                target.endpoint = snapshot.endpoint;
            }
            return target;
        }

        template <typename DiagnosticsContainer>
        void AppendViewDiagnostics(
            const std::vector<viewdemo::ViewRegistryDiagnostic> &view_diagnostics,
            const std::string &request_id,
            DiagnosticsContainer *diagnostics)
        {
            if (diagnostics == nullptr)
            {
                return;
            }

            for (const auto &item : view_diagnostics)
            {
                ObjectTransferDiagnostic diagnostic;
                diagnostic.status = ObjectTransferStatusCode::kDiscoveryUnavailable;
                diagnostic.message = item.message;
                diagnostic.request_id = request_id;
                diagnostic.node_id = item.node_id;
                diagnostic.endpoint = item.endpoint;
                diagnostics->push_back(std::move(diagnostic));
            }
        }

        template <typename DiagnosticsContainer>
        void AppendMetadataDiagnostics(
            const std::vector<MetadataTransferDiagnostic> &metadata_diagnostics,
            DiagnosticsContainer *diagnostics)
        {
            if (diagnostics == nullptr)
            {
                return;
            }

            for (const auto &item : metadata_diagnostics)
            {
                ObjectTransferDiagnostic diagnostic;
                diagnostic.status = MapMetadataStatus(item.status);
                diagnostic.message = item.message;
                diagnostic.request_id = item.request_id;
                diagnostic.endpoint = item.endpoint;
                diagnostic.retryable = item.retryable;
                diagnostics->push_back(std::move(diagnostic));
            }
        }

        [[nodiscard]] std::shared_ptr<MetadataTransferClient> DiscoverMetadataClient(
            const std::string &request_id,
            const std::string &cluster_id,
            const std::shared_ptr<MetadataTransferClient> &metadata_client_seed,
            const std::shared_ptr<viewdemo::ViewNodeClient> &view_client,
            std::vector<ObjectTransferDiagnostic> *diagnostics,
            ObjectTransferStatusCode *status,
            std::string *error_detail)
        {
            if (view_client == nullptr)
            {
                if (status != nullptr)
                {
                    *status = ObjectTransferStatusCode::kDiscoveryUnavailable;
                }
                SetErrorDetail(error_detail,
                               "ViewNode discovery is required but view_client is null");
                return nullptr;
            }

            const auto discovery_call = view_client->DiscoverMetadata(
                {.request_id = request_id,
                 .cluster_id = cluster_id,
                 .prefer_leader = true,
                 .live_only = true,
                 .limit = 3});
            AppendViewDiagnostics(discovery_call.result.diagnostics,
                                  request_id,
                                  diagnostics);

            if (!discovery_call.transport_ok() || !discovery_call.result.ok())
            {
                if (status != nullptr)
                {
                    *status = discovery_call.transport_ok()
                                  ? MapViewStatus(
                                        discovery_call.result.summary.status)
                                  : ObjectTransferStatusCode::kDiscoveryUnavailable;
                }
                SetErrorDetail(
                    error_detail,
                    discovery_call.transport_ok()
                        ? "ViewNode DiscoverMetadata failed: " +
                              discovery_call.result.summary.message
                        : "ViewNode DiscoverMetadata RPC failed: " +
                              discovery_call.rpc.grpc_error_message);
                return nullptr;
            }

            const std::string endpoint = SelectMetadataEndpoint(
                discovery_call.result);
            if (endpoint.empty())
            {
                if (status != nullptr)
                {
                    *status = ObjectTransferStatusCode::kDiscoveryUnavailable;
                }
                SetErrorDetail(error_detail,
                               "ViewNode returned no usable MetadataNode endpoint");
                return nullptr;
            }

            if (diagnostics != nullptr)
            {
                diagnostics->push_back(MakeDiagnostic(
                    ObjectTransferStatusCode::kOk,
                    discovery_call.result.leader_hint.has_value()
                            ? "ViewNode leader hint selected as MetadataNode endpoint candidate; MetadataService remains authority"
                            : "ViewNode metadata snapshot selected endpoint candidate; MetadataService remains authority",
                    request_id,
                    0,
                    0,
                    ChunkId(),
                    false));
                diagnostics->back().endpoint = endpoint;
                if (discovery_call.result.leader_hint.has_value())
                {
                    diagnostics->back().node_id =
                        discovery_call.result.leader_hint->node_id;
                }
            }

            MetadataTransferClientConfig config;
            if (metadata_client_seed != nullptr)
            {
                config = metadata_client_seed->config();
            }
            if (status != nullptr)
            {
                *status = ObjectTransferStatusCode::kOk;
            }
            return CreateGrpcMetadataTransferClient(endpoint, std::move(config));
        }

        [[nodiscard]] std::unordered_map<StorageNodeId, StorageTransferTarget>
        DiscoverStorageTargets(
            const std::string &request_id,
            const std::string &cluster_id,
            const std::uint64_t minimum_available_capacity_bytes,
            const std::uint32_t limit,
            const bool require_writable,
            const std::shared_ptr<viewdemo::ViewNodeClient> &view_client,
            std::vector<ObjectTransferDiagnostic> *diagnostics,
            ObjectTransferStatusCode *status,
            std::string *error_detail,
            viewdemo::DiscoverStorageResult *discovery_result = nullptr)
        {
            std::unordered_map<StorageNodeId, StorageTransferTarget> targets;
            if (view_client == nullptr)
            {
                if (status != nullptr)
                {
                    *status = ObjectTransferStatusCode::kDiscoveryUnavailable;
                }
                SetErrorDetail(error_detail,
                               "ViewNode discovery is required but view_client is null");
                return targets;
            }

            const auto discovery_call = view_client->DiscoverStorage(
                {.request_id = request_id,
                 .cluster_id = cluster_id,
                 .live_only = true,
                 .minimum_available_capacity_bytes =
                     minimum_available_capacity_bytes,
                 .limit = limit,
                 .require_writable = require_writable});
            AppendViewDiagnostics(discovery_call.result.diagnostics,
                                  request_id,
                                  diagnostics);

            if (!discovery_call.transport_ok() || !discovery_call.result.ok())
            {
                if (status != nullptr)
                {
                    *status = discovery_call.transport_ok()
                                  ? MapViewStatus(
                                        discovery_call.result.summary.status)
                                  : ObjectTransferStatusCode::kDiscoveryUnavailable;
                }
                SetErrorDetail(
                    error_detail,
                    discovery_call.transport_ok()
                        ? "ViewNode DiscoverStorage failed: " +
                              discovery_call.result.summary.message
                        : "ViewNode DiscoverStorage RPC failed: " +
                              discovery_call.rpc.grpc_error_message);
                return targets;
            }

            if (discovery_result != nullptr)
            {
                *discovery_result = discovery_call.result;
            }

            for (const auto &snapshot : discovery_call.result.storage_nodes)
            {
                StorageTransferTarget target = MakeStorageTargetFromSnapshot(snapshot);
                if (target.node_id.empty() || target.endpoint.empty())
                {
                    continue;
                }
                targets.emplace(target.node_id, target);

                if (diagnostics != nullptr)
                {
                    auto diagnostic = MakeDiagnostic(
                        ObjectTransferStatusCode::kOk,
                        "ViewNode storage snapshot recorded for data-plane endpoint resolution only; it does not imply object visibility",
                        request_id);
                    diagnostic.node_id = target.node_id;
                    diagnostic.endpoint = target.endpoint;
                    diagnostics->push_back(std::move(diagnostic));
                }
            }

            if (targets.empty())
            {
                if (status != nullptr)
                {
                    *status = ObjectTransferStatusCode::kDiscoveryUnavailable;
                }
                SetErrorDetail(error_detail,
                               "ViewNode returned no usable StorageNode endpoint");
                return targets;
            }

            if (status != nullptr)
            {
                *status = ObjectTransferStatusCode::kOk;
            }
            return targets;
        }

        [[nodiscard]] std::uint64_t ResolvePlanExpiryWindowMs(
            const MetadataTransferClient &metadata_client,
            const viewdemo::ViewNodeClient *view_client)
        {
            const auto metadata_timeout_ms = static_cast<std::uint64_t>(
                std::max<std::int64_t>(
                    0,
                    metadata_client.config().create_write_plan_timeout.count()));
            const auto discovery_timeout_ms =
                view_client == nullptr
                    ? 0ULL
                    : static_cast<std::uint64_t>(std::max<std::int64_t>(
                          0,
                          view_client->config().discovery_timeout.count()));
            return std::max<std::uint64_t>(
                1000ULL,
                std::max<std::uint64_t>(metadata_timeout_ms,
                                        discovery_timeout_ms));
        }

        [[nodiscard]] std::optional<std::string> ValidateWritePlanLayout(
            const TransferWritePlan &plan)
        {
            if (plan.chunk_size_bytes == 0)
            {
                return "write plan chunk_size_bytes must be greater than zero";
            }
            if (plan.replica_count == 0)
            {
                return "write plan replica_count must be greater than zero";
            }
            if (plan.minimum_successful_writes == 0 ||
                plan.minimum_successful_writes > plan.replica_count)
            {
                return "write plan minimum_successful_writes must be in [1, replica_count]";
            }
            if (plan.total_chunks != plan.chunks.size())
            {
                return "write plan total_chunks does not match chunk plan count";
            }
            if (plan.object_checksum.size == 0 && !plan.chunks.empty())
            {
                return "empty-object write plan must not contain chunk plans";
            }

            std::uint64_t expected_offset = 0;
            for (const auto &chunk : plan.chunks)
            {
                if (chunk.identity.offset != expected_offset || chunk.offset != expected_offset)
                {
                    return "write plan chunk offsets are not contiguous";
                }
                if (chunk.offset != chunk.identity.offset)
                {
                    return "write plan chunk offset does not match chunk identity offset";
                }
                if (chunk.expected_size == 0)
                {
                    return "write plan chunk expected_size must be greater than zero";
                }
                if (!chunk.expected_checksum.IsSet())
                {
                    return "write plan chunk is missing checksum facts";
                }
                if (chunk.required_replica_count == 0)
                {
                    return "write plan chunk required_replica_count must be greater than zero";
                }
                if (chunk.minimum_successful_writes == 0 ||
                    chunk.minimum_successful_writes > chunk.required_replica_count)
                {
                    return "write plan chunk minimum_successful_writes must be in [1, required_replica_count]";
                }
                if (chunk.selected_replica_nodes.size() != chunk.required_replica_count)
                {
                    return "write plan chunk selected_replica_nodes count does not match required_replica_count";
                }

                std::unordered_set<std::string> unique_selected_nodes(
                    chunk.selected_replica_nodes.begin(),
                    chunk.selected_replica_nodes.end());
                if (unique_selected_nodes.size() !=
                    chunk.selected_replica_nodes.size())
                {
                    return "write plan chunk selected_replica_nodes contain duplicates";
                }

                expected_offset += chunk.expected_size;
            }

            if (plan.object_checksum.size != 0 &&
                expected_offset != plan.object_checksum.size)
            {
                return "write plan chunk layout does not match object size";
            }
            return std::nullopt;
        }

        [[nodiscard]] ObjectTransferStatusCode MapStorageStatus(
            const StorageNodeStatusCode status)
        {
            switch (status)
            {
            case StorageNodeStatusCode::kOk:
                return ObjectTransferStatusCode::kOk;
            case StorageNodeStatusCode::kNotFound:
                return ObjectTransferStatusCode::kNotFound;
            case StorageNodeStatusCode::kInvalidArgument:
                return ObjectTransferStatusCode::kInvalidArgument;
            case StorageNodeStatusCode::kChecksumMismatch:
            case StorageNodeStatusCode::kCorrupted:
                return ObjectTransferStatusCode::kChecksumMismatch;
            case StorageNodeStatusCode::kTimeout:
                return ObjectTransferStatusCode::kTimeout;
            case StorageNodeStatusCode::kCancelled:
                return ObjectTransferStatusCode::kCancelled;
            case StorageNodeStatusCode::kUnsupported:
                return ObjectTransferStatusCode::kUnsupported;
            case StorageNodeStatusCode::kConflict:
                return ObjectTransferStatusCode::kConflict;
            case StorageNodeStatusCode::kAlreadyExists:
            case StorageNodeStatusCode::kDiskFull:
            case StorageNodeStatusCode::kPermissionDenied:
            case StorageNodeStatusCode::kIoError:
            case StorageNodeStatusCode::kOverloaded:
            case StorageNodeStatusCode::kNodeUnavailable:
            default:
                return ObjectTransferStatusCode::kStorageRejected;
            }
        }

        [[nodiscard]] std::string SanitizePathToken(std::string_view value)
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
                sanitized = "request";
            }
            return sanitized;
        }

        [[nodiscard]] std::filesystem::path MakeTemporaryDownloadPath(
            const std::filesystem::path &destination_path,
            const std::string &request_id)
        {
            auto temp_path = destination_path;
            temp_path += ".";
            temp_path += SanitizePathToken(request_id);
            temp_path += ".part";
            return temp_path;
        }

        void RemovePathIfExists(const std::filesystem::path &path)
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }

        [[nodiscard]] std::optional<std::string> ValidateObjectChecksumFacts(
            const TransferObjectChecksumFacts &actual,
            const TransferObjectChecksumFacts &expected)
        {
            if (expected.size != 0 && actual.size != expected.size)
            {
                return "downloaded object size does not match expected checksum facts";
            }
            if (expected.checksum.IsSet())
            {
                if (actual.checksum.algorithm != expected.checksum.algorithm ||
                    actual.checksum.value != expected.checksum.value)
                {
                    return "downloaded object checksum does not match expected checksum facts";
                }
            }
            if (!expected.etag.empty() && actual.etag != expected.etag)
            {
                return "downloaded object etag does not match expected checksum facts";
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::string> ValidateManifestLayout(
            const TransferCommittedManifest &manifest,
            std::vector<TransferCommittedChunk> *ordered_chunks)
        {
            if (ordered_chunks == nullptr)
            {
                return std::string("ordered_chunks output must not be null");
            }

            *ordered_chunks = manifest.chunks;
            std::sort(ordered_chunks->begin(),
                      ordered_chunks->end(),
                      [](const TransferCommittedChunk &lhs,
                         const TransferCommittedChunk &rhs)
                      {
                          if (lhs.identity.offset != rhs.identity.offset)
                          {
                              return lhs.identity.offset < rhs.identity.offset;
                          }
                          return lhs.identity.chunk_index < rhs.identity.chunk_index;
                      });

            if (!manifest.object_checksum.checksum.IsSet())
            {
                return "manifest does not contain verifiable object checksum facts";
            }

            std::uint64_t expected_offset = 0;
            for (const auto &chunk : *ordered_chunks)
            {
                if (chunk.identity.offset != expected_offset)
                {
                    return "manifest chunk offsets are not contiguous";
                }
                if (chunk.size == 0 && manifest.object_checksum.size != 0)
                {
                    return "manifest contains zero-sized chunk for non-empty object";
                }
                if (chunk.size != 0 && !chunk.checksum.IsSet())
                {
                    return "manifest chunk is missing checksum facts";
                }
                if (chunk.size != 0 && chunk.replica_nodes.empty())
                {
                    return "manifest chunk does not contain replica node ids";
                }
                expected_offset += chunk.size;
            }

            if (manifest.object_checksum.size != 0 &&
                expected_offset != manifest.object_checksum.size)
            {
                return "manifest chunk sizes do not match object size";
            }
            if (manifest.object_checksum.size == 0 && !ordered_chunks->empty())
            {
                return "empty object manifest must not contain chunks";
            }
            return std::nullopt;
        }

        struct DownloadReplicaAttemptFailure
        {
            ObjectTransferStatusCode status{ObjectTransferStatusCode::kStorageRejected};
            std::string node_id;
            std::string endpoint;
            std::string classification;
            std::string detail;
            bool retryable{false};
        };

        [[nodiscard]] std::vector<StorageTransferTarget> ResolveManifestReplicaTargets(
            const TransferCommittedChunk &chunk,
            const std::unordered_map<StorageNodeId, StorageTransferTarget> &storage_targets)
        {
            std::vector<StorageTransferTarget> targets;
            targets.reserve(chunk.replica_nodes.size());
            for (const auto &replica_node_id : chunk.replica_nodes)
            {
                const auto it = storage_targets.find(replica_node_id);
                if (it != storage_targets.end() && !it->second.endpoint.empty())
                {
                    targets.push_back(it->second);
                }
            }
            return targets;
        }

        [[nodiscard]] DownloadReplicaAttemptFailure MakeDownloadReplicaAttemptFailure(
            const StorageTransferTarget &target,
            const ObjectTransferStatusCode status,
            std::string classification,
            std::string detail,
            const bool retryable = false)
        {
            DownloadReplicaAttemptFailure failure;
            failure.status = status;
            failure.node_id = target.node_id;
            failure.endpoint = target.endpoint;
            failure.classification = std::move(classification);
            failure.detail = std::move(detail);
            failure.retryable = retryable;
            return failure;
        }

        [[nodiscard]] DownloadReplicaAttemptFailure BuildReadResultFailure(
            const StorageTransferTarget &target,
            const StorageTransferReadResult &read_result)
        {
            std::string classification;
            switch (read_result.status)
            {
            case StorageNodeStatusCode::kNotFound:
                classification = "missing";
                break;
            case StorageNodeStatusCode::kTimeout:
                classification = "timeout";
                break;
            case StorageNodeStatusCode::kChecksumMismatch:
                classification = "checksum mismatch";
                break;
            case StorageNodeStatusCode::kCorrupted:
                classification = "corruption";
                break;
            case StorageNodeStatusCode::kCancelled:
                classification = "cancelled";
                break;
            case StorageNodeStatusCode::kInvalidArgument:
                classification = "invalid payload";
                break;
            default:
                classification = read_result.retryable
                                     ? "retryable failure"
                                     : "transport/storage failure";
                break;
            }

            std::string detail = read_result.error_detail;
            if (detail.empty())
            {
                detail = "StorageNode ReadChunk returned status=";
                detail += ToString(read_result.status);
            }
            return MakeDownloadReplicaAttemptFailure(
                target,
                MapStorageStatus(read_result.status),
                std::move(classification),
                std::move(detail),
                read_result.retryable);
        }

        [[nodiscard]] std::string BuildReplicaAttemptDiagnosticMessage(
            const DownloadReplicaAttemptFailure &failure)
        {
            std::string message =
                "manifest replica read attempt failed (" + failure.classification +
                "); trying next same-chunk manifest replica when available: " +
                failure.detail;
            return message;
        }

        [[nodiscard]] std::string BuildAggregatedChunkReadFailureDetail(
            const TransferCommittedChunk &chunk,
            const std::vector<DownloadReplicaAttemptFailure> &failures)
        {
            std::string detail =
                "chunk " + std::to_string(chunk.identity.chunk_index) +
                " failed after all same-chunk manifest replicas were attempted; attempted node ids=[";
            for (std::size_t index = 0; index < failures.size(); ++index)
            {
                if (index != 0)
                {
                    detail += ", ";
                }
                detail += failures[index].node_id.empty() ? "<unknown>"
                                                          : failures[index].node_id;
            }
            detail += "]; replica failures={";
            for (std::size_t index = 0; index < failures.size(); ++index)
            {
                if (index != 0)
                {
                    detail += "; ";
                }
                const auto &failure = failures[index];
                detail += failure.node_id.empty() ? "<unknown>" : failure.node_id;
                detail += ": ";
                detail += failure.classification;
                if (failure.retryable)
                {
                    detail += " (retryable)";
                }
                detail += ": ";
                detail += failure.detail;
            }
            detail += "}";
            return detail;
        }

        [[nodiscard]] ObjectTransferStatusCode AggregateChunkReadFailureStatus(
            const std::vector<DownloadReplicaAttemptFailure> &failures)
        {
            if (failures.empty())
            {
                return ObjectTransferStatusCode::kStorageRejected;
            }

            auto contains_status = [&failures](const ObjectTransferStatusCode status)
            {
                return std::any_of(
                    failures.begin(),
                    failures.end(),
                    [status](const DownloadReplicaAttemptFailure &failure)
                    {
                        return failure.status == status;
                    });
            };

            if (contains_status(ObjectTransferStatusCode::kChecksumMismatch))
            {
                return ObjectTransferStatusCode::kChecksumMismatch;
            }
            if (contains_status(ObjectTransferStatusCode::kConflict))
            {
                return ObjectTransferStatusCode::kConflict;
            }
            if (contains_status(ObjectTransferStatusCode::kTimeout))
            {
                return ObjectTransferStatusCode::kTimeout;
            }
            if (contains_status(ObjectTransferStatusCode::kNotFound))
            {
                return ObjectTransferStatusCode::kNotFound;
            }
            if (contains_status(ObjectTransferStatusCode::kCancelled))
            {
                return ObjectTransferStatusCode::kCancelled;
            }
            if (contains_status(ObjectTransferStatusCode::kInvalidArgument))
            {
                return ObjectTransferStatusCode::kInvalidArgument;
            }
            if (contains_status(ObjectTransferStatusCode::kUnsupported))
            {
                return ObjectTransferStatusCode::kUnsupported;
            }
            return failures.front().status;
        }

        [[nodiscard]] std::vector<StorageTransferTarget>
        ResolveSelectedChunkTargetsFromPlan(
            const TransferChunkPlan &chunk_plan,
            const std::unordered_map<StorageNodeId, StorageTransferTarget> &storage_targets,
            std::string *error_detail)
        {
            const auto chunk_identity = chunk_plan.identity.chunk_id.empty()
                                            ? ("chunk_index=" +
                                               std::to_string(
                                                   chunk_plan.identity.chunk_index))
                                            : ("chunk_id=" +
                                               chunk_plan.identity.chunk_id +
                                               " chunk_index=" +
                                               std::to_string(
                                                   chunk_plan.identity.chunk_index));

            if (chunk_plan.required_replica_count == 0)
            {
                SetErrorDetail(
                    error_detail,
                    "upload write plan selected_replica_nodes are invalid for " +
                        chunk_identity +
                        ": required_replica_count must be greater than zero");
                return {};
            }

            if (chunk_plan.selected_replica_nodes.empty())
            {
                SetErrorDetail(
                    error_detail,
                    "upload write plan selected_replica_nodes are empty for " +
                        chunk_identity);
                return {};
            }

            if (chunk_plan.selected_replica_nodes.size() !=
                chunk_plan.required_replica_count)
            {
                SetErrorDetail(
                    error_detail,
                    "upload write plan selected_replica_nodes count=" +
                        std::to_string(
                            chunk_plan.selected_replica_nodes.size()) +
                        " does not match required_replica_count=" +
                        std::to_string(chunk_plan.required_replica_count) +
                        " for " + chunk_identity);
                return {};
            }

            std::vector<StorageTransferTarget> targets;
            targets.reserve(chunk_plan.selected_replica_nodes.size());
            std::unordered_set<StorageNodeId> unique_selected_nodes;
            unique_selected_nodes.reserve(chunk_plan.selected_replica_nodes.size());
            for (const auto &node_id : chunk_plan.selected_replica_nodes)
            {
                if (node_id.empty())
                {
                    SetErrorDetail(
                        error_detail,
                        "upload write plan selected_replica_nodes contain empty node_id for " +
                            chunk_identity);
                    return {};
                }

                if (!unique_selected_nodes.insert(node_id).second)
                {
                    SetErrorDetail(
                        error_detail,
                        "upload write plan selected_replica_nodes contain duplicate node_id=" +
                            node_id + " for " + chunk_identity);
                    return {};
                }

                const auto it = storage_targets.find(node_id);
                if (it == storage_targets.end() ||
                    it->second.endpoint.empty())
                {
                    SetErrorDetail(
                        error_detail,
                        "upload write plan selected node_id=" + node_id +
                            " for " + chunk_identity +
                            " is not discoverable via ViewNode storage endpoints");
                    return {};
                }

                targets.push_back(it->second);
            }

            return targets;
        }

        [[nodiscard]] TransferCommittedChunk BuildDurableChunkFacts(
            const ChunkIdentity &identity,
            const std::uint64_t expected_size,
            const ChunkChecksum &expected_checksum,
            const StorageTransferWriteResult &first_durable_result,
            std::vector<StorageNodeId> durable_replicas)
        {
            TransferCommittedChunk committed_chunk;
            committed_chunk.identity = identity;
            committed_chunk.size = first_durable_result.metadata.size == 0
                                       ? expected_size
                                       : first_durable_result.metadata.size;
            committed_chunk.checksum =
                first_durable_result.metadata.checksum.IsSet()
                    ? first_durable_result.metadata.checksum
                    : expected_checksum;
            committed_chunk.replica_nodes = std::move(durable_replicas);
            return committed_chunk;
        }

        [[nodiscard]] std::vector<CleanupCandidate>
        BuildFailedUploadCleanupCandidatesFromChunks(
            const std::string &bucket,
            const std::string &object_key,
            const std::string &object_id,
            const std::uint64_t version,
            const std::uint64_t created_at_unix_ms,
            const std::vector<TransferCommittedChunk> &durable_chunks)
        {
            std::vector<CleanupChunkFact> cleanup_chunk_facts;
            cleanup_chunk_facts.reserve(durable_chunks.size());
            for (const auto &chunk : durable_chunks)
            {
                CleanupChunkFact fact;
                fact.identity = chunk.identity;
                fact.identity.offset = chunk.identity.offset;
                fact.size = chunk.size;
                fact.checksum = chunk.checksum;
                fact.replica_nodes = chunk.replica_nodes;
                cleanup_chunk_facts.push_back(std::move(fact));
            }

            FailedUploadCleanupRequest cleanup_request;
            cleanup_request.bucket = bucket;
            cleanup_request.object_key = object_key;
            cleanup_request.object_id = object_id;
            cleanup_request.version = version;
            cleanup_request.object_state = CleanupObjectState::kPending;
            cleanup_request.created_at_unix_ms = created_at_unix_ms;
            cleanup_request.durable_chunks = std::move(cleanup_chunk_facts);
            return BuildFailedUploadCleanupCandidates(cleanup_request);
        }

        void AppendStorageWriteDiagnostic(const std::string &request_id,
                                         const StorageTransferWriteResult &write_result,
                                         const ChunkIdentity &identity,
                                         const std::uint32_t chunk_index,
                                         const std::uint64_t offset,
                                         std::vector<ObjectTransferDiagnostic> *diagnostics)
        {
            if (diagnostics == nullptr)
            {
                return;
            }

            ObjectTransferDiagnostic diagnostic;
            diagnostic.status = MapStorageStatus(write_result.status);
            diagnostic.message =
                write_result.ok()
                    ? "StorageNode WriteChunk recorded durable chunk facts; object visibility still depends on CommitObject"
                    : "StorageNode WriteChunk failed: " + write_result.error_detail;
            diagnostic.request_id = request_id;
            diagnostic.node_id = write_result.target.node_id;
            diagnostic.endpoint = write_result.target.endpoint;
            diagnostic.chunk_id = identity.chunk_id;
            diagnostic.chunk_index = chunk_index;
            diagnostic.offset = offset;
            diagnostic.retryable = write_result.retryable;
            diagnostics->push_back(std::move(diagnostic));
        }

        [[nodiscard]] TransferCommittedManifest BuildFallbackCommittedManifest(
            const TransferWritePlan &write_plan,
            const TransferObjectChecksumFacts &object_checksum,
            const std::vector<TransferCommittedChunk> &committed_chunks,
            const std::uint64_t committed_at_unix_ms)
        {
            TransferCommittedManifest manifest;
            manifest.bucket = write_plan.bucket;
            manifest.object_key = write_plan.object_key;
            manifest.object_id = write_plan.object_id;
            manifest.version = write_plan.version;
            manifest.object_checksum = object_checksum;
            manifest.chunks = committed_chunks;
            manifest.committed_at_unix_ms = committed_at_unix_ms;
            return manifest;
        }

        class FileTransferChunkReader final : public TransferChunkReader
        {
        public:
            ObjectTransferStatusCode Open(const TransferChunkReaderOpenRequest &request,
                                          std::string *error_detail) override
            {
                Close();

                if (request.source_path.empty())
                {
                    SetErrorDetail(error_detail, "source_path is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request.chunk_size == 0)
                {
                    SetErrorDetail(error_detail, "chunk_size must be greater than 0");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request.start_offset % request.chunk_size != 0)
                {
                    SetErrorDetail(error_detail,
                                   "start_offset must align with chunk_size boundary");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }

                std::error_code stat_ec;
                if (!std::filesystem::exists(request.source_path, stat_ec))
                {
                    SetErrorDetail(error_detail,
                                   stat_ec ? "failed to stat source_path: " +
                                                 stat_ec.message()
                                           : "source_path does not exist");
                    return stat_ec ? ObjectTransferStatusCode::kIoError
                                   : ObjectTransferStatusCode::kNotFound;
                }
                if (!std::filesystem::is_regular_file(request.source_path, stat_ec))
                {
                    SetErrorDetail(error_detail,
                                   stat_ec ? "failed to verify regular file: " +
                                                 stat_ec.message()
                                           : "source_path must be a regular file");
                    return stat_ec ? ObjectTransferStatusCode::kIoError
                                   : ObjectTransferStatusCode::kInvalidArgument;
                }

                const auto file_size = std::filesystem::file_size(request.source_path,
                                                                  stat_ec);
                if (stat_ec)
                {
                    SetErrorDetail(error_detail,
                                   "failed to read source file size: " + stat_ec.message());
                    return ObjectTransferStatusCode::kIoError;
                }
                if (request.start_offset > file_size)
                {
                    SetErrorDetail(error_detail,
                                   "start_offset exceeds source file size");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request.chunk_size >
                    static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
                {
                    SetErrorDetail(error_detail,
                                   "chunk_size exceeds supported stream buffer size");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }

                input_.open(request.source_path, std::ios::binary);
                if (!input_.is_open())
                {
                    SetErrorDetail(error_detail,
                                   "failed to open source file for reading");
                    return ObjectTransferStatusCode::kIoError;
                }

                input_.seekg(static_cast<std::streamoff>(request.start_offset),
                             std::ios::beg);
                if (!input_)
                {
                    Close();
                    SetErrorDetail(error_detail, "failed to seek source file");
                    return ObjectTransferStatusCode::kIoError;
                }

                request_ = request;
                file_size_ = file_size;
                next_chunk_index_ =
                    static_cast<std::uint32_t>(request.start_offset / request.chunk_size);
                next_offset_ = request.start_offset;
                opened_ = true;
                return ObjectTransferStatusCode::kOk;
            }

            TransferChunkReadResult ReadNextChunk() override
            {
                TransferChunkReadResult result;
                if (!opened_)
                {
                    result.status = ObjectTransferStatusCode::kInvalidArgument;
                    result.error_detail = "chunk reader is not open";
                    return result;
                }

                result.chunk_index = next_chunk_index_;
                result.offset = next_offset_;
                if (next_offset_ >= file_size_)
                {
                    result.eof = true;
                    result.last_chunk = true;
                    return result;
                }

                const std::uint64_t remaining = file_size_ - next_offset_;
                const std::uint64_t to_read =
                    std::min<std::uint64_t>(request_.chunk_size, remaining);
                result.payload.resize(static_cast<std::size_t>(to_read));
                input_.read(result.payload.data(),
                            static_cast<std::streamsize>(to_read));
                const auto bytes_read =
                    static_cast<std::uint64_t>(input_.gcount());
                if (bytes_read != to_read)
                {
                    result.status = ObjectTransferStatusCode::kIoError;
                    result.error_detail = "failed to read full bounded chunk from source file";
                    result.payload.resize(static_cast<std::size_t>(bytes_read));
                    return result;
                }
                if (input_.bad())
                {
                    result.status = ObjectTransferStatusCode::kIoError;
                    result.error_detail = "source file stream entered bad state";
                    return result;
                }

                next_offset_ += to_read;
                ++next_chunk_index_;
                result.last_chunk = next_offset_ >= file_size_;
                result.eof = result.last_chunk;
                return result;
            }

            void Close() override
            {
                if (input_.is_open())
                {
                    input_.close();
                }
                request_ = TransferChunkReaderOpenRequest{};
                file_size_ = 0;
                next_chunk_index_ = 0;
                next_offset_ = 0;
                opened_ = false;
            }

        private:
            TransferChunkReaderOpenRequest request_{};
            std::ifstream input_;
            std::uint64_t file_size_{0};
            std::uint32_t next_chunk_index_{0};
            std::uint64_t next_offset_{0};
            bool opened_{false};
        };

        class IncrementalTransferChecksumState final : public TransferChecksumState
        {
        public:
            TransferChecksumUpdateResult Append(
                const TransferChecksumUpdateRequest &request) override
            {
                TransferChecksumUpdateResult result;
                if (finalized_)
                {
                    result.status = ObjectTransferStatusCode::kConflict;
                    result.error_detail =
                        "checksum state is finalized and must be reset before append";
                    return result;
                }

                ChunkChecksum chunk_checksum;
                std::string error_detail;
                const auto compute_status =
                    ComputeChunkChecksum(request.payload, &chunk_checksum, &error_detail);
                if (compute_status != StorageNodeStatusCode::kOk)
                {
                    result.status = ObjectTransferStatusCode::kInternalError;
                    result.error_detail = "failed to compute chunk checksum: " +
                                          error_detail;
                    return result;
                }

                if (request.expected_chunk_checksum.has_value())
                {
                    const auto verify_status = VerifyChunkChecksum(
                        request.payload,
                        *request.expected_chunk_checksum,
                        &chunk_checksum,
                        &error_detail);
                    if (verify_status != StorageNodeStatusCode::kOk)
                    {
                        result.status = verify_status == StorageNodeStatusCode::kChecksumMismatch
                                            ? ObjectTransferStatusCode::kChecksumMismatch
                                            : ObjectTransferStatusCode::kInternalError;
                        result.error_detail = "chunk checksum verification failed: " +
                                              error_detail;
                        return result;
                    }
                    result.chunk_checksum_verified = true;
                }

                const auto *bytes =
                    reinterpret_cast<const std::uint8_t *>(request.payload.data());
                UpdateSha256(&object_checksum_state_, bytes, request.payload.size());
                bytes_processed_ += static_cast<std::uint64_t>(request.payload.size());
                ++chunks_processed_;

                result.chunk_checksum = std::move(chunk_checksum);
                result.bytes_processed = bytes_processed_;
                result.chunks_processed = chunks_processed_;
                return result;
            }

            TransferChecksumFinalizeResult Finalize() override
            {
                TransferChecksumFinalizeResult result;
                if (finalized_)
                {
                    if (final_object_checksum_.has_value())
                    {
                        result.object_checksum = *final_object_checksum_;
                    }
                    return result;
                }

                finalized_ = true;
                const auto digest = FinalizeSha256(object_checksum_state_);
                ChunkChecksum checksum;
                checksum.algorithm = ChunkChecksumAlgorithm::kSha256;
                checksum.value = EncodeLowerHex(digest.data(), digest.size());
                checksum.size_bytes = bytes_processed_;
                checksum.computed_at = 0;

                final_object_checksum_ =
                    MakeObjectChecksumFacts(checksum, bytes_processed_);
                result.object_checksum = *final_object_checksum_;
                return result;
            }

            TransferChecksumSnapshot Snapshot() const override
            {
                TransferChecksumSnapshot snapshot;
                snapshot.bytes_processed = bytes_processed_;
                snapshot.chunks_processed = chunks_processed_;
                snapshot.finalized = finalized_;
                snapshot.object_checksum = final_object_checksum_;
                return snapshot;
            }

            void Reset() override
            {
                object_checksum_state_ = Sha256State{};
                bytes_processed_ = 0;
                chunks_processed_ = 0;
                finalized_ = false;
                final_object_checksum_.reset();
            }

        private:
            Sha256State object_checksum_state_{};
            std::uint64_t bytes_processed_{0};
            std::uint32_t chunks_processed_{0};
            bool finalized_{false};
            std::optional<TransferObjectChecksumFacts> final_object_checksum_;
        };

        class BasicTransferSession
        {
        public:
            explicit BasicTransferSession(TransferSessionSnapshot snapshot)
                : snapshot_(std::move(snapshot))
            {
            }

            [[nodiscard]] TransferSessionSnapshot Snapshot() const
            {
                return snapshot_;
            }

            [[nodiscard]] bool finished() const
            {
                return IsFinishedStage(snapshot_.stage);
            }

        protected:
            void SetStage(const ObjectTransferStage stage)
            {
                snapshot_.stage = stage;
            }

            void SetFailure(const TransferFailureSummary &failure)
            {
                snapshot_.failure = failure;
                snapshot_.stage = ObjectTransferStage::kFailed;
            }

            void MarkCompleted()
            {
                snapshot_.stage = ObjectTransferStage::kCompleted;
            }

            TransferSessionSnapshot &mutable_snapshot()
            {
                return snapshot_;
            }

            const TransferSessionSnapshot &snapshot_ref() const
            {
                return snapshot_;
            }

        private:
            TransferSessionSnapshot snapshot_;
        };

        class BasicUploadTransferSession final : public UploadTransferSession,
                                                 private BasicTransferSession
        {
        public:
            BasicUploadTransferSession(
                UploadObjectRequest request,
                std::shared_ptr<MetadataTransferClient> metadata_client,
                std::shared_ptr<StorageTransferClient> storage_client,
                std::shared_ptr<viewdemo::ViewNodeClient> view_client)
                : BasicTransferSession(MakeInitialSnapshot(request)),
                  request_(std::move(request)),
                  session_budget_(ResolveUploadSessionConcurrencyBudget(
                      request_.concurrency,
                      request_.chunk_size,
                      request_.max_inflight_bytes == 0
                          ? SaturatingMultiply(
                                request_.chunk_size,
                                std::max<std::uint32_t>(1, request_.concurrency))
                          : request_.max_inflight_bytes,
                      request_.replica_fanout_concurrency == 0
                          ? std::min<std::uint32_t>(
                                request_.desired_replica_count,
                                kDefaultReplicaFanoutConcurrency)
                          : request_.replica_fanout_concurrency,
                      request_.desired_replica_count)),
                  metadata_client_(std::move(metadata_client)),
                  storage_client_(std::move(storage_client)),
                  view_client_(std::move(view_client))
            {
                if (request_.max_inflight_bytes == 0)
                {
                    request_.max_inflight_bytes = SaturatingMultiply(
                        request_.chunk_size,
                        std::max<std::uint32_t>(1, request_.concurrency));
                }
                if (request_.replica_fanout_concurrency == 0)
                {
                    request_.replica_fanout_concurrency =
                        std::min<std::uint32_t>(
                            request_.desired_replica_count,
                            kDefaultReplicaFanoutConcurrency);
                }
                mutable_snapshot().concurrency = session_budget_.effective_concurrency;
            }

            [[nodiscard]] ObjectTransferDirection direction() const override
            {
                return ObjectTransferDirection::kUpload;
            }

            [[nodiscard]] TransferSessionSnapshot Snapshot() const override
            {
                return snapshot_ref();
            }

            [[nodiscard]] bool finished() const override
            {
                return BasicTransferSession::finished();
            }

            [[nodiscard]] const UploadObjectRequest &request() const override
            {
                return request_;
            }

            UploadObjectResult Execute(TransferChunkReader &reader,
                                       TransferChecksumState &checksum_state) override
            {
                UploadObjectResult result;
                result.session = Snapshot();
                current_write_plan_.reset();

                const auto validation_status = ValidateRequest(&result.error_detail);
                if (validation_status != ObjectTransferStatusCode::kOk)
                {
                    Fail(&result, validation_status, result.error_detail);
                    return result;
                }

                AppendConcurrencyDiagnostic(&result.diagnostics);
                result.session = Snapshot();

                SetStage(ObjectTransferStage::kPreparing);
                result.session = Snapshot();

                checksum_state.Reset();
                std::string open_error;
                const auto open_status = reader.Open(
                    {.source_path = request_.source_path,
                     .chunk_size = request_.chunk_size,
                     .start_offset = 0},
                    &open_error);
                if (open_status != ObjectTransferStatusCode::kOk)
                {
                    Fail(&result, open_status, std::move(open_error));
                    return result;
                }

                std::uint64_t expected_offset = 0;
                std::uint32_t expected_chunk_index = 0;
                while (true)
                {
                    TransferChunkReadResult chunk = reader.ReadNextChunk();
                    if (!chunk.ok())
                    {
                        reader.Close();
                        Fail(&result,
                             chunk.status,
                             chunk.error_detail,
                             chunk.chunk_index,
                             chunk.offset);
                        return result;
                    }

                    if (chunk.offset != expected_offset)
                    {
                        reader.Close();
                        Fail(&result,
                             ObjectTransferStatusCode::kConflict,
                             "chunk reader returned non-contiguous offset",
                             chunk.chunk_index,
                             chunk.offset);
                        return result;
                    }
                    if (chunk.chunk_index != expected_chunk_index)
                    {
                        reader.Close();
                        Fail(&result,
                             ObjectTransferStatusCode::kConflict,
                             "chunk reader returned unexpected chunk index",
                             chunk.chunk_index,
                             chunk.offset);
                        return result;
                    }

                    if (!chunk.payload.empty())
                    {
                        if (chunk.payload.size() > request_.chunk_size)
                        {
                            reader.Close();
                            Fail(&result,
                                 ObjectTransferStatusCode::kConflict,
                                 "chunk reader exceeded bounded chunk_size",
                                 chunk.chunk_index,
                                 chunk.offset);
                            return result;
                        }

                        const auto checksum_update = checksum_state.Append(
                            {.chunk_index = chunk.chunk_index,
                             .offset = chunk.offset,
                             .payload = chunk.payload});
                        if (!checksum_update.ok())
                        {
                            reader.Close();
                            Fail(&result,
                                 checksum_update.status,
                                 checksum_update.error_detail,
                                 chunk.chunk_index,
                                 chunk.offset);
                            return result;
                        }

                        result.prepared_chunks.push_back(
                            {.chunk_index = chunk.chunk_index,
                             .offset = chunk.offset,
                             .size = static_cast<std::uint64_t>(chunk.payload.size()),
                             .checksum = checksum_update.chunk_checksum});

                        mutable_snapshot().bytes_completed +=
                            static_cast<std::uint64_t>(chunk.payload.size());
                        mutable_snapshot().chunks_completed += 1;
                        mutable_snapshot().total_bytes = mutable_snapshot().bytes_completed;
                        mutable_snapshot().total_chunks =
                            mutable_snapshot().chunks_completed;

                        expected_offset +=
                            static_cast<std::uint64_t>(chunk.payload.size());
                        ++expected_chunk_index;
                    }

                    if (chunk.last_chunk)
                    {
                        break;
                    }
                    if (chunk.eof && chunk.payload.empty())
                    {
                        break;
                    }
                }
                reader.Close();

                const auto finalize_result = checksum_state.Finalize();
                if (!finalize_result.ok())
                {
                    Fail(&result,
                         finalize_result.status,
                         finalize_result.error_detail,
                         expected_chunk_index,
                         expected_offset);
                    return result;
                }

                mutable_snapshot().final_checksum_verified = true;
                mutable_snapshot().total_bytes = finalize_result.object_checksum.size;
                result.session = Snapshot();

                if (request_.expected_object_checksum.has_value())
                {
                    const auto mismatch_reason = ValidateExpectedObjectChecksum(
                        finalize_result.object_checksum,
                        *request_.expected_object_checksum);
                    if (mismatch_reason.has_value())
                    {
                        Fail(&result,
                             ObjectTransferStatusCode::kChecksumMismatch,
                             *mismatch_reason,
                             expected_chunk_index,
                             expected_offset);
                        return result;
                    }
                }

                SetStage(ObjectTransferStage::kDiscoveringMetadata);
                result.session = Snapshot();

                ObjectTransferStatusCode discovery_status =
                    ObjectTransferStatusCode::kOk;
                std::string discovery_error;
                const auto discovered_metadata_client = DiscoverMetadataClient(
                    request_.request_id,
                    request_.cluster_id,
                    metadata_client_,
                    view_client_,
                    &result.diagnostics,
                    &discovery_status,
                    &discovery_error);
                if (discovered_metadata_client == nullptr)
                {
                    Fail(&result, discovery_status, std::move(discovery_error));
                    return result;
                }

                SetStage(ObjectTransferStage::kPlanningWrite);
                result.session = Snapshot();

                const auto create_plan_call =
                    discovered_metadata_client->CreateWritePlan(
                        {.request_id = request_.request_id,
                         .bucket = request_.bucket,
                         .object_key = request_.object_key,
                         .object_id = request_.object_id,
                         .expected_object_checksum =
                             finalize_result.object_checksum,
                         .chunk_size = request_.chunk_size,
                         .desired_replica_count =
                             request_.desired_replica_count,
                         .minimum_successful_writes =
                             request_.minimum_successful_writes,
                         .client_time_unix_ms =
                             request_.client_time_unix_ms});
                AppendMetadataDiagnostics(create_plan_call.result.diagnostics,
                                          &result.diagnostics);
                if (!create_plan_call.transport_ok() ||
                    !create_plan_call.result.ok() ||
                    !create_plan_call.result.write_plan.has_value())
                {
                    Fail(&result,
                         !create_plan_call.transport_ok()
                             ? ObjectTransferStatusCode::kMetadataRejected
                             : MapMetadataStatus(
                                   create_plan_call.result.summary.status),
                         !create_plan_call.transport_ok()
                             ? "Metadata CreateWritePlan RPC failed: " +
                                   create_plan_call.rpc.grpc_error_message
                             : "Metadata CreateWritePlan failed: " +
                                   create_plan_call.result.summary.message);
                    return result;
                }
                result.write_plan = create_plan_call.result.write_plan;

                const std::string resolved_object_id =
                    !result.write_plan->object_id.empty()
                        ? result.write_plan->object_id
                        : (!create_plan_call.result.summary.object_id.empty()
                               ? create_plan_call.result.summary.object_id
                               : request_.object_id);
                if (resolved_object_id.empty())
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kMetadataRejected,
                         "Metadata CreateWritePlan did not return a usable object_id");
                    return result;
                }
                if (result.write_plan->version == 0)
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kMetadataRejected,
                         "Metadata CreateWritePlan did not return a usable version");
                    return result;
                }
                result.write_plan->object_id = resolved_object_id;
                mutable_snapshot().object_id = resolved_object_id;
                mutable_snapshot().version = result.write_plan->version;
                result.session = Snapshot();

                std::string storage_discovery_error;
                viewdemo::DiscoverStorageResult storage_discovery_result;
                const auto storage_targets = DiscoverStorageTargets(
                    request_.request_id,
                    request_.cluster_id,
                    MaxPreparedChunkSize(result.prepared_chunks),
                    0,
                    true,
                    view_client_,
                    &result.diagnostics,
                    &discovery_status,
                    &storage_discovery_error,
                    &storage_discovery_result);
                if (storage_targets.empty())
                {
                    Fail(&result,
                         discovery_status,
                         std::move(storage_discovery_error));
                    return result;
                }

                result.write_plan->chunk_size_bytes = request_.chunk_size;
                result.write_plan->total_chunks = static_cast<std::uint32_t>(
                    result.prepared_chunks.size());
                result.write_plan->replica_count = request_.desired_replica_count;
                result.write_plan->minimum_successful_writes =
                    request_.minimum_successful_writes;
                result.write_plan->placement_epoch =
                    storage_discovery_result.observed_at_unix_ms;
                if (result.write_plan->placement_epoch == 0)
                {
                    result.write_plan->placement_epoch =
                        result.write_plan->created_at_unix_ms != 0
                            ? result.write_plan->created_at_unix_ms
                            : request_.client_time_unix_ms;
                }
                if (result.write_plan->placement_epoch != 0)
                {
                    result.write_plan->expires_at_unix_ms =
                        result.write_plan->placement_epoch +
                        ResolvePlanExpiryWindowMs(*discovered_metadata_client,
                                                  view_client_.get());
                }

                result.write_plan->chunks.clear();
                result.write_plan->chunks.reserve(result.prepared_chunks.size());
                PlacementManager placement_manager;
                for (const auto &prepared_chunk : result.prepared_chunks)
                {
                    std::string identity_error;
                    const ChunkIdentity identity = BuildChunkIdentity(
                        prepared_chunk,
                        resolved_object_id,
                        result.write_plan->version,
                        &identity_error);
                    if (!identity_error.empty())
                    {
                        Fail(&result,
                             ObjectTransferStatusCode::kInternalError,
                             "failed to build chunk identity for write plan: " +
                                 identity_error,
                             prepared_chunk.chunk_index,
                             prepared_chunk.offset);
                        return result;
                    }

                    PlacementRequest placement_request;
                    placement_request.identity = identity;
                    placement_request.chunk_size_bytes = prepared_chunk.size;
                    placement_request.policy.replica_count =
                        request_.desired_replica_count;
                    placement_request.policy.minimum_successful_writes =
                        request_.minimum_successful_writes;
                    placement_request.policy.avoid_same_node = true;
                    placement_request.decision_epoch =
                        result.write_plan->placement_epoch;

                    const auto placement_result = placement_manager.SelectPlacement(
                        placement_request,
                        storage_discovery_result);
                    if (!placement_result.ok())
                    {
                        Fail(&result,
                             ObjectTransferStatusCode::kStorageRejected,
                             "CreateWritePlan placement failed for chunk " +
                                 std::to_string(prepared_chunk.chunk_index) + ": " +
                                 placement_result.error_detail,
                             prepared_chunk.chunk_index,
                             prepared_chunk.offset,
                             identity.chunk_id);
                        return result;
                    }

                    TransferChunkPlan chunk_plan;
                    chunk_plan.identity = identity;
                    chunk_plan.offset = prepared_chunk.offset;
                    chunk_plan.expected_size = prepared_chunk.size;
                    chunk_plan.expected_checksum = prepared_chunk.checksum;
                    chunk_plan.required_replica_count =
                        request_.desired_replica_count;
                    chunk_plan.minimum_successful_writes =
                        request_.minimum_successful_writes;
                    chunk_plan.selected_replica_nodes.reserve(
                        placement_result.decision.replica_nodes.size());
                    for (const auto &selected_replica :
                         placement_result.decision.replica_nodes)
                    {
                        chunk_plan.selected_replica_nodes.push_back(
                            selected_replica.node_id);
                    }
                    result.write_plan->chunks.push_back(std::move(chunk_plan));
                }

                if (const auto write_plan_error =
                        ValidateWritePlanLayout(*result.write_plan);
                    write_plan_error.has_value())
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kMetadataRejected,
                         "CreateWritePlan produced invalid transfer facts: " +
                             *write_plan_error);
                    return result;
                }
                current_write_plan_ = result.write_plan;

                if (storage_client_ == nullptr)
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kInternalError,
                         "StorageTransferClient is required for upload chunk writes");
                    return result;
                }

                std::vector<TransferCommittedChunk> durable_chunks;
                durable_chunks.reserve(result.prepared_chunks.size());
                bool uncertain_cleanup_possible = false;
                const auto max_selected_replica_count =
                    MaxSelectedReplicaCount(result.write_plan->chunks);
                const auto max_parallel_replica_tasks =
                    SaturatingMultiplySizeT(
                        std::max<std::size_t>(1, session_budget_.max_inflight_chunks),
                        std::max<std::size_t>(1, max_selected_replica_count));
                const auto fanout_queue_capacity =
                    ResolveReplicaFanoutQueueCapacity(
                        session_budget_.max_inflight_chunks,
                        max_selected_replica_count);
                BoundedStorageExecutor replica_fanout_executor(
                    StorageExecutorConfig{
                        .worker_count = ResolveReplicaFanoutWorkerCount(
                            session_budget_.max_inflight_chunks,
                            session_budget_.effective_replica_fanout_concurrency,
                            max_parallel_replica_tasks),
                        .queue_capacity = fanout_queue_capacity});

                if (!result.prepared_chunks.empty())
                {
                    SetStage(ObjectTransferStage::kUploadingChunks);
                    result.session = Snapshot();

                    const auto chunk_count = result.prepared_chunks.size();

                    struct MultiChunkUploadState
                    {
                        std::mutex mutex;
                        std::condition_variable cv;
                        std::vector<std::optional<TransferCommittedChunk>> results;
                        std::size_t completed_count{0};
                        bool any_failed{false};
                        std::vector<TransferCommittedChunk> cleanup_durables;
                        bool uncertain_cleanup{false};
                        std::vector<ObjectTransferDiagnostic> diagnostics;
                        std::vector<std::pair<ObjectTransferStatusCode, std::string>> failure_facts;
                    };
                    auto multi_state = std::make_shared<MultiChunkUploadState>();
                    multi_state->results.resize(chunk_count);
                    multi_state->failure_facts.resize(chunk_count,
                        {ObjectTransferStatusCode::kStorageRejected, std::string{}});
                    std::size_t submitted_chunk_tasks = 0;

                    auto shared_targets = std::make_shared<
                        const std::unordered_map<
                            StorageNodeId, StorageTransferTarget>>(
                        std::move(storage_targets));

                    BoundedStorageExecutor chunk_executor(
                        StorageExecutorConfig{
                            .worker_count = std::max<std::size_t>(
                                1, session_budget_.max_inflight_chunks),
                            .queue_capacity = chunk_count +
                                session_budget_.max_inflight_chunks});

                    auto byte_budget = CreateInflightByteBudget(
                        session_budget_.max_inflight_payload_bytes);

                    bool submit_failed = false;
                    ObjectTransferStatusCode submit_failure_status =
                        ObjectTransferStatusCode::kStorageRejected;
                    std::string submit_failure_detail;

                    for (std::size_t pi = 0; pi < chunk_count; ++pi)
                    {
                        const auto &prepared_chunk =
                            result.prepared_chunks[pi];

                        std::string identity_error;
                        const ChunkIdentity identity = BuildChunkIdentity(
                            prepared_chunk,
                            resolved_object_id,
                            result.write_plan->version,
                            &identity_error);
                        if (identity.chunk_id.empty())
                        {
                            submit_failed = true;
                            submit_failure_status =
                                ObjectTransferStatusCode::kInternalError;
                            submit_failure_detail =
                                "failed to build upload chunk identity: " +
                                identity_error;
                            break;
                        }

                        const auto *chunk_plan = FindChunkPlan(
                            prepared_chunk.chunk_index);
                        if (chunk_plan == nullptr)
                        {
                            submit_failed = true;
                            submit_failure_status =
                                ObjectTransferStatusCode::kMetadataRejected;
                            submit_failure_detail =
                                "upload write plan is missing chunk placement for chunk_index=" +
                                std::to_string(prepared_chunk.chunk_index);
                            break;
                        }

                        const auto minimum_successful_writes =
                            ResolveMinimumSuccessfulWrites(
                                prepared_chunk.chunk_index);
                        const auto desired_replica_count =
                            ResolveDesiredReplicaCount(
                                prepared_chunk.chunk_index);

                        // T006-B: acquire byte budget before payload read
                        std::string byte_budget_error;
                        if (!AcquirePayloadByteBudget(
                                byte_budget.get(),
                                prepared_chunk.size,
                                &byte_budget_error))
                        {
                            submit_failed = true;
                            submit_failure_status =
                                ObjectTransferStatusCode::kInternalError;
                            submit_failure_detail =
                                "byte budget acquisition failed: " +
                                byte_budget_error;
                            break;
                        }

                        const auto submit_result = chunk_executor.Submit(
                            StorageExecutorSubmitRequest{
                                .task_name = "multi-chunk/" +
                                    std::to_string(
                                        prepared_chunk.chunk_index),
                                .task =
                                    [this, &replica_fanout_executor,
                                     byte_budget,
                                     prepared_chunk,
                                     chunk_plan = *chunk_plan,
                                     identity,
                                     minimum_successful_writes,
                                     desired_replica_count,
                                     shared_targets,
                                     multi_state,
                                     pi,
                                     source_path = request_.source_path,
                                     request_id = request_.request_id]()
                                    {
                                        ScopedPayloadByteBudgetReservation
                                            payload_budget_reservation(
                                                byte_budget.get(),
                                                prepared_chunk.size);
                                        std::ifstream chunk_file(
                                            source_path, std::ios::binary);
                                        if (!chunk_file)
                                        {
                                            std::lock_guard<std::mutex> lock(
                                                multi_state->mutex);
                                            multi_state->any_failed = true;
                                            multi_state->completed_count++;
                                            multi_state->failure_facts[pi] = {
                                                ObjectTransferStatusCode::kIoError,
                                                "failed to open source file for chunk " +
                                                    std::to_string(prepared_chunk.chunk_index)};
                                            multi_state->diagnostics.push_back(
                                                MakeDiagnostic(
                                                    ObjectTransferStatusCode::kIoError,
                                                    "multi-chunk task failed to open source file",
                                                    request_id,
                                                    prepared_chunk.chunk_index,
                                                    prepared_chunk.offset,
                                                    identity.chunk_id));
                                            multi_state->cv.notify_all();
                                            return;
                                        }

                                        chunk_file.seekg(
                                            static_cast<std::streamoff>(
                                                prepared_chunk.offset));
                                        if (!chunk_file)
                                        {
                                            std::lock_guard<std::mutex> lock(
                                                multi_state->mutex);
                                            multi_state->any_failed = true;
                                            multi_state->completed_count++;
                                            multi_state->failure_facts[pi] = {
                                                ObjectTransferStatusCode::kIoError,
                                                "failed to seek source file for chunk " +
                                                    std::to_string(prepared_chunk.chunk_index)};
                                            multi_state->diagnostics.push_back(
                                                MakeDiagnostic(
                                                    ObjectTransferStatusCode::kIoError,
                                                    "multi-chunk task failed to seek source file",
                                                    request_id,
                                                    prepared_chunk.chunk_index,
                                                    prepared_chunk.offset,
                                                    identity.chunk_id));
                                            multi_state->cv.notify_all();
                                            return;
                                        }

                                        std::string payload(
                                            prepared_chunk.size, '\0');
                                        if (!chunk_file.read(
                                                payload.data(),
                                                static_cast<std::streamsize>(
                                                    prepared_chunk.size)))
                                        {
                                            std::lock_guard<std::mutex> lock(
                                                multi_state->mutex);
                                            multi_state->any_failed = true;
                                            multi_state->completed_count++;
                                            multi_state->failure_facts[pi] = {
                                                ObjectTransferStatusCode::kIoError,
                                                "failed to read chunk payload for chunk " +
                                                    std::to_string(prepared_chunk.chunk_index)};
                                            multi_state->diagnostics.push_back(
                                                MakeDiagnostic(
                                                    ObjectTransferStatusCode::kIoError,
                                                    "multi-chunk task failed to read chunk payload",
                                                    request_id,
                                                    prepared_chunk.chunk_index,
                                                    prepared_chunk.offset,
                                                    identity.chunk_id));
                                            multi_state->cv.notify_all();
                                            return;
                                        }
                                        chunk_file.close();

                                        ChunkChecksum verified_checksum;
                                        std::string verify_error;
                                        const auto verify_status =
                                            VerifyChunkChecksum(
                                                payload,
                                                prepared_chunk.checksum,
                                                &verified_checksum,
                                                &verify_error);
                                        if (verify_status !=
                                            StorageNodeStatusCode::kOk)
                                        {
                                            std::lock_guard<std::mutex> lock(
                                                multi_state->mutex);
                                            multi_state->any_failed = true;
                                            multi_state->completed_count++;
                                            multi_state->failure_facts[pi] = {
                                                verify_status == StorageNodeStatusCode::kChecksumMismatch
                                                    ? ObjectTransferStatusCode::kChecksumMismatch
                                                    : ObjectTransferStatusCode::kInternalError,
                                                "checksum verification failed for chunk " +
                                                    std::to_string(prepared_chunk.chunk_index)};
                                            multi_state->diagnostics.push_back(
                                                MakeDiagnostic(
                                                    verify_status ==
                                                            StorageNodeStatusCode::
                                                                kChecksumMismatch
                                                        ? ObjectTransferStatusCode::
                                                              kChecksumMismatch
                                                        : ObjectTransferStatusCode::
                                                              kInternalError,
                                                    "multi-chunk payload checksum verification failed: " +
                                                        verify_error,
                                                    request_id,
                                                    prepared_chunk.chunk_index,
                                                    prepared_chunk.offset,
                                                    identity.chunk_id));
                                            multi_state->cv.notify_all();
                                            return;
                                        }

                                        std::string targets_error;
                                        const auto chunk_targets =
                                            ResolveSelectedChunkTargetsFromPlan(
                                                chunk_plan,
                                                *shared_targets,
                                                &targets_error);
                                        if (chunk_targets.empty())
                                        {
                                            std::lock_guard<std::mutex> lock(
                                                multi_state->mutex);
                                            multi_state->any_failed = true;
                                            multi_state->completed_count++;
                                            multi_state->failure_facts[pi] = {
                                                ObjectTransferStatusCode::kDiscoveryUnavailable,
                                                "failed to resolve targets for chunk " +
                                                    std::to_string(prepared_chunk.chunk_index)};
                                            multi_state->diagnostics.push_back(
                                                MakeDiagnostic(
                                                    ObjectTransferStatusCode::
                                                        kDiscoveryUnavailable,
                                                        std::move(targets_error),
                                                    request_id,
                                                    prepared_chunk.chunk_index,
                                                    prepared_chunk.offset,
                                                    identity.chunk_id));
                                            multi_state->cv.notify_all();
                                            return;
                                        }

                                        std::vector<StorageNodeId>
                                            durable_replicas;
                                        durable_replicas.reserve(
                                            chunk_targets.size());
                                        std::unordered_set<StorageNodeId>
                                            durable_replica_nodes;
                                        durable_replica_nodes.reserve(
                                            chunk_targets.size());
                                        StorageTransferWriteResult
                                            first_durable_result;
                                        bool have_durable_result = false;
                                        ObjectTransferStatusCode
                                            last_failure_status =
                                                ObjectTransferStatusCode::
                                                    kStorageRejected;
                                        std::string last_failure_message =
                                            "StorageNode WriteChunk did not reach minimum_successful_writes";
                                        StorageTransferTarget
                                            last_failure_target;
                                        bool last_failure_retryable = false;
                                        bool have_uncertain_failure = false;
                                        std::string last_uncertain_message =
                                            "StorageNode WriteChunk ended with uncertain durable state";
                                        StorageTransferTarget
                                            last_uncertain_target;
                                        std::uint32_t failed_target_count = 0;
                                        std::uint32_t uncertain_target_count = 0;
                                        bool chunk_uncertain = false;
                                        auto shared_payload =
                                            std::make_shared<const std::string>(
                                                std::move(payload));
                                        auto replica_write_state =
                                            std::make_shared<
                                                ReplicaWriteTaskSharedState>();
                                        replica_write_state->results.resize(
                                            chunk_targets.size());

                                        std::size_t submitted_replica_tasks = 0;
                                        std::optional<
                                            StorageExecutorSubmitResult>
                                            replica_submit_failure;
                                        for (std::size_t target_index = 0;
                                             target_index <
                                             chunk_targets.size();
                                             ++target_index)
                                        {
                                            const auto &target =
                                                chunk_targets[target_index];
                                            const auto sub_result =
                                                replica_fanout_executor.Submit(
                                                    StorageExecutorSubmitRequest{
                                                        .task_name =
                                                            "multi-chunk-replica-write/" +
                                                            std::to_string(
                                                                prepared_chunk
                                                                    .chunk_index) +
                                                            "/node-" +
                                                            target.node_id,
                                                        .task =
                                                            [this,
                                                             request_id,
                                                             target,
                                                             identity,
                                                             offset =
                                                                 prepared_chunk
                                                                     .offset,
                                                             expected_size =
                                                                 prepared_chunk
                                                                     .size,
                                                             expected_checksum =
                                                                 prepared_chunk
                                                                     .checksum,
                                                             shared_payload,
                                                             replica_write_state,
                                                             target_index]()
                                                            {
                                                                StorageTransferWriteResult
                                                                    write_result;
                                                                try
                                                                {
                                                                    write_result =
                                                                        storage_client_->WriteChunk(
                                                                            {.request_id =
                                                                                 request_id +
                                                                                 "/chunk-" +
                                                                                 std::to_string(
                                                                                     identity.chunk_index) +
                                                                                 "/node-" +
                                                                                 target.node_id,
                                                                             .target = target,
                                                                             .identity = identity,
                                                                             .offset = offset,
                                                                             .expected_size =
                                                                                 expected_size,
                                                                             .expected_checksum =
                                                                                 expected_checksum,
                                                                             .context =
                                                                                ResolveReplicaWriteTaskContext(
                                                                                    request_.replica_write_timeout_ms),
                                                                             .payload =
                                                                                 *shared_payload});
                                                                }
                                                                catch (const std::exception &ex)
                                                                {
                                                                    write_result.status =
                                                                        StorageNodeStatusCode::
                                                                            kIoError;
                                                                    write_result.error_detail =
                                                                        "fan-out replica write threw exception: " +
                                                                        std::string(ex.what());
                                                                    write_result.target = target;
                                                                }
                                                                catch (...)
                                                                {
                                                                    write_result.status =
                                                                        StorageNodeStatusCode::
                                                                            kIoError;
                                                                    write_result.error_detail =
                                                                        "fan-out replica write threw unknown exception";
                                                                    write_result.target = target;
                                                                }

                                                                if (write_result.target.node_id.empty())
                                                                {
                                                                    write_result.target = target;
                                                                }

                                                                RecordReplicaWriteTaskResult(
                                                                    replica_write_state.get(),
                                                                    target_index,
                                                                    std::move(write_result));
                                                            }});
                                            if (!sub_result.accepted())
                                            {
                                                replica_submit_failure =
                                                    sub_result;
                                                break;
                                            }
                                            ++submitted_replica_tasks;
                                        }

                                        WaitForReplicaWriteTasks(
                                            replica_write_state.get(),
                                            submitted_replica_tasks);

                                        if (replica_submit_failure.has_value())
                                        {
                                            std::lock_guard<std::mutex> lock(
                                                multi_state->mutex);
                                            multi_state->any_failed = true;
                                            multi_state->completed_count++;
                                            multi_state->uncertain_cleanup = true;
                                            multi_state->failure_facts[pi] = {
                                                MapStorageStatus(replica_submit_failure->status_code()),
                                                "replica fan-out rejected for chunk " +
                                                    std::to_string(prepared_chunk.chunk_index) +
                                                    " (selected_replica_count=" +
                                                    std::to_string(chunk_targets.size()) +
                                                    ", max_inflight_chunks=" +
                                                    std::to_string(session_budget_.max_inflight_chunks) +
                                                    ", fanout_worker_count=" +
                                                    std::to_string(replica_fanout_executor.config().worker_count) +
                                                    ", fanout_queue_capacity=" +
                                                    std::to_string(replica_fanout_executor.config().queue_capacity) +
                                                    ", target_node_id=" +
                                                    chunk_targets[submitted_replica_tasks].node_id +
                                                    ")"};
                                            multi_state->diagnostics.push_back(
                                                MakeDiagnostic(
                                                    MapStorageStatus(
                                                        replica_submit_failure->status_code()),
                                                    "bounded replica fan-out rejected task for chunk " +
                                                        identity.chunk_id +
                                                        " (selected_replica_count=" +
                                                        std::to_string(chunk_targets.size()) +
                                                        ", max_inflight_chunks=" +
                                                        std::to_string(session_budget_.max_inflight_chunks) +
                                                        ", fanout_worker_count=" +
                                                        std::to_string(replica_fanout_executor.config().worker_count) +
                                                        ", fanout_queue_capacity=" +
                                                        std::to_string(replica_fanout_executor.config().queue_capacity) +
                                                        ", target_node_id=" +
                                                        chunk_targets[submitted_replica_tasks].node_id +
                                                        "): " +
                                                        replica_submit_failure->error_detail,
                                                    request_id,
                                                    prepared_chunk.chunk_index,
                                                    prepared_chunk.offset,
                                                    identity.chunk_id));
                                            multi_state->cv.notify_all();
                                            return;
                                        }

                                        for (std::size_t target_index = 0;
                                             target_index <
                                             chunk_targets.size();
                                             ++target_index)
                                        {
                                            const auto &target =
                                                chunk_targets[target_index];
                                            const auto &write_result =
                                                replica_write_state
                                                    ->results[target_index];
                                            if (!write_result.has_value())
                                            {
                                                std::lock_guard<std::mutex> lock(
                                                    multi_state->mutex);
                                                multi_state->any_failed = true;
                                                multi_state->completed_count++;
                                                multi_state->uncertain_cleanup = true;
                                                multi_state->diagnostics.push_back(
                                                    MakeDiagnostic(
                                                        ObjectTransferStatusCode::
                                                            kInternalError,
                                                        "bounded replica fan-out lost result for chunk " +
                                                            identity.chunk_id +
                                                            " target node_id=" +
                                                            target.node_id,
                                                        request_id,
                                                        prepared_chunk.chunk_index,
                                                        prepared_chunk.offset,
                                                        identity.chunk_id));
                                                multi_state->cv.notify_all();
                                                return;
                                            }

                                            {
                                                std::lock_guard<std::mutex> lock(
                                                    multi_state->mutex);
                                                ObjectTransferDiagnostic diag;
                                                diag.status = MapStorageStatus(
                                                    write_result->status);
                                                diag.message =
                                                    write_result->ok()
                                                        ? "StorageNode WriteChunk recorded durable chunk facts; object visibility still depends on CommitObject"
                                                        : "StorageNode WriteChunk failed: " +
                                                              write_result->error_detail;
                                                diag.request_id = request_id;
                                                diag.node_id =
                                                    write_result->target.node_id;
                                                diag.endpoint =
                                                    write_result->target.endpoint;
                                                diag.chunk_id = identity.chunk_id;
                                                diag.chunk_index =
                                                    prepared_chunk.chunk_index;
                                                diag.offset =
                                                    prepared_chunk.offset;
                                                diag.retryable =
                                                    write_result->retryable;
                                                multi_state->diagnostics.push_back(
                                                    std::move(diag));
                                            }

                                            if (write_result->ok())
                                            {
                                                const auto
                                                    resolved_success_node_id =
                                                        ResolveDurableSuccessNodeId(
                                                            target,
                                                            *write_result);
                                                if (durable_replica_nodes
                                                        .insert(
                                                            resolved_success_node_id)
                                                        .second)
                                                {
                                                    if (!have_durable_result)
                                                    {
                                                        first_durable_result =
                                                            *write_result;
                                                        have_durable_result = true;
                                                    }
                                                    durable_replicas.push_back(
                                                        resolved_success_node_id);
                                                }
                                                continue;
                                            }

                                            last_failure_status =
                                                MapStorageStatus(
                                                    write_result->status);
                                            last_failure_message =
                                                "StorageNode WriteChunk failed: " +
                                                write_result->error_detail;
                                            last_failure_target =
                                                write_result->target.endpoint.empty()
                                                    ? target
                                                    : write_result->target;
                                            last_failure_retryable =
                                                write_result->retryable;
                                            ++failed_target_count;
                                            if (IsUncertainReplicaWriteResult(
                                                    *write_result))
                                            {
                                                ++uncertain_target_count;
                                                have_uncertain_failure = true;
                                                last_uncertain_message =
                                                    "StorageNode WriteChunk ended with uncertain durable state: " +
                                                    write_result->error_detail;
                                                last_uncertain_target =
                                                    write_result->target.endpoint.empty()
                                                        ? target
                                                        : write_result->target;
                                            }
                                            chunk_uncertain =
                                                chunk_uncertain ||
                                                IsUncertainReplicaWriteResult(
                                                    *write_result);
                                        }

                                        const bool chunk_commit_eligible =
                                            durable_replicas.size() >=
                                            minimum_successful_writes;

                                        {
                                            std::lock_guard<std::mutex> lock(
                                                multi_state->mutex);
                                            ObjectTransferDiagnostic summary;
                                            summary.status =
                                                chunk_commit_eligible
                                                    ? ObjectTransferStatusCode::kOk
                                                    : (uncertain_target_count != 0
                                                           ? ObjectTransferStatusCode::
                                                                 kTimeout
                                                           : ObjectTransferStatusCode::
                                                                 kStorageRejected);
                                            summary.message =
                                                "multi-chunk fan-out summary: selected_targets=" +
                                                std::to_string(
                                                    chunk_targets.size()) +
                                                ", durable_successes=" +
                                                std::to_string(
                                                    durable_replicas.size()) +
                                                ", failed_targets=" +
                                                std::to_string(
                                                    failed_target_count) +
                                                ", uncertain_targets=" +
                                                std::to_string(
                                                    uncertain_target_count) +
                                                ", commit_eligible=" +
                                                std::string(
                                                    chunk_commit_eligible
                                                        ? "true"
                                                        : "false");
                                            summary.request_id = request_id;
                                            summary.chunk_id = identity.chunk_id;
                                            summary.chunk_index =
                                                prepared_chunk.chunk_index;
                                            summary.offset =
                                                prepared_chunk.offset;
                                            summary.retryable =
                                                uncertain_target_count != 0;
                                            multi_state->diagnostics.push_back(
                                                std::move(summary));
                                        }

                                        if (!chunk_commit_eligible)
                                        {
                                            std::lock_guard<std::mutex> lock(
                                                multi_state->mutex);
                                            multi_state->any_failed = true;
                                            multi_state->failure_facts[pi] = {
                                                have_uncertain_failure
                                                    ? ObjectTransferStatusCode::kTimeout
                                                    : last_failure_status,
                                                "chunk " + std::to_string(prepared_chunk.chunk_index) +
                                                    " did not reach minimum_successful_writes"};
                                            if (have_durable_result)
                                            {
                                                multi_state->cleanup_durables
                                                    .push_back(
                                                        BuildDurableChunkFacts(
                                                            identity,
                                                            prepared_chunk.size,
                                                            prepared_chunk.checksum,
                                                            first_durable_result,
                                                            durable_replicas));
                                            }
                                            multi_state->uncertain_cleanup =
                                                multi_state
                                                    ->uncertain_cleanup ||
                                                chunk_uncertain;
                                            multi_state->diagnostics
                                                .push_back(
                                                    MakeDiagnostic(
                                                        have_uncertain_failure
                                                            ? ObjectTransferStatusCode::
                                                                  kTimeout
                                                            : last_failure_status,
                                                        "chunk " +
                                                            identity.chunk_id +
                                                            " did not reach minimum_successful_writes=" +
                                                            std::to_string(
                                                                minimum_successful_writes) +
                                                            "; " +
                                                            (have_uncertain_failure
                                                                 ? last_uncertain_message
                                                                 : last_failure_message),
                                                        request_id,
                                                        prepared_chunk
                                                            .chunk_index,
                                                        prepared_chunk.offset,
                                                        identity.chunk_id,
                                                        have_uncertain_failure ||
                                                            last_failure_retryable));
                                            multi_state->completed_count++;
                                            multi_state->cv.notify_all();
                                            return;
                                        }

                                        auto durable_chunk =
                                            BuildDurableChunkFacts(
                                                identity,
                                                prepared_chunk.size,
                                                prepared_chunk.checksum,
                                                first_durable_result,
                                                durable_replicas);
                                        {
                                            std::lock_guard<std::mutex> lock(
                                                multi_state->mutex);
                                            multi_state->results[pi] =
                                                std::move(durable_chunk);
                                            multi_state->cleanup_durables
                                                .push_back(
                                                    *multi_state->results[pi]);
                                            multi_state->completed_count++;
                                        }
                                        multi_state->cv.notify_all();
                                    }});

                        if (!submit_result.accepted())
                        {
                            ReleasePayloadByteBudget(
                                byte_budget.get(),
                                prepared_chunk.size);
                            submit_failed = true;
                            submit_failure_status =
                                MapStorageStatus(
                                    submit_result.status_code());
                            submit_failure_detail =
                                "multi-chunk executor rejected task for chunk " +
                                std::to_string(prepared_chunk.chunk_index) +
                                ": " + submit_result.error_detail;
                            break;
                        }
                        ++submitted_chunk_tasks;
                    }

                    if (submit_failed)
                    {
                        std::unique_lock<std::mutex> lock(multi_state->mutex);
                        multi_state->cv.wait(lock, [&]()
                        {
                            return multi_state->completed_count >= submitted_chunk_tasks;
                        });
                        lock.unlock();

                        for (std::size_t si = 0; si < chunk_count; ++si)
                        {
                            if (multi_state->results[si].has_value())
                            {
                                durable_chunks.push_back(
                                    std::move(*multi_state->results[si]));
                            }
                        }
                        uncertain_cleanup_possible =
                            multi_state->uncertain_cleanup;
                        for (auto &diag : multi_state->diagnostics)
                        {
                            diag.request_id = request_.request_id;
                            result.diagnostics.push_back(std::move(diag));
                        }
                        BuildCleanupCandidates(
                            &result, durable_chunks,
                            uncertain_cleanup_possible);
                        Fail(&result,
                             submit_failure_status,
                             submit_failure_detail);
                        return result;
                    }

                    {
                        std::unique_lock<std::mutex> lock(multi_state->mutex);
                        multi_state->cv.wait(lock, [&]()
                        {
                            return multi_state->completed_count >= chunk_count;
                        });
                    }

                    for (auto &diag : multi_state->diagnostics)
                    {
                        result.diagnostics.push_back(std::move(diag));
                    }

                    if (multi_state->any_failed)
                    {
                        for (std::size_t si = 0; si < chunk_count; ++si)
                        {
                            if (multi_state->results[si].has_value())
                            {
                                durable_chunks.push_back(
                                    std::move(*multi_state->results[si]));
                            }
                        }
                        uncertain_cleanup_possible =
                            multi_state->uncertain_cleanup;
                        BuildCleanupCandidates(
                            &result,
                            multi_state->cleanup_durables,
                            uncertain_cleanup_possible,
                            uncertain_cleanup_possible
                                ? "upload failure left uncertain chunk placement facts after bounded multi-chunk pipeline; cleanup_candidate_possible stays true"
                                : std::string());
                        // T006 regression fix: extract the primary failure status
                        // from the first failed chunk (by chunk_index).
                        ObjectTransferStatusCode primary_status =
                            ObjectTransferStatusCode::kStorageRejected;
                        std::string primary_detail =
                            "multi-chunk upload detected one or more chunk failures; no CommitObject called";
                        for (std::size_t fi = 0; fi < chunk_count; ++fi)
                        {
                            if (!multi_state->results[fi].has_value() &&
                                multi_state->failure_facts[fi].second.size() > 0)
                            {
                                primary_status = multi_state->failure_facts[fi].first;
                                primary_detail =
                                    "chunk " + std::to_string(fi) + " failed: " +
                                    multi_state->failure_facts[fi].second +
                                    "; no CommitObject called";
                                break;
                            }
                        }
                        Fail(&result,
                             primary_status,
                             primary_detail);
                        return result;
                    }

                    result.committed_chunks.reserve(chunk_count);
                    for (std::size_t si = 0; si < chunk_count; ++si)
                    {
                        if (multi_state->results[si].has_value())
                        {
                            auto &committed = *multi_state->results[si];
                            durable_chunks.push_back(committed);
                            result.committed_chunks.push_back(
                                std::move(committed));
                        }
                    }
                }


                SetStage(ObjectTransferStage::kCommittingObject);
                mutable_snapshot().metadata_commit_attempted = true;
                result.commit_attempted = true;
                result.session = Snapshot();

                const auto commit_call = discovered_metadata_client->CommitObject(
                    {.request_id = MakeMetadataOperationRequestId(request_.request_id,
                                                                 "commit"),
                     .bucket = request_.bucket,
                     .object_key = request_.object_key,
                     .object_id = result.write_plan->object_id,
                     .version = result.write_plan->version,
                     .object_checksum = finalize_result.object_checksum,
                     .committed_chunks = result.committed_chunks,
                     .client_time_unix_ms = request_.client_time_unix_ms});
                AppendMetadataDiagnostics(commit_call.result.diagnostics,
                                          &result.diagnostics);
                if (!commit_call.transport_ok() || !commit_call.result.ok() ||
                    !commit_call.result.committed)
                {
                    BuildCleanupCandidates(
                        &result,
                        durable_chunks,
                        !durable_chunks.empty(),
                        durable_chunks.empty()
                            ? std::string()
                            : "CommitObject failed after chunk writes; durable chunks remain cleanup candidates until metadata authority confirms otherwise");
                    Fail(&result,
                         !commit_call.transport_ok()
                             ? ObjectTransferStatusCode::kMetadataRejected
                             : MapMetadataStatus(commit_call.result.summary.status),
                         !commit_call.transport_ok()
                             ? "Metadata CommitObject RPC failed: " +
                                   commit_call.rpc.grpc_error_message
                             : "Metadata CommitObject failed: " +
                                   commit_call.result.summary.message);
                    return result;
                }

                result.committed = true;
                mutable_snapshot().committed_visible = true;
                if (commit_call.result.committed_manifest.has_value())
                {
                    result.committed_manifest =
                        commit_call.result.committed_manifest;
                }
                else
                {
                    result.committed_manifest = BuildFallbackCommittedManifest(
                        *result.write_plan,
                        finalize_result.object_checksum,
                        result.committed_chunks,
                        request_.client_time_unix_ms);
                }
                result.cleanup_candidates.clear();
                result.cleanup_candidate_possible = false;

                MarkCompleted();
                result.status = ObjectTransferStatusCode::kOk;
                result.session = Snapshot();
                result.diagnostics.push_back(
                    MakeDiagnostic(
                        ObjectTransferStatusCode::kOk,
                        "upload session completed bounded chunk writes and CommitObject; COMMITTED visibility still comes only from MetadataNode authority",
                        request_.request_id));
                return result;
            }

        private:
            static TransferSessionSnapshot MakeInitialSnapshot(
                const UploadObjectRequest &request)
            {
                TransferSessionSnapshot snapshot;
                snapshot.direction = ObjectTransferDirection::kUpload;
                snapshot.stage = ObjectTransferStage::kPreparing;
                snapshot.request_id = request.request_id;
                snapshot.cluster_id = request.cluster_id;
                snapshot.bucket = request.bucket;
                snapshot.object_key = request.object_key;
                snapshot.object_id = request.object_id;
                snapshot.source_path = request.source_path;
                snapshot.chunk_size = request.chunk_size;
                snapshot.concurrency = request.concurrency;
                return snapshot;
            }

            [[nodiscard]] ObjectTransferStatusCode ValidateRequest(
                std::string *error_detail) const
            {
                if (request_.request_id.empty())
                {
                    SetErrorDetail(error_detail, "request_id is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.cluster_id.empty())
                {
                    SetErrorDetail(error_detail, "cluster_id is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.bucket.empty())
                {
                    SetErrorDetail(error_detail, "bucket is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.object_key.empty())
                {
                    SetErrorDetail(error_detail, "object_key is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.source_path.empty())
                {
                    SetErrorDetail(error_detail, "source_path is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.chunk_size == 0)
                {
                    SetErrorDetail(error_detail, "chunk_size must be greater than 0");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.concurrency == 0)
                {
                    SetErrorDetail(error_detail, "concurrency must be greater than 0");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.max_inflight_bytes < request_.chunk_size)
                {
                    SetErrorDetail(
                        error_detail,
                        "max_inflight_bytes must be greater than or equal to chunk_size");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.replica_fanout_concurrency == 0)
                {
                    SetErrorDetail(
                        error_detail,
                        "replica_fanout_concurrency must be greater than 0");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.desired_replica_count == 0)
                {
                    SetErrorDetail(error_detail,
                                   "desired_replica_count must be greater than 0");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.replica_fanout_concurrency >
                    request_.desired_replica_count)
                {
                    SetErrorDetail(
                        error_detail,
                        "replica_fanout_concurrency must be less than or equal to desired_replica_count");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.minimum_successful_writes == 0 ||
                    request_.minimum_successful_writes >
                        request_.desired_replica_count)
                {
                    SetErrorDetail(error_detail,
                                   "minimum_successful_writes must be in [1, desired_replica_count]");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                return ObjectTransferStatusCode::kOk;
            }

            void AppendConcurrencyDiagnostic(
                std::vector<ObjectTransferDiagnostic> *diagnostics) const
            {
                if (diagnostics == nullptr)
                {
                    return;
                }

                auto diagnostic = MakeDiagnostic(
                    ObjectTransferStatusCode::kOk,
                    DescribeConcurrencyBudget(
                        ObjectTransferDirection::kUpload,
                        session_budget_),
                    request_.request_id);
                diagnostics->push_back(std::move(diagnostic));
            }

            [[nodiscard]] std::optional<std::string> ValidateExpectedObjectChecksum(
                const TransferObjectChecksumFacts &actual,
                const TransferObjectChecksumFacts &expected) const
            {
                if (expected.size != 0 && actual.size != expected.size)
                {
                    return "object size does not match expected checksum facts";
                }
                if (expected.checksum.IsSet())
                {
                    if (actual.checksum.algorithm != expected.checksum.algorithm ||
                        actual.checksum.value != expected.checksum.value)
                    {
                        return "object checksum does not match expected checksum facts";
                    }
                }
                if (!expected.etag.empty() && actual.etag != expected.etag)
                {
                    return "object etag does not match expected checksum facts";
                }
                return std::nullopt;
            }

            void BuildCleanupCandidates(
                UploadObjectResult *result,
                const std::vector<TransferCommittedChunk> &durable_chunks,
                const bool uncertain_cleanup_possible,
                const std::string &uncertain_reason = std::string()) const
            {
                if (result == nullptr)
                {
                    return;
                }

                if (!durable_chunks.empty())
                {
                    const auto object_id =
                        !snapshot_ref().object_id.empty()
                            ? snapshot_ref().object_id
                            : (result->write_plan.has_value()
                                       ? result->write_plan->object_id
                                       : request_.object_id);
                    const auto version =
                        snapshot_ref().version != 0
                            ? snapshot_ref().version
                            : (result->write_plan.has_value()
                                   ? result->write_plan->version
                                   : 0);
                    const auto created_at_unix_ms =
                        result->write_plan.has_value()
                            ? result->write_plan->created_at_unix_ms
                            : request_.client_time_unix_ms;
                    result->cleanup_candidates =
                        BuildFailedUploadCleanupCandidatesFromChunks(
                            request_.bucket,
                            request_.object_key,
                            object_id,
                            version,
                            created_at_unix_ms,
                            durable_chunks);
                }

                result->cleanup_candidate_possible =
                    uncertain_cleanup_possible || !result->cleanup_candidates.empty();
                if (uncertain_cleanup_possible && !uncertain_reason.empty())
                {
                    auto diagnostic = MakeDiagnostic(
                        ObjectTransferStatusCode::kStorageRejected,
                        uncertain_reason,
                        request_.request_id);
                    result->diagnostics.push_back(std::move(diagnostic));
                }
            }

            [[nodiscard]] std::vector<StorageTransferTarget> ResolveChunkTargets(
                const TransferPreparedChunk &chunk,
                const std::unordered_map<StorageNodeId, StorageTransferTarget> &storage_targets,
                const std::uint32_t desired_replica_count,
                std::string *error_detail) const
            {
                const auto *chunk_plan = FindChunkPlan(chunk.chunk_index);
                if (chunk_plan == nullptr)
                {
                    const auto chunk_identity = chunk.chunk_index;
                    SetErrorDetail(
                        error_detail,
                        "upload write plan is missing chunk placement for chunk_index=" +
                            std::to_string(chunk_identity));
                    return {};
                }

                if (desired_replica_count != 0 &&
                    chunk_plan->required_replica_count != 0 &&
                    chunk_plan->required_replica_count != desired_replica_count)
                {
                    SetErrorDetail(
                        error_detail,
                        "upload write plan required_replica_count=" +
                            std::to_string(chunk_plan->required_replica_count) +
                            " does not match desired_replica_count=" +
                            std::to_string(desired_replica_count) +
                            " for chunk_id=" + chunk_plan->identity.chunk_id +
                            " chunk_index=" +
                            std::to_string(chunk_plan->identity.chunk_index));
                    return {};
                }

                return ResolveSelectedChunkTargetsFromPlan(*chunk_plan,
                                                           storage_targets,
                                                           error_detail);
            }

            [[nodiscard]] const TransferChunkPlan *FindChunkPlan(
                const std::uint32_t chunk_index) const
            {
                if (!current_write_plan_.has_value())
                {
                    return nullptr;
                }
                for (const auto &chunk_plan : current_write_plan_->chunks)
                {
                    if (chunk_plan.identity.chunk_index == chunk_index)
                    {
                        return &chunk_plan;
                    }
                }
                return nullptr;
            }

            [[nodiscard]] std::uint32_t ResolveMinimumSuccessfulWrites(
                const std::uint32_t chunk_index) const
            {
                const auto *chunk_plan = FindChunkPlan(chunk_index);
                if (chunk_plan != nullptr && chunk_plan->minimum_successful_writes != 0)
                {
                    return chunk_plan->minimum_successful_writes;
                }
                return request_.minimum_successful_writes;
            }

            [[nodiscard]] std::uint32_t ResolveDesiredReplicaCount(
                const std::uint32_t chunk_index) const
            {
                const auto *chunk_plan = FindChunkPlan(chunk_index);
                if (chunk_plan != nullptr && chunk_plan->required_replica_count != 0)
                {
                    return chunk_plan->required_replica_count;
                }
                return request_.desired_replica_count;
            }

            [[nodiscard]] ChunkIdentity BuildChunkIdentity(
                const TransferPreparedChunk &chunk,
                const std::string &object_id,
                const std::uint64_t version,
                std::string *error_detail) const
            {
                ChunkIdentity identity;
                identity.object_id = object_id;
                identity.version = version;
                identity.chunk_index = chunk.chunk_index;
                identity.offset = chunk.offset;
                const auto status = MakeChunkId(identity.object_id,
                                                identity.version,
                                                identity.chunk_index,
                                                &identity.chunk_id,
                                                error_detail);
                if (status != StorageNodeStatusCode::kOk)
                {
                    return {};
                }
                return identity;
            }

            void Fail(UploadObjectResult *result,
                      const ObjectTransferStatusCode status,
                      std::string message,
                      const std::uint32_t chunk_index = 0,
                      const std::uint64_t offset = 0,
                      const ChunkId &chunk_id = ChunkId(),
                      const std::string &node_id = std::string(),
                      const std::string &endpoint = std::string(),
                      const std::optional<bool> retryable_override = std::nullopt)
            {
                if (result == nullptr)
                {
                    return;
                }

                TransferFailureSummary failure;
                failure.status = status;
                failure.error_detail = message;
                failure.node_id = node_id;
                failure.endpoint = endpoint;
                failure.chunk_id = chunk_id;
                failure.chunk_index = chunk_index;
                failure.offset = offset;
                failure.retryable = retryable_override.value_or(
                    status == ObjectTransferStatusCode::kIoError ||
                    status == ObjectTransferStatusCode::kTimeout ||
                    status == ObjectTransferStatusCode::kDiscoveryUnavailable ||
                    status == ObjectTransferStatusCode::kMetadataNotLeader);
                SetFailure(failure);

                result->status = status;
                result->error_detail = std::move(message);
                result->session = Snapshot();
                auto diagnostic = MakeDiagnostic(result->status,
                                                 result->error_detail,
                                                 request_.request_id,
                                                 chunk_index,
                                                 offset,
                                                 chunk_id,
                                                 failure.retryable);
                diagnostic.node_id = node_id;
                diagnostic.endpoint = endpoint;
                result->diagnostics.push_back(std::move(diagnostic));
            }

            UploadObjectRequest request_;
            SessionConcurrencyBudget session_budget_{};
            std::shared_ptr<MetadataTransferClient> metadata_client_;
            std::shared_ptr<StorageTransferClient> storage_client_;
            std::shared_ptr<viewdemo::ViewNodeClient> view_client_;
            std::optional<TransferWritePlan> current_write_plan_;
        };

        class BasicDownloadTransferSession final : public DownloadTransferSession,
                                                   private BasicTransferSession
        {
        public:
            BasicDownloadTransferSession(
                DownloadObjectRequest request,
                std::shared_ptr<MetadataTransferClient> metadata_client,
                std::shared_ptr<StorageTransferClient> storage_client,
                std::shared_ptr<viewdemo::ViewNodeClient> view_client)
                : BasicTransferSession(MakeInitialSnapshot(request)),
                  request_(std::move(request)),
                  session_budget_(
                      ResolveSessionConcurrencyBudget(request_.concurrency, 0)),
                  metadata_client_(std::move(metadata_client)),
                  storage_client_(std::move(storage_client)),
                  view_client_(std::move(view_client))
            {
                mutable_snapshot().concurrency = session_budget_.effective_concurrency;
            }

            [[nodiscard]] ObjectTransferDirection direction() const override
            {
                return ObjectTransferDirection::kDownload;
            }

            [[nodiscard]] TransferSessionSnapshot Snapshot() const override
            {
                return snapshot_ref();
            }

            [[nodiscard]] bool finished() const override
            {
                return BasicTransferSession::finished();
            }

            [[nodiscard]] const DownloadObjectRequest &request() const override
            {
                return request_;
            }

            DownloadObjectResult Execute(
                TransferChecksumState &checksum_state) override
            {
                DownloadObjectResult result;
                result.session = Snapshot();

                const auto validation_status = ValidateRequest(&result.error_detail);
                if (validation_status != ObjectTransferStatusCode::kOk)
                {
                    Fail(&result, validation_status, result.error_detail);
                    return result;
                }

                SetStage(ObjectTransferStage::kDiscoveringMetadata);
                result.session = Snapshot();

                ObjectTransferStatusCode discovery_status =
                    ObjectTransferStatusCode::kOk;
                std::string discovery_error;
                const auto discovered_metadata_client = DiscoverMetadataClient(
                    request_.request_id,
                    request_.cluster_id,
                    metadata_client_,
                    view_client_,
                    &result.diagnostics,
                    &discovery_status,
                    &discovery_error);
                if (discovered_metadata_client == nullptr)
                {
                    TransferFailureSummary failure;
                    failure.status = discovery_status;
                    failure.error_detail = discovery_error;
                    failure.retryable = discovery_status ==
                                            ObjectTransferStatusCode::kDiscoveryUnavailable;
                    SetFailure(failure);
                    result.status = discovery_status;
                    result.error_detail = std::move(discovery_error);
                    result.session = Snapshot();
                    return result;
                }

                SetStage(ObjectTransferStage::kFetchingManifest);
                result.session = Snapshot();

                const auto manifest_call = discovered_metadata_client->GetObjectManifest(
                    {.request_id = request_.request_id,
                     .bucket = request_.bucket,
                     .object_key = request_.object_key,
                     .object_id = request_.object_id,
                     .version = request_.version,
                     .require_committed_visible = true});
                AppendMetadataDiagnostics(manifest_call.result.diagnostics,
                                          &result.diagnostics);
                if (!manifest_call.transport_ok() || !manifest_call.result.ok() ||
                    !manifest_call.result.manifest.has_value())
                {
                    const auto status = !manifest_call.transport_ok()
                                            ? ObjectTransferStatusCode::kMetadataRejected
                                            : MapMetadataStatus(
                                                  manifest_call.result.summary.status);
                    const std::string error =
                        !manifest_call.transport_ok()
                            ? "Metadata GetObjectManifest RPC failed: " +
                                  manifest_call.rpc.grpc_error_message
                            : "Metadata GetObjectManifest failed: " +
                                  manifest_call.result.summary.message;
                    TransferFailureSummary failure;
                    failure.status = status;
                    failure.error_detail = error;
                    failure.retryable = status ==
                                            ObjectTransferStatusCode::kMetadataNotLeader;
                    SetFailure(failure);
                    result.status = status;
                    result.error_detail = error;
                    result.session = Snapshot();
                    return result;
                }

                result.manifest = manifest_call.result.manifest;
                mutable_snapshot().object_id = result.manifest->object_id;
                mutable_snapshot().version = result.manifest->version;
                mutable_snapshot().committed_visible = true;
                mutable_snapshot().total_bytes =
                    result.manifest->object_checksum.size;
                mutable_snapshot().total_chunks = static_cast<std::uint32_t>(
                    result.manifest->chunks.size());

                std::vector<TransferCommittedChunk> ordered_chunks;
                if (const auto layout_error = ValidateManifestLayout(
                        *result.manifest,
                        &ordered_chunks);
                    layout_error.has_value())
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kConflict,
                         "COMMITTED manifest layout is invalid: " + *layout_error);
                    return result;
                }

                session_budget_ = ResolveSessionConcurrencyBudget(
                    request_.concurrency,
                    MaxManifestChunkSize(ordered_chunks));
                mutable_snapshot().concurrency = session_budget_.effective_concurrency;
                AppendConcurrencyDiagnostic(&result.diagnostics);
                result.session = Snapshot();

                std::string storage_discovery_error;
                const auto storage_targets = DiscoverStorageTargets(
                    request_.request_id,
                    request_.cluster_id,
                    0,
                    0,
                    false,
                    view_client_,
                    &result.diagnostics,
                    &discovery_status,
                    &storage_discovery_error);
                if (storage_targets.empty())
                {
                    Fail(&result, discovery_status, std::move(storage_discovery_error));
                    return result;
                }

                const auto temp_path = MakeTemporaryDownloadPath(
                    request_.destination_path,
                    request_.request_id);
                const auto cleanup_temp = [&temp_path]()
                {
                    RemovePathIfExists(temp_path);
                };

                std::error_code path_ec;
                const auto parent_path = request_.destination_path.parent_path();
                if (!parent_path.empty() &&
                    !std::filesystem::exists(parent_path, path_ec))
                {
                    Fail(&result,
                         path_ec ? ObjectTransferStatusCode::kIoError
                                 : ObjectTransferStatusCode::kNotFound,
                         path_ec ? "failed to access destination directory: " +
                                       path_ec.message()
                                 : "destination directory does not exist");
                    return result;
                }
                if (path_ec)
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kIoError,
                         "failed to access destination directory: " +
                             path_ec.message());
                    return result;
                }
                if (std::filesystem::exists(request_.destination_path, path_ec))
                {
                    if (path_ec)
                    {
                        Fail(&result,
                             ObjectTransferStatusCode::kIoError,
                             "failed to inspect destination_path: " +
                                 path_ec.message());
                    }
                    else
                    {
                        Fail(&result,
                             ObjectTransferStatusCode::kConflict,
                             "destination_path already exists");
                    }
                    return result;
                }
                RemovePathIfExists(temp_path);

                checksum_state.Reset();
                SetStage(ObjectTransferStage::kDownloadingChunks);
                result.session = Snapshot();

                std::ofstream output(temp_path,
                                     std::ios::binary | std::ios::trunc);
                if (!output.is_open())
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kIoError,
                         "failed to open temporary download file for writing");
                    cleanup_temp();
                    return result;
                }
                const auto close_output = [&output]()
                {
                    if (output.is_open())
                    {
                        output.close();
                    }
                };

                for (const auto &chunk : ordered_chunks)
                {
                    const auto manifest_targets =
                        ResolveManifestReplicaTargets(chunk, storage_targets);
                    if (manifest_targets.empty())
                    {
                        Fail(&result,
                             ObjectTransferStatusCode::kDiscoveryUnavailable,
                             "ViewNode did not provide a readable endpoint for manifest replica_nodes",
                             chunk.identity.chunk_index,
                             chunk.identity.offset);
                        close_output();
                        cleanup_temp();
                        return result;
                    }

                    std::optional<StorageTransferReadResult> successful_read;
                    std::optional<StorageTransferTarget> successful_target;
                    std::vector<DownloadReplicaAttemptFailure> attempt_failures;
                    attempt_failures.reserve(manifest_targets.size());
                    for (const auto &target : manifest_targets)
                    {
                        const auto read_result = storage_client_->ReadChunk(
                            {.request_id = request_.request_id,
                             .target = target,
                             .identity = chunk.identity,
                             .expected_checksum = chunk.checksum,
                             .verify_checksum = true});

                        if (!read_result.ok())
                        {
                            auto failure =
                                BuildReadResultFailure(target, read_result);
                            result.diagnostics.push_back(
                                MakeDiagnostic(
                                    failure.status,
                                    BuildReplicaAttemptDiagnosticMessage(failure),
                                    request_.request_id,
                                    chunk.identity.chunk_index,
                                    chunk.identity.offset,
                                    chunk.identity.chunk_id,
                                    failure.retryable));
                            result.diagnostics.back().node_id = failure.node_id;
                            result.diagnostics.back().endpoint = failure.endpoint;
                            attempt_failures.push_back(std::move(failure));
                            continue;
                        }

                        if (read_result.metadata.state == ChunkState::kCorrupted)
                        {
                            auto failure = MakeDownloadReplicaAttemptFailure(
                                target,
                                ObjectTransferStatusCode::kChecksumMismatch,
                                "corruption",
                                "StorageNode returned corrupted chunk state for COMMITTED manifest replica");
                            result.diagnostics.push_back(
                                MakeDiagnostic(
                                    failure.status,
                                    BuildReplicaAttemptDiagnosticMessage(failure),
                                    request_.request_id,
                                    chunk.identity.chunk_index,
                                    chunk.identity.offset,
                                    chunk.identity.chunk_id,
                                    failure.retryable));
                            result.diagnostics.back().node_id = failure.node_id;
                            result.diagnostics.back().endpoint = failure.endpoint;
                            attempt_failures.push_back(std::move(failure));
                            continue;
                        }

                        if (static_cast<std::uint64_t>(read_result.payload.size()) !=
                            chunk.size)
                        {
                            auto failure = MakeDownloadReplicaAttemptFailure(
                                target,
                                ObjectTransferStatusCode::kConflict,
                                "size mismatch",
                                "expected_size=" + std::to_string(chunk.size) +
                                    ", actual_size=" +
                                    std::to_string(read_result.payload.size()));
                            result.diagnostics.push_back(
                                MakeDiagnostic(
                                    failure.status,
                                    BuildReplicaAttemptDiagnosticMessage(failure),
                                    request_.request_id,
                                    chunk.identity.chunk_index,
                                    chunk.identity.offset,
                                    chunk.identity.chunk_id,
                                    failure.retryable));
                            result.diagnostics.back().node_id = failure.node_id;
                            result.diagnostics.back().endpoint = failure.endpoint;
                            attempt_failures.push_back(std::move(failure));
                            continue;
                        }

                        const auto checksum_update = checksum_state.Append(
                            {.chunk_index = chunk.identity.chunk_index,
                             .offset = chunk.identity.offset,
                             .payload = read_result.payload,
                             .expected_chunk_checksum = chunk.checksum});
                        if (!checksum_update.ok())
                        {
                            if (checksum_update.status !=
                                ObjectTransferStatusCode::kChecksumMismatch)
                            {
                                Fail(&result,
                                     checksum_update.status,
                                     checksum_update.error_detail,
                                     chunk.identity.chunk_index,
                                     chunk.identity.offset,
                                     target.node_id,
                                     target.endpoint,
                                     chunk.identity.chunk_id,
                                     false);
                                close_output();
                                cleanup_temp();
                                return result;
                            }

                            auto failure = MakeDownloadReplicaAttemptFailure(
                                target,
                                checksum_update.status,
                                "checksum mismatch",
                                checksum_update.error_detail);
                            result.diagnostics.push_back(
                                MakeDiagnostic(
                                    failure.status,
                                    BuildReplicaAttemptDiagnosticMessage(failure),
                                    request_.request_id,
                                    chunk.identity.chunk_index,
                                    chunk.identity.offset,
                                    chunk.identity.chunk_id,
                                    failure.retryable));
                            result.diagnostics.back().node_id = failure.node_id;
                            result.diagnostics.back().endpoint = failure.endpoint;
                            attempt_failures.push_back(std::move(failure));
                            continue;
                        }

                        successful_read = read_result;
                        successful_target = target;
                        break;
                    }

                    if (!successful_read.has_value())
                    {
                        const auto fallback_status =
                            AggregateChunkReadFailureStatus(attempt_failures);
                        const auto aggregated_detail =
                            BuildAggregatedChunkReadFailureDetail(
                                chunk,
                                attempt_failures);
                        Fail(&result,
                             fallback_status,
                             aggregated_detail,
                             chunk.identity.chunk_index,
                             chunk.identity.offset,
                             !attempt_failures.empty()
                                 ? attempt_failures.back().node_id
                                 : std::string(),
                             !attempt_failures.empty()
                                 ? attempt_failures.back().endpoint
                                 : std::string(),
                             chunk.identity.chunk_id,
                             !attempt_failures.empty() &&
                                 attempt_failures.back().retryable);
                        close_output();
                        cleanup_temp();
                        return result;
                    }

                    const auto &read_result = *successful_read;
                    const auto &selected_target = *successful_target;

                    output.seekp(static_cast<std::streamoff>(chunk.identity.offset),
                                 std::ios::beg);
                    if (!output)
                    {
                        Fail(&result,
                             ObjectTransferStatusCode::kIoError,
                             "failed to seek temporary download file",
                             chunk.identity.chunk_index,
                             chunk.identity.offset,
                             selected_target.node_id,
                             selected_target.endpoint,
                             chunk.identity.chunk_id,
                             false);
                        close_output();
                        cleanup_temp();
                        return result;
                    }

                    output.write(read_result.payload.data(),
                                 static_cast<std::streamsize>(
                                     read_result.payload.size()));
                    if (!output)
                    {
                        Fail(&result,
                             ObjectTransferStatusCode::kIoError,
                             "failed to write chunk payload into temporary download file",
                             chunk.identity.chunk_index,
                             chunk.identity.offset,
                             selected_target.node_id,
                             selected_target.endpoint,
                             chunk.identity.chunk_id,
                             false);
                        close_output();
                        cleanup_temp();
                        return result;
                    }

                    mutable_snapshot().bytes_completed += chunk.size;
                    mutable_snapshot().chunks_completed += 1;
                    result.session = Snapshot();
                }

                output.flush();
                if (!output)
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kIoError,
                         "failed to flush temporary download file");
                    close_output();
                    cleanup_temp();
                    return result;
                }
                close_output();
                if (!output)
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kIoError,
                         "failed to finalize temporary download file");
                    cleanup_temp();
                    return result;
                }

                SetStage(ObjectTransferStage::kVerifyingChecksums);
                result.session = Snapshot();

                const auto finalize_result = checksum_state.Finalize();
                if (!finalize_result.ok())
                {
                    Fail(&result,
                         finalize_result.status,
                         finalize_result.error_detail);
                    close_output();
                    cleanup_temp();
                    return result;
                }

                result.downloaded_object_checksum = finalize_result.object_checksum;
                if (const auto mismatch_reason = ValidateObjectChecksumFacts(
                        finalize_result.object_checksum,
                        result.manifest->object_checksum);
                    mismatch_reason.has_value())
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kChecksumMismatch,
                         *mismatch_reason);
                    close_output();
                    cleanup_temp();
                    return result;
                }
                if (request_.expected_object_checksum.has_value())
                {
                    if (const auto mismatch_reason = ValidateObjectChecksumFacts(
                            finalize_result.object_checksum,
                            *request_.expected_object_checksum);
                        mismatch_reason.has_value())
                    {
                        Fail(&result,
                             ObjectTransferStatusCode::kChecksumMismatch,
                             *mismatch_reason);
                        close_output();
                        cleanup_temp();
                        return result;
                    }
                }

                std::filesystem::rename(temp_path,
                                        request_.destination_path,
                                        path_ec);
                if (path_ec)
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kIoError,
                         "failed to publish temporary download file: " +
                             path_ec.message());
                    cleanup_temp();
                    return result;
                }

                mutable_snapshot().final_checksum_verified = true;
                result.checksum_verified = true;
                result.status = ObjectTransferStatusCode::kOk;
                MarkCompleted();
                result.session = Snapshot();
                result.diagnostics.push_back(
                    MakeDiagnostic(
                        ObjectTransferStatusCode::kOk,
                        "download reconstructed from MetadataNode COMMITTED manifest with per-chunk and final object checksum verification",
                        request_.request_id));
                return result;
            }

        private:
            static TransferSessionSnapshot MakeInitialSnapshot(
                const DownloadObjectRequest &request)
            {
                TransferSessionSnapshot snapshot;
                snapshot.direction = ObjectTransferDirection::kDownload;
                snapshot.stage = ObjectTransferStage::kPreparing;
                snapshot.request_id = request.request_id;
                snapshot.cluster_id = request.cluster_id;
                snapshot.bucket = request.bucket;
                snapshot.object_key = request.object_key;
                snapshot.object_id = request.object_id;
                snapshot.version = request.version.value_or(0);
                snapshot.destination_path = request.destination_path;
                snapshot.concurrency = request.concurrency;
                return snapshot;
            }

            [[nodiscard]] ObjectTransferStatusCode ValidateRequest(
                std::string *error_detail) const
            {
                if (request_.request_id.empty())
                {
                    SetErrorDetail(error_detail, "request_id is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.cluster_id.empty())
                {
                    SetErrorDetail(error_detail, "cluster_id is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.bucket.empty())
                {
                    SetErrorDetail(error_detail, "bucket is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.object_key.empty())
                {
                    SetErrorDetail(error_detail, "object_key is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.destination_path.empty())
                {
                    SetErrorDetail(error_detail, "destination_path is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.concurrency == 0)
                {
                    SetErrorDetail(error_detail, "concurrency must be greater than 0");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (storage_client_ == nullptr)
                {
                    SetErrorDetail(error_detail,
                                   "storage_client is required for manifest-driven download");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                return ObjectTransferStatusCode::kOk;
            }

            void AppendConcurrencyDiagnostic(
                std::vector<ObjectTransferDiagnostic> *diagnostics) const
            {
                if (diagnostics == nullptr)
                {
                    return;
                }

                auto diagnostic = MakeDiagnostic(
                    ObjectTransferStatusCode::kOk,
                    DescribeConcurrencyBudget(
                        ObjectTransferDirection::kDownload,
                        session_budget_),
                    request_.request_id);
                diagnostics->push_back(std::move(diagnostic));
            }

            void Fail(DownloadObjectResult *result,
                      const ObjectTransferStatusCode status,
                      std::string message,
                      const std::uint32_t chunk_index = 0,
                      const std::uint64_t offset = 0,
                      std::string node_id = {},
                      std::string endpoint = {},
                      ChunkId chunk_id = {},
                      const bool retryable = false)
            {
                if (result == nullptr)
                {
                    return;
                }

                TransferFailureSummary failure;
                failure.status = status;
                failure.error_detail = message;
                failure.node_id = node_id;
                failure.endpoint = endpoint;
                failure.chunk_id = chunk_id;
                failure.chunk_index = chunk_index;
                failure.offset = offset;
                failure.retryable = retryable;
                SetFailure(failure);

                result->status = status;
                result->error_detail = std::move(message);
                result->session = Snapshot();

                auto diagnostic = MakeDiagnostic(result->status,
                                                 result->error_detail,
                                                 request_.request_id,
                                                 chunk_index,
                                                 offset,
                                                 chunk_id,
                                                 retryable);
                diagnostic.node_id = std::move(node_id);
                diagnostic.endpoint = std::move(endpoint);
                result->diagnostics.push_back(std::move(diagnostic));
            }

            DownloadObjectRequest request_;
            SessionConcurrencyBudget session_budget_{};
            std::shared_ptr<MetadataTransferClient> metadata_client_;
            std::shared_ptr<StorageTransferClient> storage_client_;
            std::shared_ptr<viewdemo::ViewNodeClient> view_client_;
        };
    } // namespace

    std::vector<StorageTransferTarget> ResolveSelectedChunkTargetsForTesting(
        const TransferChunkPlan &chunk_plan,
        const std::unordered_map<StorageNodeId, StorageTransferTarget> &storage_targets,
        std::string *error_detail)
    {
        return ResolveSelectedChunkTargetsFromPlan(chunk_plan,
                                                   storage_targets,
                                                   error_detail);
    }

    TransferChunkReader::~TransferChunkReader() = default;

    std::unique_ptr<TransferChunkReader> CreateFileTransferChunkReader()
    {
        return std::make_unique<FileTransferChunkReader>();
    }

    TransferChecksumState::~TransferChecksumState() = default;

    std::unique_ptr<TransferChecksumState> CreateTransferChecksumState()
    {
        return std::make_unique<IncrementalTransferChecksumState>();
    }

    TransferSession::~TransferSession() = default;

    UploadTransferSession::~UploadTransferSession() = default;

    void UploadTransferSession::SetMaxInflightPayloadBytesOverrideForTesting(
    const std::uint64_t max_bytes)
{
    g_test_max_inflight_bytes_override = max_bytes;
}

DownloadTransferSession::~DownloadTransferSession() = default;

    ObjectTransfer::ObjectTransfer(
        std::shared_ptr<MetadataTransferClient> metadata_client,
        std::shared_ptr<StorageTransferClient> storage_client,
        std::shared_ptr<viewdemo::ViewNodeClient> view_client)
        : metadata_client_(std::move(metadata_client)),
          storage_client_(std::move(storage_client)),
          view_client_(std::move(view_client))
    {
    }

    ObjectTransfer::~ObjectTransfer() = default;

    ObjectTransfer::ObjectTransfer(ObjectTransfer &&) noexcept = default;

    ObjectTransfer &ObjectTransfer::operator=(ObjectTransfer &&) noexcept = default;

    std::unique_ptr<UploadTransferSession> ObjectTransfer::StartUploadSession(
        const UploadObjectRequest &request) const
    {
        return std::make_unique<BasicUploadTransferSession>(request,
                                                            metadata_client_,
                                                            storage_client_,
                                                            view_client_);
    }

    std::unique_ptr<DownloadTransferSession> ObjectTransfer::StartDownloadSession(
        const DownloadObjectRequest &request) const
    {
        return std::make_unique<BasicDownloadTransferSession>(request,
                                                              metadata_client_,
                                                              storage_client_,
                                                              view_client_);
    }

    const std::shared_ptr<MetadataTransferClient> &ObjectTransfer::metadata_client()
        const
    {
        return metadata_client_;
    }

    const std::shared_ptr<StorageTransferClient> &ObjectTransfer::storage_client()
        const
    {
        return storage_client_;
    }

    const std::shared_ptr<viewdemo::ViewNodeClient> &ObjectTransfer::view_client()
        const
    {
        return view_client_;
    }
} // namespace storedemo
