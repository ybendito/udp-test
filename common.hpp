#pragma once

#include "packet.hpp"

#include <boost/asio.hpp>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace boostudp {

// Max logical batch size (single header + payload, USO-segmented on send).
// Matches kUsoMaxBytesPerBatch / kUroMaxCoalescedRecv (64 KiB per syscall).
constexpr std::size_t kMaxBatchSize = 64 * 1024;

constexpr std::size_t kDefaultMss = 1400;

inline boost::asio::ip::address parse_address(const std::string& text)
{
    if (text == "*" || text == "0.0.0.0" || text == "0") {
        return boost::asio::ip::address_v4::any();
    }
    if (text == "::" || text == "0:0:0:0:0:0:0:0") {
        return boost::asio::ip::address_v6::any();
    }
    return boost::asio::ip::make_address(text);
}

inline std::uint16_t parse_port(const std::string& text)
{
    const auto value = std::stoul(text);
    if (value > 65535) {
        throw std::out_of_range("port out of range");
    }
    return static_cast<std::uint16_t>(value);
}

inline std::size_t parse_batch_size(const std::string& text)
{
    const auto value = std::stoull(text);
    if (value < kPacketHeaderSize || value > kMaxBatchSize) {
        throw std::out_of_range(
            "batch size must be " + std::to_string(kPacketHeaderSize)
            + ".." + std::to_string(kMaxBatchSize));
    }
    return static_cast<std::size_t>(value);
}

inline std::size_t parse_mss(const std::string& text)
{
    const auto value = std::stoull(text);
    if (value > kMaxBatchSize) {
        throw std::out_of_range(
            "mss must be 0.." + std::to_string(kMaxBatchSize));
    }
    return static_cast<std::size_t>(value);
}

inline std::size_t parse_count(const std::string& text)
{
    const auto value = std::stoull(text);
    if (value == 0) {
        throw std::out_of_range("count must be >= 1");
    }
    return static_cast<std::size_t>(value);
}

} // namespace boostudp
