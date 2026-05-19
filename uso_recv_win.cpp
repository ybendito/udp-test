// Windows 10 20H1+ UDP receive coalescing (WSARecvMsg + UDP_COALESCED_INFO).
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00

#include "uso_recv.hpp"
#include "common.hpp"

#ifdef _WIN32

#include <cstring>
#include <mswsock.h>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef UDP_RECV_MAX_COALESCED_SIZE
#define UDP_RECV_MAX_COALESCED_SIZE 1
#endif

#ifndef UDP_COALESCED_INFO
#define UDP_COALESCED_INFO 3
#endif

namespace boostudp {
namespace {

using WSARecvMsgFn = INT (WSAAPI*)(
    SOCKET,
    LPWSAMSG,
    LPDWORD,
    LPWSAOVERLAPPED,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE);

using WSASetUdpRecvMaxCoalescedSizeFn = INT (WSAAPI*)(SOCKET, DWORD);
using WSAGetUdpRecvMaxCoalescedSizeFn = INT (WSAAPI*)(SOCKET, DWORD*);

WSARecvMsgFn resolve_wsarecvmsg(SOCKET probe_socket)
{
    static WSARecvMsgFn fn = nullptr;
    static bool resolved = false;
    if (resolved) {
        return fn;
    }
    resolved = true;

    GUID guid = WSAID_WSARECVMSG;
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

WSASetUdpRecvMaxCoalescedSizeFn resolve_set_udp_recv_max_coalesced_size()
{
    return reinterpret_cast<WSASetUdpRecvMaxCoalescedSizeFn>(GetProcAddress(
        GetModuleHandleW(L"ws2_32.dll"),
        "WSASetUdpRecvMaxCoalescedSize"));
}

WSAGetUdpRecvMaxCoalescedSizeFn resolve_get_udp_recv_max_coalesced_size()
{
    return reinterpret_cast<WSAGetUdpRecvMaxCoalescedSizeFn>(GetProcAddress(
        GetModuleHandleW(L"ws2_32.dll"),
        "WSAGetUdpRecvMaxCoalescedSize"));
}

bool get_udp_recv_max_coalesced_size(SOCKET sock, DWORD& out, std::string& error)
{
    if (auto get_fn = resolve_get_udp_recv_max_coalesced_size()) {
        if (get_fn(sock, &out) == 0) {
            return true;
        }
        error = "WSAGetUdpRecvMaxCoalescedSize failed: "
            + std::to_string(WSAGetLastError());
        return false;
    }

    int len = sizeof(out);
    if (getsockopt(
            sock,
            IPPROTO_UDP,
            UDP_RECV_MAX_COALESCED_SIZE,
            reinterpret_cast<char*>(&out),
            &len) == SOCKET_ERROR) {
        error = "getsockopt(UDP_RECV_MAX_COALESCED_SIZE) failed: "
            + std::to_string(WSAGetLastError());
        return false;
    }

    return true;
}

bool set_udp_recv_max_coalesced_size(SOCKET sock, DWORD max_size, std::string& error)
{
    if (auto set_fn = resolve_set_udp_recv_max_coalesced_size()) {
        if (set_fn(sock, max_size) != 0) {
            error = "WSASetUdpRecvMaxCoalescedSize failed: "
                + std::to_string(WSAGetLastError());
            return false;
        }
    } else if (setsockopt(
                   sock,
                   IPPROTO_UDP,
                   UDP_RECV_MAX_COALESCED_SIZE,
                   reinterpret_cast<const char*>(&max_size),
                   sizeof(max_size)) == SOCKET_ERROR) {
        error = "setsockopt(UDP_RECV_MAX_COALESCED_SIZE) failed: "
            + std::to_string(WSAGetLastError());
        return false;
    }

    DWORD actual = 0;
    if (!get_udp_recv_max_coalesced_size(sock, actual, error)) {
        return false;
    }
    if (actual != max_size) {
        error = "UDP_RECV_MAX_COALESCED_SIZE: got " + std::to_string(actual)
            + " expected " + std::to_string(max_size);
        return false;
    }

    return true;
}

bool sockaddr_to_endpoint(
    const sockaddr_storage& storage,
    int storage_len,
    boost::asio::ip::udp::endpoint& out)
{
    if (storage.ss_family == AF_INET && storage_len >= static_cast<int>(sizeof(sockaddr_in))) {
        const auto* sa = reinterpret_cast<const sockaddr_in*>(&storage);
        boost::asio::ip::address_v4::bytes_type bytes {};
        std::memcpy(bytes.data(), &sa->sin_addr, bytes.size());
        out = boost::asio::ip::udp::endpoint(
            boost::asio::ip::address_v4(bytes),
            ntohs(sa->sin_port));
        return true;
    }

    if (storage.ss_family == AF_INET6 && storage_len >= static_cast<int>(sizeof(sockaddr_in6))) {
        const auto* sa = reinterpret_cast<const sockaddr_in6*>(&storage);
        boost::asio::ip::address_v6::bytes_type bytes {};
        std::memcpy(bytes.data(), &sa->sin6_addr, bytes.size());
        out = boost::asio::ip::udp::endpoint(
            boost::asio::ip::address_v6(bytes, sa->sin6_scope_id),
            ntohs(sa->sin6_port));
        return true;
    }

    return false;
}

} // namespace

bool uro_configure_recv_socket(
    std::uintptr_t native_socket,
    std::size_t max_coalesced_size,
    std::string& error)
{
    const SOCKET sock = static_cast<SOCKET>(native_socket);
    if (max_coalesced_size == 0 || max_coalesced_size > kUroMaxCoalescedRecv) {
        error = "max coalesced recv size must be 1.."
            + std::to_string(kUroMaxCoalescedRecv);
        return false;
    }

    return set_udp_recv_max_coalesced_size(
        sock,
        static_cast<DWORD>(max_coalesced_size),
        error);
}

UroRecvResult uro_recv_msg(
    std::uintptr_t native_socket,
    void* buffer,
    std::size_t buffer_capacity)
{
    UroRecvResult result;
    const SOCKET sock = static_cast<SOCKET>(native_socket);

    if (buffer_capacity == 0 || buffer_capacity > kUroMaxCoalescedRecv) {
        result.error = "invalid receive buffer size";
        return result;
    }

    const WSARecvMsgFn wsarecvmsg = resolve_wsarecvmsg(sock);
    if (!wsarecvmsg) {
        result.error = "WSARecvMsg not available (SIO_GET_EXTENSION_FUNCTION_POINTER)";
        return result;
    }

    WSABUF buf {};
    buf.buf = static_cast<char*>(buffer);
    buf.len = static_cast<ULONG>(buffer_capacity);

    char cmbuf[WSA_CMSG_SPACE(sizeof(DWORD))] = {};
    sockaddr_storage remote {};
    int remote_len = sizeof(remote);
    WSAMSG msg {};
    msg.name = reinterpret_cast<sockaddr*>(&remote);
    msg.namelen = remote_len;
    msg.lpBuffers = &buf;
    msg.dwBufferCount = 1;
    msg.dwFlags = 0;
    msg.Control.buf = cmbuf;
    msg.Control.len = sizeof(cmbuf);

    DWORD bytes_received = 0;
    if (wsarecvmsg(sock, &msg, &bytes_received, nullptr, nullptr) == SOCKET_ERROR) {
        const auto err = WSAGetLastError();
        if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
            result.success = false;
            result.bytes_received = 0;
            return result;
        }
        result.error = "WSARecvMsg failed: " + std::to_string(err);
        return result;
    }

    result.bytes_received = static_cast<int>(bytes_received);
    result.success = true;
    result.have_remote =
        sockaddr_to_endpoint(remote, static_cast<int>(msg.namelen), result.remote);

    for (auto* cm = WSA_CMSG_FIRSTHDR(&msg); cm != nullptr;
         cm = WSA_CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level == IPPROTO_UDP && cm->cmsg_type == UDP_COALESCED_INFO
            && cm->cmsg_len >= WSA_CMSG_LEN(sizeof(DWORD))) {
            result.coalesced = true;
            result.segment_size = *reinterpret_cast<DWORD*>(WSA_CMSG_DATA(cm));
            break;
        }
    }

    return result;
}

} // namespace boostudp

#endif
