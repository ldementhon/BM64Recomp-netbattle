#pragma once

// Private owner/child IPC on one machine, not the network wire protocol.
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include "input_history.hpp"

namespace online {
constexpr uint64_t match_channel_version = 2;
enum class MatchEvent : uint64_t { Ready = 1, Start, Finished, InputUpload, InputRelay };
struct MatchMessage {
    uint64_t version = match_channel_version;
    MatchEvent event = MatchEvent::Ready;
    uint64_t match = 0, frame = 0, ram = 0, registers = 0;
    InputBatch input{};
};
static_assert(sizeof(MatchMessage) == 240);
inline MatchMessage input_message(const InputBatch& input, bool relay) {
    MatchMessage message;
    message.event = relay ? MatchEvent::InputRelay : MatchEvent::InputUpload;
    message.match = input.match; message.input = input;
    return message;
}

class MatchChannel {
public:
    MatchChannel() = default;
    explicit MatchChannel(int fd) : fd_(fd) {}
    ~MatchChannel() { if (fd_ >= 0) close(fd_); }
    MatchChannel(const MatchChannel&) = delete;
    MatchChannel& operator=(const MatchChannel&) = delete;
    MatchChannel(MatchChannel&& other) noexcept : fd_(std::exchange(other.fd_, -1)),
        bytes_(other.bytes_), used_(other.used_), outgoing_(std::move(other.outgoing_)), sent_(other.sent_) {}
    MatchChannel& operator=(MatchChannel&& other) noexcept {
        std::swap(fd_, other.fd_);
        std::swap(bytes_, other.bytes_);
        std::swap(used_, other.used_);
        std::swap(outgoing_, other.outgoing_); std::swap(sent_, other.sent_);
        return *this;
    }
    int fd() const { return fd_; }

    static std::pair<MatchChannel, MatchChannel> pair() {
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) fail("match socket");
        MatchChannel owner(sockets[0]), child(sockets[1]);
        for (int fd : sockets) {
            if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
                fail("match socket flags");
#ifdef SO_NOSIGPIPE
            int enabled = 1;
            if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled))) fail("match socket signal");
#endif
        }
        return {std::move(owner), std::move(child)};
    }

    void send(const MatchMessage& message) {
        flush();
        // More than five seconds of four 60-Hz relay streams, with a fixed cap.
        if (outgoing_.size() >= 2048) throw std::runtime_error("match event queue is full");
        outgoing_.push_back(message);
        flush();
    }

    bool flush() {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        while (!outgoing_.empty()) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(&outgoing_.front());
            ssize_t size;
            do { size = ::send(fd_, bytes + sent_, sizeof(MatchMessage) - sent_, flags); }
            while (size < 0 && errno == EINTR);
            if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;
            if (size < 0) fail("send match event");
            if (!size) throw std::runtime_error("match channel stopped accepting data");
            sent_ += size;
            if (sent_ == sizeof(MatchMessage)) { outgoing_.pop_front(); sent_ = 0; }
        }
        return true;
    }

    std::optional<MatchMessage> receive() {
        flush();
        ssize_t size;
        do { size = recv(fd_, bytes_.data() + used_, bytes_.size() - used_, 0); }
        while (size < 0 && errno == EINTR);
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return std::nullopt;
        if (size < 0) fail("receive match event");
        if (!size) throw std::runtime_error("match owner/child disconnected");
        used_ += size;
        if (used_ < bytes_.size()) return std::nullopt;
        MatchMessage message;
        std::memcpy(&message, bytes_.data(), bytes_.size());
        used_ = 0;
        bool input = message.event == MatchEvent::InputUpload || message.event == MatchEvent::InputRelay;
        if (message.version != match_channel_version || message.event < MatchEvent::Ready || message.event > MatchEvent::InputRelay ||
            !message.match || (!input && !message.frame))
            throw std::runtime_error("invalid match event");
        if (input) {
            validate_batch(message.input);
            if (message.input.match != message.match) throw std::runtime_error("input match differs from event");
        }
        return message;
    }

private:
    [[noreturn]] static void fail(const char* operation) {
        throw std::system_error(errno, std::generic_category(), operation);
    }
    int fd_ = -1;
    std::array<uint8_t, sizeof(MatchMessage)> bytes_{};
    size_t used_ = 0;
    std::deque<MatchMessage> outgoing_;
    size_t sent_ = 0;
};
}
