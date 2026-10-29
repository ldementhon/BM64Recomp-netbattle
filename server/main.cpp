#include "server.hpp"
#include "release_file.hpp"
#include <fstream>
#include <charconv>
#include <csignal>
#include <iostream>

static volatile std::sig_atomic_t stopping = 0;
static void stop(int) { stopping = 1; }

static std::string short_hex(const online::lobby::Fingerprint& fingerprint) {
    constexpr char hex[] = "0123456789abcdef";
    std::string text;
    for (size_t i = 0; i < 4; ++i) { text += hex[fingerprint[i] >> 4]; text += hex[fingerprint[i] & 15]; }
    return text;
}

int main(int argc, char** argv) {
    if (argc < 4 || argc > 7) {
        std::cerr << "Usage: bm64-lobby BIND_ADDRESS UDP_PORT RELEASE_FILE\n"
                     "       bm64-lobby BIND_ADDRESS UDP_PORT CLIENT_FINGERPRINT_HEX [CLIENT_FINGERPRINT_HEX ...]\n"
                     "RELEASE_FILE comes from server/make_release.py. Fingerprints are for testing local builds\n"
                     "(one per platform build of a set validated together, at most 4).\n";
        return 1;
    }
    try {
        unsigned port = 0;
        std::string port_text = argv[2];
        auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() || !port || port > 65535)
            throw std::invalid_argument("UDP port must be 1..65535");
        std::vector<online::lobby::Fingerprint> accepted;
        std::string accepting;
        if (argc == 4 && std::string_view(argv[3]).size() != 64) {
            std::ifstream input(argv[3]);
            if (!input) throw std::runtime_error(std::string("cannot read release file ") + argv[3]);
            auto file = online::lobby::parse_release_file(input);
            if (file.release != online::release)
                throw std::runtime_error("this server is online " + online::release_label(online::release) +
                                         " but the release file is for " + online::release_label(file.release) +
                                         "; build the server from the matching source");
            for (const auto& [label, fingerprint] : file.builds) {
                accepted.push_back(fingerprint);
                accepting += (accepting.empty() ? "" : ", ") + label + " " + short_hex(fingerprint);
            }
        } else {
            for (int arg = 3; arg < argc; ++arg) accepted.push_back(online::lobby::parse_fingerprint(argv[arg]));
            accepting = std::to_string(accepted.size()) + " build(s) from the command line";
        }
        if (enet_initialize() != 0) throw std::runtime_error("ENet initialization failed");
        struct Cleanup { ~Cleanup() { enet_deinitialize(); } } cleanup;
        ENetAddress address{}; address.port = uint16_t(port);
        if (enet_address_set_host(&address, argv[1]) < 0) throw std::invalid_argument("cannot resolve bind address");
        online::lobby::Server server(address, std::move(accepted));
        std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
        std::cout << "Online " << online::release_label(online::release) << ": accepting " << accepting << std::endl;
        std::cout << "Lobby listening on " << argv[1] << ':' << server.port() << std::endl;
        while (!stopping) server.service(5);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
