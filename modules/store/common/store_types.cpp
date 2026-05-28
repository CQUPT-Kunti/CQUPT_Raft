#include "store/common/store_types.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>

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

        void SetErrorDetail(std::string *error_detail, const std::string_view detail)
        {
            if (error_detail != nullptr)
            {
                error_detail->assign(detail);
            }
        }

        bool IsAllowedChunkObjectIdChar(const char ch)
        {
            const unsigned char value = static_cast<unsigned char>(ch);
            return std::isalnum(value) != 0 || ch == '-' || ch == '_' || ch == '.';
        }

        bool HasCanonicalUnsignedEncoding(const std::string_view text)
        {
            return text.size() == 1 || text.front() != '0';
        }

        template <typename UInt>
        bool ParseUnsigned(const std::string_view text, UInt *out_value)
        {
            if (out_value == nullptr || text.empty())
            {
                return false;
            }

            UInt value = 0;
            const char *begin = text.data();
            const char *end = begin + text.size();
            const auto [ptr, ec] = std::from_chars(begin, end, value);
            if (ec != std::errc{} || ptr != end)
            {
                return false;
            }

            *out_value = value;
            return true;
        }

        std::uint32_t RotateRight(const std::uint32_t value, const std::uint32_t bits)
        {
            return (value >> bits) | (value << (32U - bits));
        }

        std::uint32_t LoadBigEndianWord(const std::uint8_t *bytes)
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
                            state->buffer.begin() + static_cast<std::ptrdiff_t>(state->buffer_size));
                state->buffer_size += copy_size;
                offset += copy_size;

                if (state->buffer_size == state->buffer.size())
                {
                    ProcessSha256Block(state, state->buffer.data());
                    state->buffer_size = 0;
                }
            }
        }

        std::array<std::uint8_t, kSha256DigestBytes> FinalizeSha256(Sha256State state)
        {
            const std::uint64_t total_bits = state.total_bytes * 8U;

            state.buffer[state.buffer_size++] = 0x80U;
            if (state.buffer_size > 56U)
            {
                std::fill(state.buffer.begin() + static_cast<std::ptrdiff_t>(state.buffer_size),
                          state.buffer.end(),
                          static_cast<std::uint8_t>(0));
                ProcessSha256Block(&state, state.buffer.data());
                state.buffer_size = 0;
            }

            std::fill(state.buffer.begin() + static_cast<std::ptrdiff_t>(state.buffer_size),
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

        std::string EncodeLowerHex(const std::uint8_t *bytes, const std::size_t size)
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

        ChunkChecksum ComputeSha256Checksum(const std::string_view payload)
        {
            Sha256State state;
            const auto *bytes =
                reinterpret_cast<const std::uint8_t *>(payload.data());
            UpdateSha256(&state, bytes, payload.size());
            const auto digest = FinalizeSha256(state);

            ChunkChecksum checksum;
            checksum.algorithm = ChunkChecksumAlgorithm::kSha256;
            checksum.value = EncodeLowerHex(digest.data(), digest.size());
            checksum.size_bytes = payload.size();
            checksum.computed_at = 0;
            return checksum;
        }
    }

    const char *ToString(const StorageNodeStatusCode code)
    {
        switch (code)
        {
        case StorageNodeStatusCode::kOk:
            return "Ok";
        case StorageNodeStatusCode::kAlreadyExists:
            return "AlreadyExists";
        case StorageNodeStatusCode::kNotFound:
            return "NotFound";
        case StorageNodeStatusCode::kConflict:
            return "Conflict";
        case StorageNodeStatusCode::kChecksumMismatch:
            return "ChecksumMismatch";
        case StorageNodeStatusCode::kCorrupted:
            return "Corrupted";
        case StorageNodeStatusCode::kDiskFull:
            return "DiskFull";
        case StorageNodeStatusCode::kPermissionDenied:
            return "PermissionDenied";
        case StorageNodeStatusCode::kIoError:
            return "IoError";
        case StorageNodeStatusCode::kTimeout:
            return "Timeout";
        case StorageNodeStatusCode::kCancelled:
            return "Cancelled";
        case StorageNodeStatusCode::kOverloaded:
            return "Overloaded";
        case StorageNodeStatusCode::kNodeUnavailable:
            return "NodeUnavailable";
        case StorageNodeStatusCode::kUnsupported:
            return "Unsupported";
        case StorageNodeStatusCode::kInvalidArgument:
            return "InvalidArgument";
}

        return "Unknown";
    }

    const char *ToString(const ChunkState state)
    {
        switch (state)
        {
        case ChunkState::kStaging:
            return "Staging";
        case ChunkState::kLive:
            return "Live";
        case ChunkState::kDeleting:
            return "Deleting";
        case ChunkState::kDeleted:
            return "Deleted";
        case ChunkState::kQuarantined:
            return "Quarantined";
        case ChunkState::kCorrupted:
            return "Corrupted";
        case ChunkState::kMissing:
            return "Missing";
        }

        return "Unknown";
    }

    bool IsRetriableStatus(const StorageNodeStatusCode code)
    {
        switch (code)
        {
        case StorageNodeStatusCode::kTimeout:
        case StorageNodeStatusCode::kIoError:
        case StorageNodeStatusCode::kOverloaded:
        case StorageNodeStatusCode::kNodeUnavailable:
            return true;
        case StorageNodeStatusCode::kOk:
        case StorageNodeStatusCode::kAlreadyExists:
        case StorageNodeStatusCode::kNotFound:
        case StorageNodeStatusCode::kConflict:
        case StorageNodeStatusCode::kChecksumMismatch:
        case StorageNodeStatusCode::kCorrupted:
        case StorageNodeStatusCode::kDiskFull:
        case StorageNodeStatusCode::kPermissionDenied:
        case StorageNodeStatusCode::kCancelled:
        case StorageNodeStatusCode::kUnsupported:
        case StorageNodeStatusCode::kInvalidArgument:
        default:
            return false;
        }
    }

    bool IsReadableChunkState(const ChunkState state)
    {
        return state == ChunkState::kLive;
    }

    bool IsTerminalChunkState(const ChunkState state)
    {
        switch (state)
        {
        case ChunkState::kDeleted:
        case ChunkState::kQuarantined:
        case ChunkState::kCorrupted:
        case ChunkState::kMissing:
            return true;
        case ChunkState::kStaging:
        case ChunkState::kLive:
        case ChunkState::kDeleting:
        default:
            return false;
        }
    }

    StorageNodeStatusCode ComputeChunkChecksum(const std::string_view payload,
                                               ChunkChecksum *out_checksum,
                                               std::string *error_detail)
    {
        if (out_checksum == nullptr)
        {
            SetErrorDetail(error_detail, "out_checksum must not be null");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        *out_checksum = ComputeSha256Checksum(payload);
        return StorageNodeStatusCode::kOk;
    }

    StorageNodeStatusCode VerifyChunkChecksum(std::string_view payload,
                                              const ChunkChecksum &expected_checksum,
                                              ChunkChecksum *out_actual_checksum,
                                              std::string *error_detail)
    {
        if (expected_checksum.algorithm == ChunkChecksumAlgorithm::kUnknown)
        {
            SetErrorDetail(error_detail, "expected_checksum algorithm must be set");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (expected_checksum.algorithm != ChunkChecksumAlgorithm::kSha256)
        {
            SetErrorDetail(error_detail, "expected_checksum algorithm is not supported");
            return StorageNodeStatusCode::kUnsupported;
        }

        if (expected_checksum.value.size() != kSha256DigestHexChars)
        {
            SetErrorDetail(error_detail, "expected_checksum value must be 64 hex chars");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        ChunkChecksum actual_checksum;
        const StorageNodeStatusCode compute_status =
            ComputeChunkChecksum(payload, &actual_checksum, error_detail);
        if (compute_status != StorageNodeStatusCode::kOk)
        {
            return compute_status;
        }

        if (out_actual_checksum != nullptr)
        {
            *out_actual_checksum = actual_checksum;
        }

        if (expected_checksum.size_bytes != actual_checksum.size_bytes ||
            expected_checksum.value != actual_checksum.value)
        {
            SetErrorDetail(error_detail, "payload checksum mismatch");
            return StorageNodeStatusCode::kChecksumMismatch;
        }

        return StorageNodeStatusCode::kOk;
    }

    StorageNodeStatusCode ValidateChunkObjectId(const std::string_view object_id,
                                                std::string *error_detail)
    {
        if (object_id.empty())
        {
            SetErrorDetail(error_detail, "object_id must not be empty");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (object_id.size() > kMaxChunkObjectIdLength)
        {
            SetErrorDetail(error_detail, "object_id exceeds chunk filename safe length");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (object_id == "." || object_id == "..")
        {
            SetErrorDetail(error_detail, "object_id must not be relative path segment");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (object_id.find("..") != std::string_view::npos)
        {
            SetErrorDetail(error_detail, "object_id must not contain path escape marker");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (object_id.front() == '.' || object_id.back() == '.')
        {
            SetErrorDetail(error_detail,
                           "object_id must not start or end with '.'");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (object_id.find(kChunkIdSeparator) != std::string_view::npos)
        {
            SetErrorDetail(error_detail,
                           "object_id must not contain chunk id separator");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        for (const char ch : object_id)
        {
            if (!IsAllowedChunkObjectIdChar(ch))
            {
                SetErrorDetail(error_detail,
                               "object_id contains path separator or unsafe character");
                return StorageNodeStatusCode::kInvalidArgument;
            }
        }

        return StorageNodeStatusCode::kOk;
    }

    StorageNodeStatusCode MakeChunkId(const std::string_view object_id,
                                      const std::uint64_t version,
                                      const std::uint32_t chunk_index,
                                      ChunkId *out_chunk_id,
                                      std::string *error_detail)
    {
        if (out_chunk_id == nullptr)
        {
            SetErrorDetail(error_detail, "out_chunk_id must not be null");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        const StorageNodeStatusCode object_id_status =
            ValidateChunkObjectId(object_id, error_detail);
        if (object_id_status != StorageNodeStatusCode::kOk)
        {
            return object_id_status;
        }

        if (version == 0)
        {
            SetErrorDetail(error_detail, "version must be greater than zero");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        const std::string chunk_id = std::string(object_id) + kChunkIdSeparator +
                                     std::to_string(version) + kChunkIdSeparator +
                                     std::to_string(chunk_index);
        if (chunk_id.size() > kMaxChunkIdLength)
        {
            SetErrorDetail(error_detail, "chunk_id exceeds filename safe length");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        *out_chunk_id = chunk_id;
        return StorageNodeStatusCode::kOk;
    }

    StorageNodeStatusCode ParseChunkId(const std::string_view chunk_id,
                                       ChunkIdentity *out_identity,
                                       std::string *error_detail)
    {
        if (chunk_id.empty())
        {
            SetErrorDetail(error_detail, "chunk_id must not be empty");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (chunk_id.size() > kMaxChunkIdLength)
        {
            SetErrorDetail(error_detail, "chunk_id exceeds filename safe length");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        const std::size_t first_separator = chunk_id.find(kChunkIdSeparator);
        if (first_separator == std::string_view::npos)
        {
            SetErrorDetail(error_detail, "chunk_id missing first separator");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        const std::size_t second_separator =
            chunk_id.find(kChunkIdSeparator, first_separator + 1U);
        if (second_separator == std::string_view::npos)
        {
            SetErrorDetail(error_detail, "chunk_id missing second separator");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (chunk_id.find(kChunkIdSeparator, second_separator + 1U) !=
            std::string_view::npos)
        {
            SetErrorDetail(error_detail, "chunk_id has unexpected extra separator");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        const std::string_view object_id = chunk_id.substr(0, first_separator);
        const std::string_view version_text =
            chunk_id.substr(first_separator + 1U,
                            second_separator - first_separator - 1U);
        const std::string_view chunk_index_text =
            chunk_id.substr(second_separator + 1U);

        const StorageNodeStatusCode object_id_status =
            ValidateChunkObjectId(object_id, error_detail);
        if (object_id_status != StorageNodeStatusCode::kOk)
        {
            return object_id_status;
        }

        if (version_text.empty() || chunk_index_text.empty())
        {
            SetErrorDetail(error_detail, "chunk_id must contain non-empty version and chunk_index");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (!HasCanonicalUnsignedEncoding(version_text))
        {
            SetErrorDetail(error_detail, "version must use canonical unsigned encoding");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (!HasCanonicalUnsignedEncoding(chunk_index_text))
        {
            SetErrorDetail(error_detail, "chunk_index must use canonical unsigned encoding");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        std::uint64_t version = 0;
        if (!ParseUnsigned(version_text, &version))
        {
            SetErrorDetail(error_detail, "version is not a valid uint64");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (version == 0)
        {
            SetErrorDetail(error_detail, "version must be greater than zero");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        std::uint32_t chunk_index = 0;
        if (!ParseUnsigned(chunk_index_text, &chunk_index))
        {
            SetErrorDetail(error_detail, "chunk_index is not a valid uint32");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (out_identity != nullptr)
        {
            out_identity->chunk_id = std::string(chunk_id);
            out_identity->object_id = std::string(object_id);
            out_identity->version = version;
            out_identity->chunk_index = chunk_index;
            out_identity->offset = 0;
        }

        return StorageNodeStatusCode::kOk;
    }

    StorageNodeStatusCode ValidateChunkId(const std::string_view chunk_id,
                                          std::string *error_detail)
    {
        return ParseChunkId(chunk_id, nullptr, error_detail);
    }

    bool ChunkLocation::IsValid() const
    {
        return !node_id.empty() && !chunk_id.empty();
    }

    bool ChunkChecksum::IsSet() const
    {
        return algorithm != ChunkChecksumAlgorithm::kUnknown &&
               !value.empty();
    }

    bool ChunkIdentity::HasChunkKey() const
    {
        return !chunk_id.empty();
    }

    bool ChunkReplica::IsReadable() const
    {
        return !node_id.empty() &&
               !chunk_id.empty() &&
               size > 0 &&
               checksum.IsSet() &&
               IsReadableChunkState(state);
    }

    bool ChunkMetadata::IsReadable() const
    {
        return identity.HasChunkKey() &&
               size > 0 &&
               checksum.IsSet() &&
               IsReadableChunkState(state);
    }

    bool ChunkIndexEntry::HasFinalPath() const
    {
        return !final_path.empty();
    }

}
