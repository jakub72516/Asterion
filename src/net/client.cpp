#include "net/client.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "common/crc32.hpp"

namespace ast {

namespace {
bool winsock_ready() {
    static bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
} // namespace

Result<Client> Client::connect(const Config& cfg) {
    if (!winsock_ready()) return Error(Err::Internal, "WSAStartup failed");

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    std::string port = std::to_string(cfg.port);
    if (getaddrinfo(cfg.host.c_str(), port.c_str(), &hints, &result) != 0)
        return Error(Err::IoError, "getaddrinfo failed for " + cfg.host);

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* ai = result; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (::connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(result);
    if (s == INVALID_SOCKET)
        return Error(Err::IoError, "connect failed to " + cfg.host + ":" + port);

    DWORD timeout = (DWORD)cfg.timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    Client c;
    c.sock_ = (uintptr_t)s;
    c.max_frame_size_ = cfg.max_frame_size;
    c.timeout_ms_ = cfg.timeout_ms;
    return c;
}

void Client::close() {
    if (is_open()) {
        closesocket((SOCKET)sock_);
        sock_ = kInvalidSocket;
    }
}

Client::Client(Client&& o) noexcept
    : sock_(o.sock_), max_frame_size_(o.max_frame_size_), timeout_ms_(o.timeout_ms_) {
    o.sock_ = kInvalidSocket;
}

Client& Client::operator=(Client&& o) noexcept {
    if (this != &o) {
        close();
        sock_ = o.sock_;
        max_frame_size_ = o.max_frame_size_;
        timeout_ms_ = o.timeout_ms_;
        o.sock_ = kInvalidSocket;
    }
    return *this;
}

Error Client::send_all(const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send((SOCKET)sock_, reinterpret_cast<const char*>(data + sent), (int)(len - sent), 0);
        if (n == SOCKET_ERROR) return Error(Err::IoError, "send failed");
        if (n == 0) return Error(Err::IoError, "send closed");
        sent += (size_t)n;
    }
    return ok();
}

Error Client::read_exact(uint8_t* dst, size_t len) {
    size_t got = 0;
    while (got < len) {
        int n = recv((SOCKET)sock_, reinterpret_cast<char*>(dst + got), (int)(len - got), 0);
        if (n == SOCKET_ERROR) return Error(Err::IoError, "recv failed");
        if (n == 0) return Error(Err::IoError, "connection closed");
        got += (size_t)n;
    }
    return ok();
}

Result<proto::Frame> Client::read_frame() {
    uint8_t hdr[proto::kHeaderSize];
    auto e = read_exact(hdr, proto::kHeaderSize);
    if (!e.ok()) return e;

    uint32_t magic = get_le32(hdr + 0);
    uint8_t version = hdr[4];
    uint32_t body_len = get_le32(hdr + 8);
    uint32_t crc = get_le32(hdr + 12);
    if (magic != proto::kMagic) return Error(Err::Corrupt, "bad magic from server");
    if (version != proto::kVersion) return Error(Err::Unsupported, "unsupported version");
    if ((uint64_t)body_len > max_frame_size_) return Error(Err::LimitExceeded, "frame too large");

    std::vector<uint8_t> body(body_len);
    if (body_len > 0) {
        e = read_exact(body.data(), body_len);
        if (!e.ok()) return e;
    }
    if (crc32(body.data(), body_len) != crc)
        return Error(Err::Corrupt, "frame checksum mismatch");

    proto::Frame f;
    f.type = hdr[5];
    f.flags = get_le16(hdr + 6);
    f.body = std::move(body);
    return f;
}

Result<proto::Frame> Client::request(uint8_t type, const Buffer& body) {
    return request(type, body.vec());
}

Result<proto::Frame> Client::request(uint8_t type, const std::vector<uint8_t>& body) {
    auto frame = proto::encode_frame(type, 0, body.data(), body.size());
    auto e = send_all(frame.data(), frame.size());
    if (!e.ok()) return e;
    return read_frame();
}

Result<size_t> Client::request_stream(uint8_t type, const Buffer& body, uint8_t end_type,
                                     const std::function<bool(const proto::Frame&)>& on_frame) {
    auto frame = proto::encode_frame(type, 0, body.data(), body.size());
    auto e = send_all(frame.data(), frame.size());
    if (!e.ok()) return e;

    size_t count = 0;
    while (true) {
        auto f = read_frame();
        if (!f.ok()) return f.error();
        bool more = on_frame(f.value());
        count++;
        if (f->type == end_type) break;
        if (!more) break;
    }
    return count;
}

} // namespace ast
