#include "server.hpp"
#include <charconv>
#include <csignal>
#include <iostream>

static volatile std::sig_atomic_t stopping = 0;
static void stop(int) { stopping = 1; }

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: bm64-lobby BIND_ADDRESS UDP_PORT CLIENT_FINGERPRINT_HEX\n";
        return 1;
    }
    try {
        unsigned port = 0;
        std::string port_text = argv[2], fingerprint_text = argv[3];
        auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() || !port || port > 65535)
            throw std::invalid_argument("UDP port must be 1..65535");
        online::lobby::Fingerprint fingerprint{};
        if (fingerprint_text.size() != fingerprint.size() * 2) throw std::invalid_argument("fingerprint must be 64 hex digits");
        for (size_t i = 0; i < fingerprint.size(); ++i) {
            unsigned byte;
            auto* begin = fingerprint_text.data() + 2 * i;
            auto result = std::from_chars(begin, begin + 2, byte, 16);
            if (result.ec != std::errc{} || result.ptr != begin + 2) throw std::invalid_argument("invalid fingerprint hex");
            fingerprint[i] = uint8_t(byte);
        }
        if (enet_initialize() != 0) throw std::runtime_error("ENet initialization failed");
        struct Cleanup { ~Cleanup() { enet_deinitialize(); } } cleanup;
        ENetAddress address{}; address.port = uint16_t(port);
        if (enet_address_set_host(&address, argv[1]) < 0) throw std::invalid_argument("cannot resolve bind address");
        online::lobby::Server server(address, fingerprint);
        std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
        std::cout << "Lobby listening on " << argv[1] << ':' << server.port() << std::endl;
        while (!stopping) server.service(5);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
