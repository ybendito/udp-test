#pragma once

#include "common.hpp"
#include "uso_send.hpp"

#include <cstring>
#include <iostream>
#include <optional>
#include <string>

namespace boostudp {

struct ServerOptions {
    std::string bindAddress = "0.0.0.0";
    std::uint16_t port = 0;
    std::optional<std::size_t> batchCount;
    bool verbose = false;
    bool uro = false;
};

struct ClientOptions {
    std::string sourceAddress = "0.0.0.0";
    std::string destAddress;
    std::uint16_t destPort = 0;
    std::size_t batchSize = 0;
    std::size_t batchCount = 10;
    std::size_t mss = kDefaultMss;
};

namespace detail {

inline bool is_help_flag(const char* arg)
{
    return std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0;
}

inline std::string option_key(const char* arg)
{
    const char* eq = std::strchr(arg, '=');
    if (eq) {
        return std::string(arg, eq - arg);
    }
    return arg;
}

inline std::string option_value(const char* arg, int& index, int argc, char* argv[])
{
    const char* eq = std::strchr(arg, '=');
    if (eq) {
        return eq + 1;
    }
    if (index + 1 >= argc) {
        throw std::invalid_argument(
            std::string("option ") + arg + " requires a value");
    }
    return argv[++index];
}

} // namespace detail

inline void print_server_help(const char* program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "\n"
        << "USO/hardware test receiver. Reassembles UDP fragments until\n"
        << "header.length bytes, then verifies CRC (works with or without --uro).\n"
        << "\n"
        << "Options:\n"
        << "  --bind <address>   Local address to bind (default: 0.0.0.0)\n"
        << "  --port <port>      UDP port to listen on (required)\n"
        << "  --count <n>        Receive exactly n logical batches, then exit\n"
        << "  -v                 Verbose logging\n"
        << "  --uro              WSARecvMsg + UDP_RECV_MAX_COALESCED_SIZE\n"
        << "                       (socket-level coalesce; Win10 2004+)\n"
        << "  --help, -h         Show this help\n"
        << "\n"
        << "Default (no --uro): recvfrom per datagram; frags= segment count.\n"
        << "With --uro: one read may contain many segments (coalesced recv).\n"
        << "Sender --mss 0: one datagram per batch (frags=1); IP may fragment on wire.\n"
        << "Both paths use the same app reassembly. Compare frags vs batches.\n"
        << "Without --count: idle timeout after first traffic.\n";
}

inline void print_client_help(const char* program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "\n"
        << "USO/hardware test sender (Windows: WSASendMsg + USO unless --mss 0).\n"
        << "Trace guest vNIC for large send NBL; match --size and --mss.\n"
        << "\n"
        << "Options:\n"
        << "  --source <address>   Local bind address (default: 0.0.0.0)\n"
        << "  --dest <address>     Destination host (required)\n"
        << "  --port <port>        Destination UDP port (required)\n"
        << "  --size <bytes>       Total logical batch size (one header), "
        << kPacketHeaderSize << ".." << kMaxBatchSize << " (required)\n"
        << "  --count <n>          Number of batches to send (default: 10)\n"
        << "  --mss <bytes>        USO segment size (default: " << kDefaultMss
        << ", max " << kUsoMaxSegmentSize << "; 0 = plain send, no USO)\n"
        << "  --help, -h           Show this help\n"
        << "\n"
        << "One buffer per batch (header.length = --size). Plain sendto (no USO) when\n"
        << "--mss 0 or --size < --mss. Otherwise segmented at --mss; last segment\n"
        << "shorter when size % mss != 0.\n";
}

inline bool parse_server_options(int argc, char* argv[], ServerOptions& out)
{
    for (int i = 1; i < argc; ++i) {
        if (detail::is_help_flag(argv[i])) {
            print_server_help(argv[0]);
            return false;
        }
    }

    for (int i = 1; i < argc; ++i) {
        if (detail::is_help_flag(argv[i])) {
            continue;
        }
        if (argv[i][0] != '-') {
            throw std::invalid_argument(
                std::string("unexpected argument: ") + argv[i]);
        }

        const std::string key = detail::option_key(argv[i]);
        if (key == "--bind") {
            out.bindAddress = detail::option_value(argv[i], i, argc, argv);
        } else if (key == "--port") {
            out.port = parse_port(detail::option_value(argv[i], i, argc, argv));
        } else if (key == "--count") {
            out.batchCount = parse_count(detail::option_value(argv[i], i, argc, argv));
        } else if (key == "-v") {
            out.verbose = true;
        } else if (key == "--uro") {
            out.uro = true;
        } else {
            throw std::invalid_argument("unknown option: " + key);
        }
    }

    if (out.port == 0) {
        throw std::invalid_argument("--port is required");
    }
#ifndef _WIN32
    if (out.uro) {
        throw std::invalid_argument("--uro is only supported on Windows");
    }
#endif
    return true;
}

inline bool parse_client_options(int argc, char* argv[], ClientOptions& out)
{
    for (int i = 1; i < argc; ++i) {
        if (detail::is_help_flag(argv[i])) {
            print_client_help(argv[0]);
            return false;
        }
    }

    for (int i = 1; i < argc; ++i) {
        if (detail::is_help_flag(argv[i])) {
            continue;
        }
        if (argv[i][0] != '-') {
            throw std::invalid_argument(
                std::string("unexpected argument: ") + argv[i]);
        }

        const std::string key = detail::option_key(argv[i]);
        if (key == "--source") {
            out.sourceAddress = detail::option_value(argv[i], i, argc, argv);
        } else if (key == "--dest") {
            out.destAddress = detail::option_value(argv[i], i, argc, argv);
        } else if (key == "--port") {
            out.destPort = parse_port(detail::option_value(argv[i], i, argc, argv));
        } else if (key == "--size") {
            out.batchSize = parse_batch_size(detail::option_value(argv[i], i, argc, argv));
        } else if (key == "--count") {
            out.batchCount = parse_count(detail::option_value(argv[i], i, argc, argv));
        } else if (key == "--mss") {
            out.mss = parse_mss(detail::option_value(argv[i], i, argc, argv));
        } else if (key == "--uso") {
            throw std::invalid_argument("--uso is always enabled on Windows (removed)");
        } else {
            throw std::invalid_argument("unknown option: " + key);
        }
    }

    if (out.destAddress.empty()) {
        throw std::invalid_argument("--dest is required");
    }
    if (out.destPort == 0) {
        throw std::invalid_argument("--port is required");
    }
    if (out.batchSize == 0) {
        throw std::invalid_argument("--size is required");
    }
    if (out.mss != 0 && out.mss > kUsoMaxSegmentSize) {
        throw std::invalid_argument(
            "--mss must be 0 or <= " + std::to_string(kUsoMaxSegmentSize));
    }
    if (out.batchSize > kUsoMaxBytesPerBatch) {
        throw std::invalid_argument(
            "--size must be <= " + std::to_string(kUsoMaxBytesPerBatch));
    }
    return true;
}

} // namespace boostudp
