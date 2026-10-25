#pragma once
#include "lobby_protocol.hpp"
#include <enet/enet.h>
#include <chrono>
#include <memory>
#ifndef _WIN32
#include <fcntl.h>
#endif

namespace online::lobby {
inline uint64_t monotonic_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
using Host = std::unique_ptr<ENetHost, decltype(&enet_host_destroy)>;
inline Host make_host(const ENetAddress* address, size_t peers) {
    Host host(enet_host_create(address, peers, channel_count, 0, 0), enet_host_destroy);
    if (!host) throw std::runtime_error("cannot create or bind lobby UDP socket");
    host->maximumPacketSize = packet_limit;
    host->maximumWaitingData = 64 * 1024;
#ifndef _WIN32
    // The persistent owner's connection must never leak into disposable games.
    auto flags = fcntl(host->socket, F_GETFD);
    if (flags < 0 || fcntl(host->socket, F_SETFD, flags | FD_CLOEXEC) < 0)
        throw std::runtime_error("cannot isolate lobby UDP socket from child processes");
#endif
    return host;
}
inline bool send_packet(ENetPeer* peer, uint8_t channel, std::span<const uint8_t> bytes, enet_uint32 flags) {
    auto queued = enet_list_size(&peer->outgoingCommands) + enet_list_size(&peer->outgoingSendReliableCommands) +
                  enet_list_size(&peer->sentReliableCommands);
    if (bytes.size() > packet_limit || queued >= 128) return false;
    auto* packet = enet_packet_create(bytes.data(), bytes.size(), flags);
    if (!packet) return false;
    if (enet_peer_send(peer, channel, packet) < 0) { enet_packet_destroy(packet); return false; }
    return true;
}
}
