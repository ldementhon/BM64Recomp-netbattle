#pragma once

// Private owner/child IPC on one machine, not the network wire protocol.
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#include <memory>
#include <random>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
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
#ifdef _WIN32
    using Handle = HANDLE;
    static inline const Handle invalid = INVALID_HANDLE_VALUE;
#else
    using Handle = int;
    static constexpr Handle invalid = -1;
#endif
    MatchChannel() = default;
    explicit MatchChannel(Handle fd) : fd_(fd) {}
    ~MatchChannel() {
        if (fd_ == invalid) return;
#ifdef _WIN32
        CloseHandle(fd_);
#else
        close(fd_);
#endif
    }
    MatchChannel(const MatchChannel&) = delete;
    MatchChannel& operator=(const MatchChannel&) = delete;
    MatchChannel(MatchChannel&& other) noexcept : fd_(std::exchange(other.fd_, invalid)),
        bytes_(other.bytes_), used_(other.used_), outgoing_(std::move(other.outgoing_)), sent_(other.sent_) {}
    MatchChannel& operator=(MatchChannel&& other) noexcept {
        std::swap(fd_, other.fd_);
        std::swap(bytes_, other.bytes_);
        std::swap(used_, other.used_);
        std::swap(outgoing_, other.outgoing_); std::swap(sent_, other.sent_);
        return *this;
    }
    Handle fd() const { return fd_; }

    static MatchChannel inherited() {
#ifdef _WIN32
        const char* value = std::getenv("BM64_OWNER_CHANNEL");
        if (!value) throw std::runtime_error("missing owner channel");
        uintptr_t handle = 0;
        auto* end = value + std::strlen(value);
        auto parsed = std::from_chars(value, end, handle);
        if (parsed.ec != std::errc{} || parsed.ptr != end || !handle || handle == UINTPTR_MAX)
            throw std::runtime_error("invalid owner channel");
        MatchChannel channel(reinterpret_cast<HANDLE>(handle));
        DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
        if (GetFileType(channel.fd()) != FILE_TYPE_PIPE) throw std::runtime_error("owner channel is not a pipe");
        if (!SetNamedPipeHandleState(channel.fd(), &mode, nullptr, nullptr) ||
            !SetHandleInformation(channel.fd(), HANDLE_FLAG_INHERIT, 0)) fail("adopt owner channel");
#else
        MatchChannel channel(3);
        auto flags = fcntl(channel.fd(), F_GETFL);
        if (flags < 0 || fcntl(channel.fd(), F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(channel.fd(), F_SETFL, flags | O_NONBLOCK) < 0) fail("adopt owner channel");
#endif
        return channel;
    }

    static std::pair<MatchChannel, MatchChannel> pair() {
#ifdef _WIN32
        // Both endpoints are opened here; only the child handle is inherited.
        // Restrict the pipe to its owner and refuse remote connections.
        PSECURITY_DESCRIPTOR raw = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &raw, nullptr))
            fail("match pipe security");
        std::unique_ptr<void, decltype(&LocalFree)> descriptor(raw, LocalFree);
        SECURITY_ATTRIBUTES access{sizeof(SECURITY_ATTRIBUTES), raw, FALSE};
        auto name = L"\\\\.\\pipe\\bm64-match-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                    std::to_wstring(std::random_device{}()) + L"-" + std::to_wstring(std::random_device{}());
        // This channel is explicitly polled; no outstanding overlapped I/O can
        // retain pointers into a moved channel or a popped queue entry.
        MatchChannel owner(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 64 * 1024, 64 * 1024, 0, &access));
        if (owner.fd() == invalid) fail("match pipe");
        MatchChannel child(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        if (child.fd() == invalid) fail("connect match pipe");
        if (!ConnectNamedPipe(owner.fd(), nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
            fail("accept match pipe");
        DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
        if (!SetNamedPipeHandleState(child.fd(), &mode, nullptr, nullptr)) fail("match pipe flags");
#else
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
#endif
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
#ifndef _WIN32
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
#endif
        while (!outgoing_.empty()) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(&outgoing_.front());
#ifdef _WIN32
            DWORD size = 0;
            if (!WriteFile(fd_, bytes + sent_, DWORD(sizeof(MatchMessage) - sent_), &size, nullptr))
                fail("send match event");
            if (!size) return false; // A full nonblocking byte pipe writes zero bytes.
#else
            ssize_t size;
            do { size = ::send(fd_, bytes + sent_, sizeof(MatchMessage) - sent_, flags); }
            while (size < 0 && errno == EINTR);
            if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;
            if (size < 0) fail("send match event");
            if (!size) throw std::runtime_error("match channel stopped accepting data");
#endif
            sent_ += size;
            if (sent_ == sizeof(MatchMessage)) { outgoing_.pop_front(); sent_ = 0; }
        }
        return true;
    }

    std::optional<MatchMessage> receive() {
        flush();
#ifdef _WIN32
        DWORD size = 0;
        if (!ReadFile(fd_, bytes_.data() + used_, DWORD(bytes_.size() - used_), &size, nullptr)) {
            if (GetLastError() == ERROR_NO_DATA) return std::nullopt;
            fail("receive match event");
        }
#else
        ssize_t size;
        do { size = recv(fd_, bytes_.data() + used_, bytes_.size() - used_, 0); }
        while (size < 0 && errno == EINTR);
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return std::nullopt;
        if (size < 0) fail("receive match event");
#endif
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
#ifdef _WIN32
        throw std::system_error(GetLastError(), std::system_category(), operation);
#else
        throw std::system_error(errno, std::generic_category(), operation);
#endif
    }
    Handle fd_ = invalid;
    std::array<uint8_t, sizeof(MatchMessage)> bytes_{};
    size_t used_ = 0;
    std::deque<MatchMessage> outgoing_;
    size_t sent_ = 0;
};
}
