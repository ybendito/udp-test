// Windows 10 20H1+ UDP send segmentation (Sunshine send_batch parity).
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00

#include "uso_send.hpp"
#include "common.hpp"

#ifdef _WIN32

#include <algorithm>
#include <cstring>
#include <mswsock.h>
#include <stdexcept>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef UDP_SEND_MSG_SIZE
#define UDP_SEND_MSG_SIZE 2
#endif

namespace boostudp {
namespace {

using WSASendMsgFn = INT (WSAAPI*)(
    SOCKET,
    LPWSAMSG,
    DWORD,
    LPDWORD,
    LPWSAOVERLAPPED,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE);

WSASendMsgFn resolve_wsasendmsg(SOCKET probe_socket)
{
    static WSASendMsgFn fn = nullptr;
    static bool resolved = false;
    if (resolved) {
        return fn;
    }
    resolved = true;

    GUID guid = WSAID_WSASENDMSG;
    DWORD bytes = 0;
    if (WSAIoctl(
            probe_socket,
            SIO_GET_EXTENSION_FUNCTION_POINTER,
            &guid,
            sizeof(guid),
            &fn,
            sizeof(fn),
            &bytes,
            nullptr,
            nullptr) == SOCKET_ERROR) {
        return nullptr;
    }
    return fn;
}

bool endpoint_to_sockaddr(
    const boost::asio::ip::udp::endpoint& target,
    sockaddr_storage& storage,
    int& storage_len)
{
    if (target.address().is_v4()) {
        auto addr = target.address().to_v4();
        auto native = addr.to_bytes();
        auto* sa = reinterpret_cast<sockaddr_in*>(&storage);
        std::memset(sa, 0, sizeof(*sa));
        sa->sin_family = AF_INET;
        sa->sin_port = htons(target.port());
        std::memcpy(&sa->sin_addr, native.data(), native.size());
        storage_len = sizeof(sockaddr_in);
        return true;
    }

    if (target.address().is_v6()) {
        auto addr = target.address().to_v6();
        auto native = addr.to_bytes();
        auto* sa = reinterpret_cast<sockaddr_in6*>(&storage);
        std::memset(sa, 0, sizeof(*sa));
        sa->sin6_family = AF_INET6;
        sa->sin6_port = htons(target.port());
        sa->sin6_scope_id = addr.scope_id();
        std::memcpy(&sa->sin6_addr, native.data(), native.size());
        storage_len = sizeof(sockaddr_in6);
        return true;
    }

    return false;
}

sockaddr_in to_sockaddr_v4(const boost::asio::ip::address_v4& address)
{
    sockaddr_in sa {};
    sa.sin_family = AF_INET;
    sa.sin_port = 0;
    const auto bytes = address.to_bytes();
    std::memcpy(&sa.sin_addr, bytes.data(), bytes.size());
    return sa;
}

sockaddr_in6 to_sockaddr_v6(const boost::asio::ip::address_v6& address)
{
    sockaddr_in6 sa {};
    sa.sin6_family = AF_INET6;
    sa.sin6_port = 0;
    sa.sin6_scope_id = address.scope_id();
    const auto bytes = address.to_bytes();
    std::memcpy(&sa.sin6_addr, bytes.data(), bytes.size());
    return sa;
}

} // namespace

UsoSendResult send_buffer_uso(
    std::uintptr_t native_socket,
    const char* data,
    std::size_t total_size,
    std::size_t mss,
    const boost::asio::ip::udp::endpoint& target,
    const boost::asio::ip::address& source_address)
{
    UsoSendResult result;
    const SOCKET sock = static_cast<SOCKET>(native_socket);

    if (!data || total_size == 0) {
        result.error = "empty batch buffer";
        return result;
    }
    if (total_size > kUsoMaxBytesPerBatch) {
        result.error = "batch exceeds USO max bytes per send";
        return result;
    }

    // mss==0: one datagram, no USO (OS may IP-fragment if > path MTU).
    if (mss == 0) {
        sockaddr_storage dest_storage {};
        int dest_len = 0;
        if (!endpoint_to_sockaddr(target, dest_storage, dest_len)) {
            result.error = "unsupported target address family";
            return result;
        }

        const int sent = sendto(
            sock,
            data,
            static_cast<int>(total_size),
            0,
            reinterpret_cast<sockaddr*>(&dest_storage),
            dest_len);
        if (sent == SOCKET_ERROR) {
            result.error = "sendto failed: " + std::to_string(WSAGetLastError());
            return result;
        }
        if (static_cast<std::size_t>(sent) != total_size) {
            result.error = "short plain send";
            return result;
        }

        result.success = true;
        result.segments_sent = 1;
        result.total_bytes = total_size;
        return result;
    }

    if (mss > kUsoMaxSegmentSize) {
        result.error = "invalid mss";
        return result;
    }

    const std::size_t segments = (total_size + mss - 1) / mss;

    if (segments == 1) {
        if (!send_single_uso(native_socket, data, total_size)) {
            result.error = "WSASend failed for single-segment batch";
            return result;
        }
        result.success = true;
        result.segments_sent = 1;
        result.total_bytes = total_size;
        return result;
    }

    const WSASendMsgFn wsasendmsg = resolve_wsasendmsg(sock);
    if (!wsasendmsg) {
        result.error = "WSASendMsg not available (SIO_GET_EXTENSION_FUNCTION_POINTER)";
        return result;
    }

    sockaddr_storage dest_storage {};
    int dest_len = 0;
    if (!endpoint_to_sockaddr(target, dest_storage, dest_len)) {
        result.error = "unsupported target address family";
        return result;
    }

    WSABUF buf {};
    buf.buf = const_cast<char*>(data);
    buf.len = static_cast<ULONG>(total_size);

    char cmbuf[WSA_CMSG_SPACE(sizeof(DWORD))
        + std::max(WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)), WSA_CMSG_SPACE(sizeof(IN_PKTINFO)))] =
        {};
    ULONG cmbuflen = 0;

    WSAMSG msg {};
    msg.name = reinterpret_cast<sockaddr*>(&dest_storage);
    msg.namelen = dest_len;
    msg.lpBuffers = &buf;
    msg.dwBufferCount = 1;
    msg.dwFlags = 0;
    msg.Control.buf = cmbuf;
    msg.Control.len = sizeof(cmbuf);

    auto* cm = WSA_CMSG_FIRSTHDR(&msg);
    if (!cm) {
        result.error = "WSA_CMSG_FIRSTHDR failed";
        return result;
    }

    if (source_address.is_v6()) {
        IN6_PKTINFO pktInfo {};
        const auto sa = to_sockaddr_v6(source_address.to_v6());
        pktInfo.ipi6_addr = sa.sin6_addr;
        pktInfo.ipi6_ifindex = 0;

        cmbuflen += WSA_CMSG_SPACE(sizeof(pktInfo));
        cm->cmsg_level = IPPROTO_IPV6;
        cm->cmsg_type = IPV6_PKTINFO;
        cm->cmsg_len = WSA_CMSG_LEN(sizeof(pktInfo));
        std::memcpy(WSA_CMSG_DATA(cm), &pktInfo, sizeof(pktInfo));
    } else {
        IN_PKTINFO pktInfo {};
        const auto sa = to_sockaddr_v4(source_address.to_v4());
        pktInfo.ipi_addr = sa.sin_addr;
        pktInfo.ipi_ifindex = 0;

        cmbuflen += WSA_CMSG_SPACE(sizeof(pktInfo));
        cm->cmsg_level = IPPROTO_IP;
        cm->cmsg_type = IP_PKTINFO;
        cm->cmsg_len = WSA_CMSG_LEN(sizeof(pktInfo));
        std::memcpy(WSA_CMSG_DATA(cm), &pktInfo, sizeof(pktInfo));
    }

    cmbuflen += WSA_CMSG_SPACE(sizeof(DWORD));
    cm = WSA_CMSG_NXTHDR(&msg, cm);
    if (!cm) {
        result.error = "WSA_CMSG_NXTHDR failed";
        return result;
    }
    cm->cmsg_level = IPPROTO_UDP;
    cm->cmsg_type = UDP_SEND_MSG_SIZE;
    cm->cmsg_len = WSA_CMSG_LEN(sizeof(DWORD));
    *reinterpret_cast<DWORD*>(WSA_CMSG_DATA(cm)) = static_cast<DWORD>(mss);

    msg.Control.len = cmbuflen;

    DWORD bytes_sent = 0;
    if (wsasendmsg(sock, &msg, 0, &bytes_sent, nullptr, nullptr) == SOCKET_ERROR) {
        result.error = "WSASendMsg failed: " + std::to_string(WSAGetLastError());
        return result;
    }

    const DWORD expected = static_cast<DWORD>(total_size);
    if (bytes_sent != expected) {
        result.error = "short USO send";
        return result;
    }

    result.success = true;
    result.segments_sent = segments;
    result.total_bytes = total_size;
    return result;
}

std::uintptr_t create_uso_udp_socket(const std::string& bind_address)
{
    const SOCKET sock = WSASocketW(
        AF_INET,
        SOCK_DGRAM,
        IPPROTO_UDP,
        nullptr,
        0,
        0);

    if (sock == INVALID_SOCKET) {
        throw std::runtime_error(
            "WSASocket failed: " + std::to_string(WSAGetLastError()));
    }

    BOOL reuse = TRUE;
    if (setsockopt(
            sock,
            SOL_SOCKET,
            SO_REUSEADDR,
            reinterpret_cast<const char*>(&reuse),
            sizeof(reuse)) == SOCKET_ERROR) {
        closesocket(sock);
        throw std::runtime_error(
            "SO_REUSEADDR failed: " + std::to_string(WSAGetLastError()));
    }

    int sndbuf = 1024 * 1024;
    if (setsockopt(
            sock,
            SOL_SOCKET,
            SO_SNDBUF,
            reinterpret_cast<const char*>(&sndbuf),
            sizeof(sndbuf)) == SOCKET_ERROR) {
        closesocket(sock);
        throw std::runtime_error(
            "SO_SNDBUF failed: " + std::to_string(WSAGetLastError()));
    }

    const auto addr = boostudp::parse_address(bind_address);
    if (!addr.is_v4()) {
        closesocket(sock);
        throw std::runtime_error("USO socket requires IPv4 bind address");
    }

    const auto bytes = addr.to_v4().to_bytes();
    sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    std::memcpy(&local.sin_addr, bytes.data(), bytes.size());

    if (bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        const auto err = WSAGetLastError();
        closesocket(sock);
        throw std::runtime_error("bind failed: " + std::to_string(err));
    }

    return static_cast<std::uintptr_t>(sock);
}

void close_uso_udp_socket(std::uintptr_t native_socket)
{
    closesocket(static_cast<SOCKET>(native_socket));
}

bool send_single_uso(
    std::uintptr_t native_socket,
    const char* data,
    std::size_t size)
{
    const SOCKET sock = static_cast<SOCKET>(native_socket);
    WSABUF buf {};
    buf.buf = const_cast<char*>(data);
    buf.len = static_cast<ULONG>(size);

    DWORD bytes_sent = 0;
    if (WSASend(sock, &buf, 1, &bytes_sent, 0, nullptr, nullptr) == SOCKET_ERROR) {
        return false;
    }

    return bytes_sent == size;
}

boost::asio::ip::udp::endpoint uso_socket_local_endpoint(std::uintptr_t native_socket)
{
    const SOCKET sock = static_cast<SOCKET>(native_socket);
    sockaddr_in local {};
    int len = sizeof(local);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&local), &len) == SOCKET_ERROR) {
        throw std::runtime_error(
            "getsockname failed: " + std::to_string(WSAGetLastError()));
    }

    boost::asio::ip::address_v4::bytes_type bytes {};
    std::memcpy(bytes.data(), &local.sin_addr, bytes.size());
    return boost::asio::ip::udp::endpoint(
        boost::asio::ip::address_v4(bytes),
        ntohs(local.sin_port));
}

} // namespace boostudp

#else

namespace boostudp {

UsoSendResult send_buffer_uso(
    std::uintptr_t,
    const char*,
    std::size_t,
    std::size_t,
    const boost::asio::ip::udp::endpoint&,
    const boost::asio::ip::address&)
{
    return UsoSendResult { false, 0, 0, "USO not implemented on this platform" };
}

} // namespace boostudp

#endif
