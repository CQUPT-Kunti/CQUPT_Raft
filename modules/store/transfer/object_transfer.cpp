#include "store/transfer/object_transfer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <system_error>
#include <utility>

namespace storedemo
{
    namespace
    {
        constexpr std::array<std::uint32_t, 8> kSha256InitialState = {
            0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

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
            explicit BasicUploadTransferSession(UploadObjectRequest request)
                : BasicTransferSession(MakeInitialSnapshot(request)),
                  request_(std::move(request))
            {
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

                const auto validation_status = ValidateRequest(&result.error_detail);
                if (validation_status != ObjectTransferStatusCode::kOk)
                {
                    Fail(&result, validation_status, result.error_detail);
                    return result;
                }

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

                SetStage(ObjectTransferStage::kUploadingChunks);
                result.session = Snapshot();

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

                MarkCompleted();
                result.status = ObjectTransferStatusCode::kOk;
                result.session = Snapshot();
                result.diagnostics.push_back(
                    MakeDiagnostic(ObjectTransferStatusCode::kOk,
                                   "bounded upload session prepared chunk facts only; no metadata or storage RPC executed",
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
                return ObjectTransferStatusCode::kOk;
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

            void Fail(UploadObjectResult *result,
                      const ObjectTransferStatusCode status,
                      std::string message,
                      const std::uint32_t chunk_index = 0,
                      const std::uint64_t offset = 0)
            {
                if (result == nullptr)
                {
                    return;
                }

                TransferFailureSummary failure;
                failure.status = status;
                failure.error_detail = message;
                failure.chunk_index = chunk_index;
                failure.offset = offset;
                failure.retryable = status == ObjectTransferStatusCode::kIoError ||
                                    status == ObjectTransferStatusCode::kTimeout;
                SetFailure(failure);

                result->status = status;
                result->error_detail = std::move(message);
                result->session = Snapshot();
                result->diagnostics.push_back(
                    MakeDiagnostic(result->status,
                                   result->error_detail,
                                   request_.request_id,
                                   chunk_index,
                                   offset,
                                   ChunkId(),
                                   failure.retryable));
            }

            UploadObjectRequest request_;
        };

        class BasicDownloadTransferSession final : public DownloadTransferSession,
                                                   private BasicTransferSession
        {
        public:
            explicit BasicDownloadTransferSession(DownloadObjectRequest request)
                : BasicTransferSession(MakeInitialSnapshot(request)),
                  request_(std::move(request))
            {
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
                TransferChecksumState & /*checksum_state*/) override
            {
                DownloadObjectResult result;
                result.status = ObjectTransferStatusCode::kUnsupported;
                result.error_detail =
                    "download transfer session is not implemented until manifest/adapter tasks";

                TransferFailureSummary failure;
                failure.status = result.status;
                failure.error_detail = result.error_detail;
                SetFailure(failure);

                result.session = Snapshot();
                result.diagnostics.push_back(
                    MakeDiagnostic(result.status, result.error_detail, request_.request_id));
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
                snapshot.bucket = request.bucket;
                snapshot.object_key = request.object_key;
                snapshot.object_id = request.object_id;
                snapshot.version = request.version.value_or(0);
                snapshot.destination_path = request.destination_path;
                snapshot.concurrency = request.concurrency;
                return snapshot;
            }

            DownloadObjectRequest request_;
        };
    } // namespace

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
        return std::make_unique<BasicUploadTransferSession>(request);
    }

    std::unique_ptr<DownloadTransferSession> ObjectTransfer::StartDownloadSession(
        const DownloadObjectRequest &request) const
    {
        return std::make_unique<BasicDownloadTransferSession>(request);
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
