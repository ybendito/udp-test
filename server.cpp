#include "cli.hpp"

#include "packet.hpp"



#ifdef _WIN32

#include "uso_recv.hpp"

#endif



#include <algorithm>

#include <chrono>

#include <cstdint>

#include <cstring>

#include <iostream>

#include <optional>

#include <unordered_set>

#include <vector>



#ifdef _WIN32

#include <winsock2.h>

#else

#include <sys/socket.h>

#endif



namespace {



constexpr auto kIdleTimeout = std::chrono::seconds(3);



struct ServerStats {

    std::size_t batches = 0;

    std::size_t fragments = 0;

    std::size_t corrupted = 0;

    std::size_t lost = 0;

    std::size_t duplicates = 0;

    std::size_t out_of_order = 0;

    std::unordered_set<std::uint32_t> seen_seq;

    bool have_seq = false;

    std::uint32_t highest_seq = 0;

};



struct BatchReassembly {

    std::vector<char> buffer;

    std::size_t filled = 0;

    std::uint32_t expected = 0;

    bool active = false;

    std::vector<std::size_t> fragment_sizes;

};



void set_receive_timeout(

    boost::asio::ip::udp::socket& socket,

    std::chrono::milliseconds timeout)

{

#ifdef _WIN32

    const DWORD milliseconds = static_cast<DWORD>(timeout.count());

    ::setsockopt(

        socket.native_handle(),

        SOL_SOCKET,

        SO_RCVTIMEO,

        reinterpret_cast<const char*>(&milliseconds),

        sizeof(milliseconds));

#else

    struct timeval tv {};

    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);

    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);

    ::setsockopt(

        socket.native_handle(),

        SOL_SOCKET,

        SO_RCVTIMEO,

        &tv,

        sizeof(tv));

#endif

}



void clear_receive_timeout(boost::asio::ip::udp::socket& socket)

{

    set_receive_timeout(socket, std::chrono::milliseconds(0));

}



bool is_receive_timeout(const boost::system::error_code& ec)

{

    return ec == boost::asio::error::timed_out

        || ec == boost::asio::error::try_again

        || ec == boost::asio::error::would_block;

}



void reset_reassembly(BatchReassembly& reassembly)

{

    reassembly.buffer.clear();

    reassembly.filled = 0;

    reassembly.expected = 0;

    reassembly.active = false;

    reassembly.fragment_sizes.clear();

}



std::optional<std::size_t> infer_detected_mss(const std::vector<std::size_t>& sizes)

{

    if (sizes.size() < 2) {

        return std::nullopt;

    }



    const std::size_t first = sizes.front();

    bool all_full_match = true;

    for (std::size_t i = 1; i + 1 < sizes.size(); ++i) {

        if (sizes[i] != first) {

            all_full_match = false;

            break;

        }

    }

    if (all_full_match && sizes.back() <= first) {

        return first;

    }



    return first;

}



void track_sequence(ServerStats& stats, std::uint32_t seq)

{

    if (stats.seen_seq.count(seq) != 0) {

        ++stats.duplicates;

        return;

    }



    if (stats.have_seq && seq < stats.highest_seq) {

        ++stats.out_of_order;

    }



    stats.seen_seq.insert(seq);

    if (!stats.have_seq) {

        stats.have_seq = true;

        stats.highest_seq = seq;

    } else {

        stats.highest_seq = std::max(stats.highest_seq, seq);

    }

}



void finalize_lost(

    ServerStats& stats,

    const std::optional<std::size_t>& expected_batch_count)

{

    std::uint32_t upper = 0;

    if (expected_batch_count) {

        if (*expected_batch_count == 0) {

            return;

        }

        upper = static_cast<std::uint32_t>(*expected_batch_count - 1);

    } else if (stats.have_seq) {

        upper = stats.highest_seq;

    } else {

        return;

    }



    for (std::uint32_t seq = 0; seq <= upper; ++seq) {

        if (stats.seen_seq.count(seq) == 0) {

            ++stats.lost;

        }

    }

}



void print_summary(const ServerStats& stats)

{

    std::cout << "batches=" << stats.batches

              << " frags=" << stats.fragments

              << " bad=" << stats.corrupted

              << " lost=" << stats.lost

              << " dup=" << stats.duplicates

              << " ooo=" << stats.out_of_order << '\n';

}



bool complete_batch(

    const boost::asio::ip::udp::endpoint& remote,

    BatchReassembly& reassembly,

    ServerStats& stats,

    bool verbose)

{

    const auto validation = boostudp::validate_batch(

        reassembly.buffer.data(), reassembly.expected);

    if (validation != boostudp::BatchValidation::Ok) {

        ++stats.corrupted;

        if (verbose) {

            std::cout << remote << ' '

                      << boostudp::validation_reason(validation) << " batch\n";

        }

        return false;

    }



    const auto* header =

        reinterpret_cast<const boostudp::PacketHeader*>(reassembly.buffer.data());

    track_sequence(stats, header->seq);

    ++stats.batches;



    if (verbose) {

        std::cout << remote << " ok seq=" << header->seq << ' '

                  << reassembly.expected << 'b';

        if (reassembly.fragment_sizes.size() > 1) {

            const auto mss = infer_detected_mss(reassembly.fragment_sizes);

            if (mss) {

                std::cout << " mss=" << *mss;

            }

            std::cout << " segs=" << reassembly.fragment_sizes.size();

        }

        std::cout << '\n';

    }



    return true;

}



void ingest_fragment(

    const boost::asio::ip::udp::endpoint& remote,

    const char* data,

    std::size_t length,

    ServerStats& stats,

    bool verbose,

    BatchReassembly& reassembly)

{

    ++stats.fragments;



    std::size_t offset = 0;

    while (offset < length) {

        if (!reassembly.active) {

            const std::size_t append_len = length - offset;

            reassembly.buffer.insert(

                reassembly.buffer.end(),

                data + offset,

                data + offset + append_len);

            reassembly.filled = reassembly.buffer.size();

            offset = length;



            if (reassembly.filled < boostudp::kPacketHeaderSize) {

                return;

            }



            const std::uint32_t expected_length = boostudp::peek_batch_length(

                reassembly.buffer.data(), reassembly.filled);

            if (expected_length == 0) {

                ++stats.corrupted;

                if (verbose) {

                    std::cout << remote << " bad magic (fragment "

                              << length << "b)\n";

                }

                reset_reassembly(reassembly);

                return;

            }



            if (expected_length < boostudp::kPacketHeaderSize

                || expected_length > boostudp::kMaxBatchSize) {

                ++stats.corrupted;

                if (verbose) {

                    std::cout << remote << " bad length " << expected_length

                              << '\n';

                }

                reset_reassembly(reassembly);

                return;

            }



            reassembly.expected = expected_length;

            reassembly.buffer.resize(expected_length);

            reassembly.fragment_sizes.push_back(append_len);



            if (reassembly.filled >= expected_length) {

                complete_batch(remote, reassembly, stats, verbose);

                reset_reassembly(reassembly);

            } else {

                reassembly.active = true;

                if (verbose) {

                    std::cout << remote << " reassemble " << reassembly.filled

                              << '/' << expected_length << '\n';

                }

            }

            return;

        }



        const std::size_t space = reassembly.expected - reassembly.filled;

        const std::size_t take = std::min(length - offset, space);

        reassembly.fragment_sizes.push_back(take);

        std::memcpy(

            reassembly.buffer.data() + reassembly.filled,

            data + offset,

            take);

        reassembly.filled += take;

        offset += take;



        if (reassembly.filled >= reassembly.expected) {

            complete_batch(remote, reassembly, stats, verbose);

            reset_reassembly(reassembly);

        }

    }

}



bool receive_datagram(

    boost::asio::ip::udp::socket& socket,

    std::vector<char>& buffer,

    boost::asio::ip::udp::endpoint& remote,

    ServerStats& stats,

    bool verbose,

    BatchReassembly& reassembly)

{

    boost::system::error_code ec;

    const std::size_t length = socket.receive_from(

        boost::asio::buffer(buffer), remote, 0, ec);



    if (ec) {

        if (is_receive_timeout(ec)) {

            return false;

        }

        throw boost::system::system_error(ec);

    }



    ingest_fragment(remote, buffer.data(), length, stats, verbose, reassembly);

    return true;

}



#ifdef _WIN32

bool receive_uro_datagram(

    boost::asio::ip::udp::socket& socket,

    std::vector<char>& buffer,

    boost::asio::ip::udp::endpoint& remote,

    ServerStats& stats,

    bool verbose,

    BatchReassembly& reassembly)

{

    const auto result = boostudp::uro_recv_msg(

        static_cast<std::uintptr_t>(socket.native_handle()),

        buffer.data(),

        buffer.size());



    if (!result.success) {

        if (!result.error.empty() && result.bytes_received == 0) {

            return false;

        }

        if (!result.error.empty()) {

            throw std::runtime_error(result.error);

        }

        return false;

    }



    if (result.bytes_received <= 0) {

        return false;

    }



    if (result.have_remote) {

        remote = result.remote;

    }



    if (verbose && result.coalesced && result.segment_size != 0) {

        std::cout << remote << " coalesced recv "

                  << result.bytes_received << " bytes, seg="

                  << result.segment_size << '\n';

    }



    ingest_fragment(

        remote,

        buffer.data(),

        static_cast<std::size_t>(result.bytes_received),

        stats,

        verbose,

        reassembly);

    return true;

}

#endif



void run_receive_loop(

    boost::asio::ip::udp::socket& socket,

    const boostudp::ServerOptions& options,

    bool use_uro)

{

    if (options.verbose) {

        if (options.batchCount) {

            std::cout << "  expect " << *options.batchCount << " batch(es)\n";

        } else {

            std::cout << "  idle=" << kIdleTimeout.count() << "s after first traffic\n";

        }

#ifdef _WIN32

        if (use_uro) {

            std::cout << "  recv: WSARecvMsg (UDP_RECV_MAX_COALESCED_SIZE="

                      << boostudp::kUroMaxCoalescedRecv << ")\n";

        }

#endif

    }



    std::vector<char> buffer(boostudp::kMaxBatchSize);

    boost::asio::ip::udp::endpoint remote;

    ServerStats stats;

    BatchReassembly reassembly;



    auto receive_one = [&]() -> bool {

#ifdef _WIN32

        if (use_uro) {

            return receive_uro_datagram(

                socket, buffer, remote, stats, options.verbose, reassembly);

        }

#endif

        return receive_datagram(

            socket, buffer, remote, stats, options.verbose, reassembly);

    };



    if (options.batchCount) {

        while (stats.batches < *options.batchCount) {

            if (!receive_one()) {

                throw std::runtime_error("unexpected receive timeout");

            }

        }

        finalize_lost(stats, options.batchCount);

        print_summary(stats);

        return;

    }



    clear_receive_timeout(socket);

    if (receive_one()) {

        set_receive_timeout(socket, kIdleTimeout);

        while (receive_one()) {

        }

        clear_receive_timeout(socket);

    }



    finalize_lost(stats, std::nullopt);

    print_summary(stats);

}



void run_server(const boostudp::ServerOptions& options)

{

    boost::asio::io_context io;

    boost::asio::ip::udp::endpoint listenEndpoint(

        boostudp::parse_address(options.bindAddress), options.port);



    boost::asio::ip::udp::socket socket(io);

    socket.open(listenEndpoint.protocol());

    socket.set_option(boost::asio::socket_base::reuse_address(true));

    socket.bind(listenEndpoint);



#ifdef _WIN32

    const bool use_uro = options.uro;

    if (use_uro) {

        std::string cfg_error;

        if (!boostudp::uro_configure_recv_socket(

                static_cast<std::uintptr_t>(socket.native_handle()),

                boostudp::kUroMaxCoalescedRecv,

                cfg_error)) {

            throw std::runtime_error(cfg_error);

        }



        int rcvbuf = 1024 * 1024;

        ::setsockopt(

            socket.native_handle(),

            SOL_SOCKET,

            SO_RCVBUF,

            reinterpret_cast<const char*>(&rcvbuf),

            sizeof(rcvbuf));

    }

#else

    const bool use_uro = false;

#endif



    if (options.verbose) {

        std::cout << "listen " << listenEndpoint << '\n';

    }



    run_receive_loop(socket, options, use_uro);

}



} // namespace



int main(int argc, char* argv[])

{

    boostudp::ServerOptions options;

    try {

        if (!boostudp::parse_server_options(argc, argv, options)) {

            return 0;

        }

        run_server(options);

    } catch (const std::exception& ex) {

        std::cerr << "Error: " << ex.what() << '\n';

        return 1;

    }



    return 0;

}

