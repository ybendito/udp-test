#pragma once

#include <boost/asio/ip/udp.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace boostudp {

// Max bytes returned in one coalesced WSARecvMsg (matches send batch cap).
constexpr std::size_t kUroMaxCoalescedRecv = 64 * 1024;

struct UroRecvResult {
    bool success = false;
    int bytes_received = 0;
    bool coalesced = false;
    std::uint32_t segment_size = 0;
    bool have_remote = false;
    boost::asio::ip::udp::endpoint remote;
    std::string error;
};

#ifdef _WIN32
// Enables UDP_RECV_MAX_COALESCED_SIZE (WSASetUdpRecvMaxCoalescedSize).
bool uro_configure_recv_socket(
    std::uintptr_t native_socket,
    std::size_t max_coalesced_size,
    std::string& error);

// Blocking WSARecvMsg. Fills buffer; sets coalesced + segment_size from UDP_COALESCED_INFO.
UroRecvResult uro_recv_msg(
    std::uintptr_t native_socket,
    void* buffer,
    std::size_t buffer_capacity);
#endif

} // namespace boostudp
