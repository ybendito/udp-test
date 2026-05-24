#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <stdexcept>
#include <vector>

namespace boostudp {

constexpr std::uint32_t kPacketMagic = 0x42554450; // "BDUP"
constexpr std::size_t kMaxPacketLength = 64 * 1024;

#pragma pack(push, 1)
struct PacketHeader {
    std::uint32_t magic;
    std::uint32_t seq;         // 0 .. batch_total-1
    std::uint32_t batch_total; // sender --count (same on every batch in a run)
    std::uint32_t length;      // total batch size in bytes (header + payload)
    std::uint32_t crc32;
};
#pragma pack(pop)

constexpr std::size_t kPacketHeaderSize = sizeof(PacketHeader);

inline std::uint32_t crc32_update(std::uint32_t crc, std::uint8_t byte)
{
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
        const std::uint32_t mask = -(crc & 1u);
        crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
    return crc;
}

inline std::uint32_t crc32_compute(const void* data, std::size_t length)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < length; ++i) {
        crc = crc32_update(crc, bytes[i]);
    }
    return crc ^ 0xFFFFFFFFu;
}

inline std::uint32_t batch_crc32(const void* batch, std::size_t total_size)
{
    std::vector<std::uint8_t> copy(
        static_cast<const std::uint8_t*>(batch),
        static_cast<const std::uint8_t*>(batch) + total_size);
    auto* header = reinterpret_cast<PacketHeader*>(copy.data());
    header->crc32 = 0;
    return crc32_compute(copy.data(), total_size);
}

inline bool header_meta_plausible(
    std::uint32_t seq,
    std::uint32_t batch_total,
    std::uint32_t length)
{
    if (batch_total == 0 || seq >= batch_total) {
        return false;
    }
    if (length < kPacketHeaderSize || length > kMaxPacketLength) {
        return false;
    }
    return true;
}

inline void build_batch(
    std::vector<char>& batch,
    std::uint32_t seq,
    std::uint32_t batch_total,
    std::mt19937& rng)
{
    if (batch.size() < kPacketHeaderSize) {
        throw std::invalid_argument("batch size smaller than header");
    }
    if (batch_total == 0 || seq >= batch_total) {
        throw std::invalid_argument("invalid seq/batch_total");
    }

    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (std::size_t i = kPacketHeaderSize; i < batch.size(); ++i) {
        batch[i] = static_cast<char>(byte_dist(rng));
    }

    auto* header = reinterpret_cast<PacketHeader*>(batch.data());
    header->magic = kPacketMagic;
    header->seq = seq;
    header->batch_total = batch_total;
    header->length = static_cast<std::uint32_t>(batch.size());
    header->crc32 = 0;
    header->crc32 = batch_crc32(batch.data(), batch.size());
}

enum class BatchValidation {
    Ok,
    TooShort,
    BadMagic,
    BadLength,
    BadMeta,
    BadSession,
    BadCrc,
};

inline BatchValidation validate_batch(const void* data, std::size_t received_length)
{
    if (received_length < kPacketHeaderSize) {
        return BatchValidation::TooShort;
    }

    PacketHeader header {};
    std::memcpy(&header, data, kPacketHeaderSize);

    if (header.magic != kPacketMagic) {
        return BatchValidation::BadMagic;
    }

    if (header.length != received_length) {
        return BatchValidation::BadLength;
    }

    if (!header_meta_plausible(header.seq, header.batch_total, header.length)) {
        return BatchValidation::BadMeta;
    }

    const std::uint32_t expected_crc = batch_crc32(data, received_length);
    if (header.crc32 != expected_crc) {
        return BatchValidation::BadCrc;
    }

    return BatchValidation::Ok;
}

inline BatchValidation validate_batch_session(
    const PacketHeader& header,
    std::optional<std::uint32_t>& session_batch_total)
{
    if (!header_meta_plausible(header.seq, header.batch_total, header.length)) {
        return BatchValidation::BadMeta;
    }

    if (!session_batch_total) {
        session_batch_total = header.batch_total;
    } else if (*session_batch_total != header.batch_total) {
        return BatchValidation::BadSession;
    }

    return BatchValidation::Ok;
}

inline const char* validation_reason(BatchValidation result)
{
    switch (result) {
    case BatchValidation::Ok: return "ok";
    case BatchValidation::TooShort: return "too short";
    case BatchValidation::BadMagic: return "bad magic";
    case BatchValidation::BadLength: return "bad length";
    case BatchValidation::BadMeta: return "bad meta";
    case BatchValidation::BadSession: return "bad session";
    case BatchValidation::BadCrc: return "bad crc";
    }
    return "unknown";
}

// Returns 0 if no plausible header at data[0..available).
inline std::uint32_t peek_batch_length(const void* data, std::size_t available)
{
    if (available < kPacketHeaderSize) {
        return 0;
    }

    PacketHeader header {};
    std::memcpy(&header, data, kPacketHeaderSize);
    if (header.magic != kPacketMagic) {
        return 0;
    }
    if (!header_meta_plausible(header.seq, header.batch_total, header.length)) {
        return 0;
    }

    return header.length;
}

} // namespace boostudp
