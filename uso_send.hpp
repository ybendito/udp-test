#pragma once

#include <boost/asio/ip/udp.hpp>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace boostudp {

constexpr std::size_t kUsoMaxBytesPerBatch = 64 * 1024;
// Per-datagram UDP payload must stay under path MTU (Apollo/Sunshine use ~1408).
constexpr std::size_t kUsoMaxSegmentSize = 1472;

enum class UsoSendPath {
    None,
    Plain,
    BindOnly,
    PktinfoIfindex,
};

inline const char* uso_send_path_name(UsoSendPath path)
{
    switch (path) {
    case UsoSendPath::Plain:
        return "plain";
    case UsoSendPath::BindOnly:
        return "bind-only";
    case UsoSendPath::PktinfoIfindex:
        return "pktinfo+ifindex";
    default:
        return "none";
    }
}

struct UsoSendResult {
    bool success = false;
    std::size_t segments_sent = 0;
    std::size_t total_bytes = 0;
    UsoSendPath path = UsoSendPath::None;
    std::string error;
};

#ifdef _WIN32
struct UsoBindInfo {
    std::string bind_address;
    unsigned long interface_index = 0;
    bool auto_selected = false; // picked egress NIC toward --dest (multi-homed)
};

// When --source is 0.0.0.0, bind to the IPv4 on the interface that routes to dest.
UsoBindInfo resolve_uso_bind_address(
    const std::string& source_option,
    const boost::asio::ip::udp::endpoint& destination);

void log_windows_version(std::ostream& out);

std::uintptr_t create_uso_udp_socket(const std::string& bind_address);
void close_uso_udp_socket(std::uintptr_t native_socket);
boost::asio::ip::udp::endpoint uso_socket_local_endpoint(std::uintptr_t native_socket);
#endif

// One logical batch buffer. mss==0: single sendto (no USO). Else USO at mss.
UsoSendResult send_buffer_uso(
    std::uintptr_t native_socket,
    const char* data,
    std::size_t total_size,
    std::size_t mss,
    const boost::asio::ip::udp::endpoint& target,
    const boost::asio::ip::address& source_address,
    unsigned long interface_index = 0,
    bool no_pktinfo = false,
    bool completion_routine = false);

} // namespace boostudp
