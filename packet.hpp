#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>
#include <vector>

namespace boostudp {

constexpr std::uint32_t kPacketMagic = 0x42554450; // "BDUP"

#pragma pack(push, 1)
struct PacketHeader {
    std::uint32_t magic;
    std::uint32_t seq;
    std::uint32_t length; // total batch size in bytes (header + payload)
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

inline void build_batch(
    std::vector<char>& batch,
    std::uint32_t seq,
    std::mt19937& rng)
{
    if (batch.size() < kPacketHeaderSize) {
        throw std::invalid_argument("batch size smaller than header");
    }

    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (std::size_t i = kPacketHeaderSize; i < batch.size(); ++i) {
        batch[i] = static_cast<char>(byte_dist(rng));
    }

    auto* header = reinterpret_cast<PacketHeader*>(batch.data());
    header->magic = kPacketMagic;
    header->seq = seq;
    header->length = static_cast<std::uint32_t>(batch.size());
    header->crc32 = 0;
    header->crc32 = batch_crc32(batch.data(), batch.size());
}

enum class BatchValidation {
    Ok,
    TooShort,
    BadMagic,
    BadLength,
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

    if (header.length < kPacketHeaderSize) {
        return BatchValidation::BadLength;
    }

    const std::uint32_t expected_crc = batch_crc32(data, received_length);
    if (header.crc32 != expected_crc) {
        return BatchValidation::BadCrc;
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
    case BatchValidation::BadCrc: return "bad crc";
    }
    return "unknown";
}

// Returns 0 until a full header is available.
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

    return header.length;
}

} // namespace boostudp
