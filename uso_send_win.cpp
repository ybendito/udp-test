// Windows 10 20H1+ UDP send segmentation (Sunshine send_batch parity).
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00

#include "uso_send.hpp"
#include "common.hpp"

#ifdef _WIN32

#include <windows.h>

#include <iphlpapi.h>

#include <algorithm>
#include <cstring>
#include <iostream>
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

void ensure_winsock_initialized()
{
    static bool initialized = false;
    static bool ok = false;
    if (!initialized) {
        WSADATA wsa {};
        ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        initialized = true;
    }
    if (!ok) {
        throw std::runtime_error("WSAStartup failed");
    }
}

using WSASendMsgFn = INT (WSAAPI*)(
    SOCKET,
    LPWSAMSG,
    DWORD,
    LPDWORD,
    LPWSAOVERLAPPED,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE);

using WSASetUdpSendMessageSizeFn = INT (WSAAPI*)(SOCKET, DWORD);

WSASetUdpSendMessageSizeFn resolve_set_udp_send_message_size()
{
    static WSASetUdpSendMessageSizeFn fn = nullptr;
    static bool resolved = false;
    if (resolved) {
        return fn;
    }
    resolved = true;

    HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    if (!ws2) {
        ws2 = LoadLibraryW(L"ws2_32.dll");
    }
    if (!ws2) {
        return nullptr;
    }

    fn = reinterpret_cast<WSASetUdpSendMessageSizeFn>(
        GetProcAddress(ws2, "WSASetUdpSendMessageSize"));
    return fn;
}

bool configure_udp_send_segment_size(SOCKET sock, DWORD mss, int& wsa_error)
{
    wsa_error = 0;

    if (const auto set_size = resolve_set_udp_send_message_size()) {
        if (set_size(sock, mss) == SOCKET_ERROR) {
            wsa_error = WSAGetLastError();
            return false;
        }
        return true;
    }

    if (setsockopt(
            sock,
            IPPROTO_UDP,
            UDP_SEND_MSG_SIZE,
            reinterpret_cast<const char*>(&mss),
            sizeof(mss)) == SOCKET_ERROR) {
        wsa_error = WSAGetLastError();
        return false;
    }
    return true;
}

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

bool bind_option_is_wildcard(const std::string& text)
{
    return text == "0.0.0.0" || text == "*" || text == "0";
}

bool ipv4_on_interface_index(ULONG if_index, boost::asio::ip::address_v4& out_address)
{
    ULONG buffer_size = 15 * 1024;
    std::vector<std::uint8_t> buffer(buffer_size);
    auto* addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    ULONG result = GetAdaptersAddresses(
        AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr,
        addresses,
        &buffer_size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        result = GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr,
            addresses,
            &buffer_size);
    }
    if (result != NO_ERROR) {
        return false;
    }

    for (auto* adapter = addresses; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        if (adapter->IfIndex != if_index && adapter->Ipv6IfIndex != if_index) {
            continue;
        }

        for (auto* unicast = adapter->FirstUnicastAddress; unicast;
             unicast = unicast->Next) {
            if (!unicast->Address.lpSockaddr
                || unicast->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            const auto* sa =
                reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            boost::asio::ip::address_v4::bytes_type bytes {};
            std::memcpy(bytes.data(), &sa->sin_addr, bytes.size());
            const auto candidate = boost::asio::ip::address_v4(bytes);
            if (!candidate.is_unspecified() && !candidate.is_loopback()) {
                out_address = candidate;
                return true;
            }
        }
    }

    return false;
}

bool ipv4_egress_for_destination(
    const boost::asio::ip::udp::endpoint& destination,
    boost::asio::ip::address_v4& out_address,
    ULONG& out_if_index)
{
    if (!destination.address().is_v4()) {
        return false;
    }

    const auto dest_v4 = destination.address().to_v4();
    const auto bytes = dest_v4.to_bytes();
    sockaddr_in dest_sa {};
    dest_sa.sin_family = AF_INET;
    dest_sa.sin_port = htons(destination.port());
    std::memcpy(&dest_sa.sin_addr, bytes.data(), bytes.size());

    NET_IFINDEX if_index = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&dest_sa), &if_index) != NO_ERROR) {
        return false;
    }

    if (!ipv4_on_interface_index(if_index, out_address)) {
        return false;
    }

    out_if_index = if_index;
    return true;
}

bool ipv4_interface_index(const boost::asio::ip::address_v4& address, ULONG& if_index)
{
    if (address.is_unspecified()) {
        return false;
    }

    const auto bytes = address.to_bytes();
    sockaddr_in sa {};
    sa.sin_family = AF_INET;
    sa.sin_port = 0;
    std::memcpy(&sa.sin_addr, bytes.data(), bytes.size());

    NET_IFINDEX index = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&sa), &index) != NO_ERROR) {
        return false;
    }

    if_index = index;
    return true;
}

bool source_address_is_unspecified(const boost::asio::ip::address& address)
{
    if (address.is_unspecified()) {
        return true;
    }
    if (address.is_v4() && address.to_v4().is_unspecified()) {
        return true;
    }
    if (address.is_v6() && address.to_v6().is_unspecified()) {
        return true;
    }
    return false;
}

struct WsaSendMsgIoContext {
    WSAOVERLAPPED overlapped {};
    volatile LONG completed = 0;
    DWORD io_error = 0;
    DWORD bytes_transferred = 0;
};

void CALLBACK wsasendmsg_completion_routine(
    DWORD error,
    DWORD cb_transferred,
    LPWSAOVERLAPPED overlapped,
    DWORD)
{
    auto* ctx = CONTAINING_RECORD(overlapped, WsaSendMsgIoContext, overlapped);
    ctx->io_error = error;
    ctx->bytes_transferred = cb_transferred;
    InterlockedExchange(&ctx->completed, 1);
}

bool invoke_wsasendmsg(
    WSASendMsgFn wsasendmsg,
    SOCKET sock,
    LPWSAMSG msg,
    bool use_completion_routine,
    DWORD& bytes_sent,
    int& wsa_error)
{
    wsa_error = 0;
    bytes_sent = 0;

    if (!use_completion_routine) {
        if (wsasendmsg(sock, msg, 0, &bytes_sent, nullptr, nullptr) == SOCKET_ERROR) {
            wsa_error = WSAGetLastError();
            return false;
        }
        return true;
    }

    WsaSendMsgIoContext ctx {};
    DWORD sync_bytes = 0;
    const int rc = wsasendmsg(
        sock,
        msg,
        0,
        &sync_bytes,
        &ctx.overlapped,
        wsasendmsg_completion_routine);
    if (rc == 0) {
        // Synchronous completion: bytes in sync_bytes; routine may also run.
        if (InterlockedCompareExchange(&ctx.completed, 0, 0) == 1) {
            bytes_sent = ctx.bytes_transferred;
        } else {
            bytes_sent = sync_bytes;
        }
        return true;
    }

    wsa_error = WSAGetLastError();
    if (wsa_error != WSA_IO_PENDING) {
        return false;
    }

    // Completion routine runs when this thread is in an alertable wait (MSDN).
    while (InterlockedCompareExchange(&ctx.completed, 0, 0) != 1) {
        SleepEx(16, TRUE);
    }

    if (ctx.io_error != 0) {
        wsa_error = static_cast<int>(ctx.io_error);
        return false;
    }

    bytes_sent = ctx.bytes_transferred;
    return true;
}

bool send_plain_datagram(
    SOCKET sock,
    const char* data,
    std::size_t total_size,
    const boost::asio::ip::udp::endpoint& target,
    std::string& error)
{
    sockaddr_storage dest_storage {};
    int dest_len = 0;
    if (!endpoint_to_sockaddr(target, dest_storage, dest_len)) {
        error = "unsupported target address family";
        return false;
    }

    const int sent = sendto(
        sock,
        data,
        static_cast<int>(total_size),
        0,
        reinterpret_cast<sockaddr*>(&dest_storage),
        dest_len);
    if (sent == SOCKET_ERROR) {
        error = "sendto failed: " + std::to_string(WSAGetLastError());
        return false;
    }
    if (static_cast<std::size_t>(sent) != total_size) {
        error = "short plain send";
        return false;
    }
    return true;
}

bool try_wsasendmsg_uso(
    SOCKET sock,
    WSASendMsgFn wsasendmsg,
    const char* data,
    std::size_t total_size,
    std::size_t mss,
    const boost::asio::ip::udp::endpoint& target,
    const boost::asio::ip::address& source_address,
    bool include_pktinfo,
    ULONG interface_index,
    bool use_completion_routine,
    int& wsa_error)
{
    wsa_error = 0;

    sockaddr_storage dest_storage {};
    int dest_len = 0;
    if (!endpoint_to_sockaddr(target, dest_storage, dest_len)) {
        wsa_error = WSAEINVAL;
        return false;
    }

    WSABUF buf {};
    buf.buf = const_cast<char*>(data);
    buf.len = static_cast<ULONG>(total_size);

    char cmbuf[WSA_CMSG_SPACE(sizeof(DWORD))
        + std::max(WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)), WSA_CMSG_SPACE(sizeof(IN_PKTINFO)))] =
        {};
    ULONG cmbuflen = 0;

    WSAMSG msg {};
    std::memset(&msg, 0, sizeof(msg));
    msg.name = reinterpret_cast<sockaddr*>(&dest_storage);
    msg.namelen = dest_len;
    msg.lpBuffers = &buf;
    msg.dwBufferCount = 1;
    msg.Control.buf = cmbuf;
    msg.Control.len = sizeof(cmbuf);

    auto* cm = WSA_CMSG_FIRSTHDR(&msg);
    if (!cm) {
        wsa_error = WSAEINVAL;
        return false;
    }

    if (include_pktinfo && !source_address_is_unspecified(source_address)) {
        if (source_address.is_v6()) {
            IN6_PKTINFO pktInfo {};
            const auto sa = to_sockaddr_v6(source_address.to_v6());
            pktInfo.ipi6_addr = sa.sin6_addr;
            pktInfo.ipi6_ifindex = 0;

            cm->cmsg_level = IPPROTO_IPV6;
            cm->cmsg_type = IPV6_PKTINFO;
            cm->cmsg_len = WSA_CMSG_LEN(sizeof(pktInfo));
            std::memcpy(WSA_CMSG_DATA(cm), &pktInfo, sizeof(pktInfo));
            cmbuflen += WSA_CMSG_SPACE(sizeof(pktInfo));
            cm = WSA_CMSG_NXTHDR(&msg, cm);
            if (!cm) {
                wsa_error = WSAEINVAL;
                return false;
            }
        } else {
            IN_PKTINFO pktInfo {};
            const auto v4 = source_address.to_v4();
            const auto sa = to_sockaddr_v4(v4);
            pktInfo.ipi_addr = sa.sin_addr;
            if (interface_index != 0) {
                pktInfo.ipi_ifindex = interface_index;
            } else {
                ULONG if_index = 0;
                if (ipv4_interface_index(v4, if_index)) {
                    pktInfo.ipi_ifindex = if_index;
                }
            }

            cm->cmsg_level = IPPROTO_IP;
            cm->cmsg_type = IP_PKTINFO;
            cm->cmsg_len = WSA_CMSG_LEN(sizeof(pktInfo));
            std::memcpy(WSA_CMSG_DATA(cm), &pktInfo, sizeof(pktInfo));
            cmbuflen += WSA_CMSG_SPACE(sizeof(pktInfo));
            cm = WSA_CMSG_NXTHDR(&msg, cm);
            if (!cm) {
                wsa_error = WSAEINVAL;
                return false;
            }
        }
    }

    // Sunshine only passes UDP_SEND_MSG_SIZE for multi-segment sends.
    const std::size_t segments = (total_size + mss - 1) / mss;
    if (segments > 1) {
        int cfg_error = 0;
        if (!configure_udp_send_segment_size(sock, static_cast<DWORD>(mss), cfg_error)) {
            wsa_error = cfg_error;
            return false;
        }

        cm->cmsg_level = IPPROTO_UDP;
        cm->cmsg_type = UDP_SEND_MSG_SIZE;
        cm->cmsg_len = WSA_CMSG_LEN(sizeof(DWORD));
        *reinterpret_cast<DWORD*>(WSA_CMSG_DATA(cm)) = static_cast<DWORD>(mss);
        cmbuflen += WSA_CMSG_SPACE(sizeof(DWORD));
    } else if (cmbuflen == 0) {
        msg.Control.buf = nullptr;
        msg.Control.len = 0;
    }

    msg.Control.len = cmbuflen;

    DWORD bytes_sent = 0;
    if (!invoke_wsasendmsg(
            wsasendmsg, sock, &msg, use_completion_routine, bytes_sent, wsa_error)) {
        return false;
    }

    return bytes_sent == static_cast<DWORD>(total_size);
}

} // namespace

UsoSendResult send_buffer_uso(
    std::uintptr_t native_socket,
    const char* data,
    std::size_t total_size,
    std::size_t mss,
    const boost::asio::ip::udp::endpoint& target,
    const boost::asio::ip::address& source_address,
    unsigned long interface_index,
    bool no_pktinfo,
    bool use_completion_routine)
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

    // Plain send: --mss 0, or batch smaller than segment size (USO cannot apply).
    if (mss == 0 || total_size < mss) {
        if (!send_plain_datagram(sock, data, total_size, target, result.error)) {
            return result;
        }
        result.success = true;
        result.segments_sent = 1;
        result.total_bytes = total_size;
        result.path = UsoSendPath::Plain;
        return result;
    }

    if (mss > kUsoMaxSegmentSize) {
        result.error = "invalid mss";
        return result;
    }

    const std::size_t segments = (total_size + mss - 1) / mss;

    // Exactly one segment: plain send (USO cmsg not used — see Sunshine send_batch).
    if (segments == 1) {
        if (!send_plain_datagram(sock, data, total_size, target, result.error)) {
            return result;
        }
        result.success = true;
        result.segments_sent = 1;
        result.total_bytes = total_size;
        result.path = UsoSendPath::Plain;
        return result;
    }

    const WSASendMsgFn wsasendmsg = resolve_wsasendmsg(sock);
    if (!wsasendmsg) {
        result.error = "WSASendMsg not available (SIO_GET_EXTENSION_FUNCTION_POINTER)";
        return result;
    }

    const bool specific_bind = !source_address_is_unspecified(source_address);
    const ULONG if_index = static_cast<ULONG>(interface_index);

    int wsa_error = 0;

    if (no_pktinfo) {
        if (try_wsasendmsg_uso(
                sock,
                wsasendmsg,
                data,
                total_size,
                mss,
                target,
                source_address,
                false,
                if_index,
                use_completion_routine,
                wsa_error)) {
            result.success = true;
            result.segments_sent = segments;
            result.total_bytes = total_size;
            result.path = UsoSendPath::BindOnly;
            return result;
        }
    } else {
        // Multi-subnet: pass IP_PKTINFO with egress ifindex (from route to --dest).
        if (if_index != 0 && specific_bind
            && try_wsasendmsg_uso(
                sock,
                wsasendmsg,
                data,
                total_size,
                mss,
                target,
                source_address,
                true,
                if_index,
                use_completion_routine,
                wsa_error)) {
            result.success = true;
            result.segments_sent = segments;
            result.total_bytes = total_size;
            result.path = UsoSendPath::PktinfoIfindex;
            return result;
        }

        if (try_wsasendmsg_uso(
                sock,
                wsasendmsg,
                data,
                total_size,
                mss,
                target,
                source_address,
                false,
                if_index,
                use_completion_routine,
                wsa_error)) {
            result.success = true;
            result.segments_sent = segments;
            result.total_bytes = total_size;
            result.path = UsoSendPath::BindOnly;
            return result;
        }

        if (specific_bind
            && try_wsasendmsg_uso(
                sock,
                wsasendmsg,
                data,
                total_size,
                mss,
                target,
                source_address,
                true,
                if_index,
                use_completion_routine,
                wsa_error)) {
            result.success = true;
            result.segments_sent = segments;
            result.total_bytes = total_size;
            result.path = UsoSendPath::PktinfoIfindex;
            return result;
        }
    }

    result.error = "WSASendMsg failed: " + std::to_string(wsa_error);
    return result;
}

std::uintptr_t create_uso_udp_socket(const std::string& bind_address)
{
    ensure_winsock_initialized();

    const SOCKET sock = WSASocketW(
        AF_INET,
        SOCK_DGRAM,
        IPPROTO_UDP,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED);

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

void log_windows_version(std::ostream& out)
{
    using RtlGetVersionFn = LONG (WINAPI*)(PRTL_OSVERSIONINFOW);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return;
    }

    const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    if (!rtl_get_version) {
        return;
    }

    RTL_OSVERSIONINFOW version {};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtl_get_version(&version) != 0) {
        return;
    }

    out << "windows build " << version.dwBuildNumber << " ("
        << version.dwMajorVersion << '.' << version.dwMinorVersion << ")\n";
}

UsoBindInfo resolve_uso_bind_address(
    const std::string& source_option,
    const boost::asio::ip::udp::endpoint& destination)
{
    UsoBindInfo info;
    info.bind_address = source_option;

    if (!bind_option_is_wildcard(source_option)) {
        const auto addr = boostudp::parse_address(source_option);
        if (addr.is_v4()) {
            ipv4_interface_index(addr.to_v4(), info.interface_index);
        }
        if (info.interface_index == 0 && destination.address().is_v4()) {
            boost::asio::ip::address_v4 ignored {};
            ipv4_egress_for_destination(destination, ignored, info.interface_index);
        }
        return info;
    }

    boost::asio::ip::address_v4 egress {};
    ULONG if_index = 0;
    if (!ipv4_egress_for_destination(destination, egress, if_index)) {
        return info;
    }

    info.bind_address = egress.to_string();
    info.interface_index = if_index;
    info.auto_selected = true;
    return info;
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
    const boost::asio::ip::address&,
    unsigned long,
    bool,
    bool)
{
    return UsoSendResult { false, 0, 0, UsoSendPath::None, "USO not implemented on this platform" };
}

} // namespace boostudp

#endif
