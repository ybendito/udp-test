#include "cli.hpp"

#include "packet.hpp"

#include "uso_send.hpp"



#include <chrono>

#include <iostream>

#include <random>

#include <stdexcept>

#include <vector>



namespace {



#ifdef _WIN32

void send_batch_windows(

    std::uintptr_t native_socket,

    const boost::asio::ip::udp::endpoint& remote,

    const boost::asio::ip::address& source_address,

    const char* data,

    std::size_t batch_size,

    std::size_t mss)

{

    const auto result = boostudp::send_buffer_uso(

        native_socket, data, batch_size, mss, remote, source_address);



    if (!result.success) {

        throw std::runtime_error("send failed: " + result.error);

    }



    if (mss == 0) {

        std::cout << "  plain " << result.total_bytes << " bytes (one datagram, no USO)\n";

    } else {

        std::cout << "  USO " << result.total_bytes << " bytes, "

                  << result.segments_sent << " segment(s), mss=" << mss << '\n';

    }

}

#else

void send_batch_fallback(

    boost::asio::ip::udp::socket& socket,

    const boost::asio::ip::udp::endpoint& remote,

    const char* data,

    std::size_t batch_size,

    std::size_t mss)

{

    if (mss == 0) {

        const std::size_t sent = socket.send_to(

            boost::asio::buffer(data, batch_size), remote);

        if (sent != batch_size) {

            throw std::runtime_error("short send");

        }

        std::cout << "  plain " << batch_size << " bytes (one datagram, no USO)\n";

        return;

    }



    std::size_t offset = 0;

    std::size_t segments = 0;

    while (offset < batch_size) {

        const std::size_t chunk = std::min(mss, batch_size - offset);

        const std::size_t sent = socket.send_to(

            boost::asio::buffer(data + offset, chunk), remote);

        if (sent != chunk) {

            throw std::runtime_error("short send");

        }

        ++segments;

        offset += chunk;

    }

    std::cout << "  sent " << batch_size << " bytes in " << segments

              << " datagram(s), mss=" << mss << " (no USO)\n";

}

#endif



void run_client(const boostudp::ClientOptions& options)

{

    const boost::asio::ip::udp::endpoint remoteEndpoint(

        boostudp::parse_address(options.destAddress), options.destPort);



    std::vector<char> batch(options.batchSize);



    std::random_device seed;

    std::mt19937 rng(seed());



#ifdef _WIN32

    const auto uso_socket = boostudp::create_uso_udp_socket(options.sourceAddress);

    const auto bound = boostudp::uso_socket_local_endpoint(uso_socket);



    std::cout << "bound to " << bound << ", sending " << options.batchCount

              << " batch(es), size=" << options.batchSize

              << " mss=" << options.mss << " to " << remoteEndpoint

              << (options.mss == 0 ? " (plain UDP)\n" : " (USO)\n");



    const auto started = std::chrono::steady_clock::now();



    for (std::size_t i = 0; i < options.batchCount; ++i) {

        boostudp::build_batch(batch, static_cast<std::uint32_t>(i), rng);

        std::cout << "batch " << i << ":\n";

        send_batch_windows(

            uso_socket,

            remoteEndpoint,

            bound.address(),

            batch.data(),

            options.batchSize,

            options.mss);

    }



    boostudp::close_uso_udp_socket(uso_socket);



    const auto elapsed = std::chrono::steady_clock::now() - started;

    const auto ms =

        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    std::cout << "sent " << options.batchCount << " batch(es), "

              << options.batchSize * options.batchCount

              << " bytes in " << ms << " ms\n";

#else

    boost::asio::io_context io;

    boost::asio::ip::udp::endpoint localEndpoint(

        boostudp::parse_address(options.sourceAddress), 0);

    boost::asio::ip::udp::socket socket(io);

    socket.open(localEndpoint.protocol());

    socket.set_option(boost::asio::socket_base::reuse_address(true));

    socket.bind(localEndpoint);



    const auto bound = socket.local_endpoint();

    std::cout << "bound to " << bound << ", sending " << options.batchCount

              << " batch(es), size=" << options.batchSize

              << " mss=" << options.mss << " to " << remoteEndpoint << '\n';



    const auto started = std::chrono::steady_clock::now();



    for (std::size_t i = 0; i < options.batchCount; ++i) {

        boostudp::build_batch(batch, static_cast<std::uint32_t>(i), rng);

        std::cout << "batch " << i << ":\n";

        send_batch_fallback(

            socket, remoteEndpoint, batch.data(), options.batchSize, options.mss);

    }



    const auto elapsed = std::chrono::steady_clock::now() - started;

    const auto ms =

        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    std::cout << "sent " << options.batchCount << " batch(es), "

              << options.batchSize * options.batchCount

              << " bytes in " << ms << " ms\n";

#endif

}



} // namespace



int main(int argc, char* argv[])

{

    boostudp::ClientOptions options;

    try {

        if (!boostudp::parse_client_options(argc, argv, options)) {

            return 0;

        }

        run_client(options);

    } catch (const std::exception& ex) {

        std::cerr << "Error: " << ex.what() << '\n';

        return 1;

    }



    return 0;

}

