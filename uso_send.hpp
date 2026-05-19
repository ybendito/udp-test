#pragma once

#include <boost/asio/ip/udp.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace boostudp {

constexpr std::size_t kUsoMaxSegmentsPerBatch = 64;
constexpr std::size_t kUsoMaxBytesPerBatch = 64 * 1024;
// Per-datagram UDP payload must stay under path MTU (Apollo/Sunshine use ~1408).
constexpr std::size_t kUsoMaxSegmentSize = 1472;

struct UsoSendResult {
    bool success = false;
    std::size_t segments_sent = 0;
    std::size_t total_bytes = 0;
    std::string error;
};

#ifdef _WIN32
std::uintptr_t create_uso_udp_socket(const std::string& bind_address);
void close_uso_udp_socket(std::uintptr_t native_socket);
boost::asio::ip::udp::endpoint uso_socket_local_endpoint(std::uintptr_t native_socket);

bool send_single_uso(
    std::uintptr_t native_socket,
    const char* data,
    std::size_t size);
#endif

// One logical batch buffer. mss==0: single sendto (no USO). Else USO at mss.
UsoSendResult send_buffer_uso(
    std::uintptr_t native_socket,
    const char* data,
    std::size_t total_size,
    std::size_t mss,
    const boost::asio::ip::udp::endpoint& target,
    const boost::asio::ip::address& source_address);

} // namespace boostudp
