#include "net/server.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "common/crc32.hpp"
#include "common/logging.hpp"
#include "protocol/protocol.hpp"

namespace ast {

namespace {

bool winsock_ready() {
    static bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}

constexpr ULONG_PTR kSentinelKey = (ULONG_PTR)(-1);

} // namespace

// A single connection. Lifetime is managed by a manual refcount (`refs`):
//   - the connection table holds one reference,
//   - each outstanding overlapped op (recv/send) holds one,
//   - each queued worker job holds one.
// When the count reaches zero the connection is deleted. This keeps the
// OVERLAPPED structs (embedded in the object) valid until their I/O completes.
struct Connection {
    SOCKET s = INVALID_SOCKET;
    uint64_t id = 0;
    std::atomic<bool> closing{false};
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);

    // recv (one in flight)
    std::vector<uint8_t> recv_buf;
    WSABUF recv_wsa{};
    OVERLAPPED recv_ov{};
    std::vector<uint8_t> recv_pending;

    // send (one in flight) + bounded queue
    std::mutex send_mu;
    std::deque<std::vector<uint8_t>> send_queue;
    uint64_t send_queued_bytes = 0;
    bool send_pending = false;
    WSABUF send_wsa{};
    OVERLAPPED send_ov{};
    std::vector<uint8_t> send_buf;

    std::atomic<int> refs{1};

    void add_ref() { refs.fetch_add(1); }
    void release() {
        if (refs.fetch_sub(1) == 1) delete this;
    }
};

struct Server::Impl {
    Config cfg;
    RequestHandler* handler = nullptr;

    SOCKET listen_sock = INVALID_SOCKET;
    HANDLE iocp = INVALID_HANDLE_VALUE;

    std::atomic<bool> stopping{false};

    std::mutex conn_mu;
    std::unordered_map<uint64_t, Connection*> conns;
    std::atomic<uint64_t> next_id{1};

    struct Job {
        uint8_t type;
        std::vector<uint8_t> body;
        Connection* conn;
    };
    std::mutex job_mu;
    std::condition_variable job_cv;
    std::deque<Job> jobs;
    std::atomic<bool> running{false};

    std::vector<std::thread> io_threads;
    std::vector<std::thread> worker_threads;
    std::thread listener_thread;

    std::atomic<uint64_t> connections{0};
    std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<uint64_t> bytes_in{0};
    std::atomic<uint64_t> bytes_out{0};
    std::atomic<uint64_t> frames_handled{0};
    std::atomic<uint64_t> queued{0};
};

Server::Server() = default;

static void close_conn(Server::Impl* impl, Connection* c) {
    if (c->closing.exchange(true)) return;
    if (c->cancel) c->cancel->store(true);
    if (c->s != INVALID_SOCKET) {
        closesocket(c->s);
        c->s = INVALID_SOCKET;
    }
    {
        std::lock_guard<std::mutex> lk(impl->conn_mu);
        impl->conns.erase(c->id);
    }
    c->release(); // the table's reference
}

static void post_recv(Server::Impl* impl, Connection* c) {
    if (c->closing) return;
    DWORD flags = 0;
    c->add_ref();
    int rc = WSARecv(c->s, &c->recv_wsa, 1, nullptr, &flags, &c->recv_ov, nullptr);
    if (rc == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            c->release();
            close_conn(impl, c);
        }
    }
}

// Parse complete frames from recv_pending; dispatch each to a worker job.
static Error parse_frames(Server::Impl* impl, Connection* c) {
    auto& buf = c->recv_pending;
    size_t pos = 0;
    while (true) {
        if (buf.size() - pos < proto::kHeaderSize) break;
        uint32_t magic = get_le32(buf.data() + pos);
        uint8_t version = buf[pos + 4];
        uint8_t type = buf[pos + 5];
        uint32_t body_len = get_le32(buf.data() + pos + 8);
        uint32_t crc = get_le32(buf.data() + pos + 12);

        if (magic != proto::kMagic) return Error(Err::InvalidArgument, "bad magic");
        if (version != proto::kVersion) return Error(Err::Unsupported, "unsupported version");
        if ((uint64_t)body_len > impl->cfg.max_frame_size)
            return Error(Err::LimitExceeded, "frame too large");
        if (buf.size() - pos < proto::kHeaderSize + body_len) break; // need more

        if (crc32(buf.data() + pos + proto::kHeaderSize, body_len) != crc)
            return Error(Err::Corrupt, "frame checksum mismatch");

        Server::Impl::Job job;
        job.type = type;
        job.body.assign(buf.data() + pos + proto::kHeaderSize,
                        buf.data() + pos + proto::kHeaderSize + body_len);
        job.conn = c;
        c->add_ref(); // job reference
        {
            std::lock_guard<std::mutex> lk(impl->job_mu);
            if (impl->jobs.size() >= impl->cfg.max_queued_requests) {
                c->release(); // undo the job reference
                return Error(Err::LimitExceeded, "request queue full");
            }
            impl->jobs.push_back(std::move(job));
        }
        impl->job_cv.notify_one();
        pos += proto::kHeaderSize + body_len;
    }
    if (pos > 0) buf.erase(buf.begin(), buf.begin() + pos);
    return ok();
}

static void initiate_send(Connection* c) {
    // Requires c->send_mu held.
    if (c->closing || c->send_queue.empty()) return;
    c->send_buf = std::move(c->send_queue.front());
    c->send_queue.pop_front();
    c->send_queued_bytes -= c->send_buf.size();
    c->send_wsa.buf = reinterpret_cast<CHAR*>(c->send_buf.data());
    c->send_wsa.len = (ULONG)c->send_buf.size();
    c->add_ref();
    int rc = WSASend(c->s, &c->send_wsa, 1, nullptr, 0, &c->send_ov, nullptr);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        c->release();
        c->send_pending = false;
    }
}

static void enqueue_send(Server::Impl* impl, Connection* c, std::vector<uint8_t>&& frame) {
    std::unique_lock<std::mutex> lk(c->send_mu);
    if (c->closing) return;
    if (c->send_queued_bytes + frame.size() > impl->cfg.send_queue_max_bytes) {
        // Backpressure: drop this connection rather than grow without bound.
        lk.unlock();
        close_conn(impl, c);
        return;
    }
    c->send_queued_bytes += frame.size();
    c->send_queue.push_back(std::move(frame));
    if (!c->send_pending) {
        c->send_pending = true;
        initiate_send(c);
    }
}

static void on_recv(Server::Impl* impl, Connection* c, DWORD bytes) {
    if (bytes > 0) {
        impl->bytes_in.fetch_add(bytes);
        c->recv_pending.insert(c->recv_pending.end(), c->recv_buf.begin(),
                               c->recv_buf.begin() + bytes);
        auto e = parse_frames(impl, c);
        if (!e.ok()) {
            auto err = proto::encode_error_response((uint8_t)e.code, e.message);
            auto frame = proto::encode_frame(proto::ErrorResp, 0, err);
            enqueue_send(impl, c, std::move(frame.vec()));
            close_conn(impl, c);
        }
    }
    if (!c->closing) post_recv(impl, c);
    c->release();
}

static void on_send(Server::Impl* impl, Connection* c, DWORD bytes) {
    impl->bytes_out.fetch_add(bytes);
    {
        std::lock_guard<std::mutex> lk(c->send_mu);
        c->send_pending = false;
        if (!c->closing && !c->send_queue.empty()) {
            c->send_pending = true;
            initiate_send(c);
        }
    }
    c->release();
}

static void process_job(Server::Impl* impl, Server::Impl::Job&& job) {
    Connection* c = job.conn;
    if (c->closing) {
        c->release();
        return;
    }
    std::vector<uint8_t> types;
    std::vector<uint16_t> flags;
    std::vector<std::vector<uint8_t>> bodies;
    impl->handler->handle(job.type, job.body.data(), job.body.size(), types, flags, bodies,
                          c->cancel);
    impl->frames_handled.fetch_add(1);

    for (size_t i = 0; i < types.size(); ++i) {
        auto frame = proto::encode_frame(types[i], flags[i], bodies[i].data(), bodies[i].size());
        enqueue_send(impl, c, std::move(frame.vec()));
    }
    c->release();
}

static void io_loop(Server::Impl* impl) {
    while (true) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* ov = nullptr;
        BOOL ok = GetQueuedCompletionStatus(impl->iocp, &bytes, &key, &ov, INFINITE);
        if (ov == nullptr) {
            if (key == kSentinelKey) break;
            continue;
        }
        Connection* c = reinterpret_cast<Connection*>(key);
        if (!c) continue;
        if (ov == &c->recv_ov) {
            on_recv(impl, c, ok ? bytes : 0);
        } else if (ov == &c->send_ov) {
            on_send(impl, c, ok ? bytes : 0);
        }
    }
}

static void worker_loop(Server::Impl* impl) {
    while (true) {
        Server::Impl::Job job;
        {
            std::unique_lock<std::mutex> lk(impl->job_mu);
            impl->job_cv.wait(lk, [&] { return !impl->running || !impl->jobs.empty(); });
            if (impl->jobs.empty() && !impl->running) break;
            job = std::move(impl->jobs.front());
            impl->jobs.pop_front();
        }
        impl->queued.store(impl->jobs.size());
        process_job(impl, std::move(job));
    }
}

static void listener_loop(Server::Impl* impl) {
    while (!impl->stopping) {
        SOCKET s = accept(impl->listen_sock, nullptr, nullptr);
        if (s == INVALID_SOCKET) {
            if (impl->stopping) break;
            continue;
        }
        uint64_t nconns;
        {
            std::lock_guard<std::mutex> lk(impl->conn_mu);
            nconns = impl->conns.size();
        }
        if (nconns >= impl->cfg.max_connections) {
            impl->rejected.fetch_add(1);
            closesocket(s);
            continue;
        }
        auto conn = new Connection();
        conn->s = s;
        conn->id = impl->next_id.fetch_add(1);
        conn->recv_buf.resize(impl->cfg.recv_buffer_size);
        conn->recv_wsa.buf = reinterpret_cast<CHAR*>(conn->recv_buf.data());
        conn->recv_wsa.len = (ULONG)conn->recv_buf.size();

        CreateIoCompletionPort(reinterpret_cast<HANDLE>(s), impl->iocp,
                               reinterpret_cast<ULONG_PTR>(conn), 0);
        {
            std::lock_guard<std::mutex> lk(impl->conn_mu);
            impl->conns[conn->id] = conn;
            impl->connections.store(impl->conns.size());
        }
        impl->accepted.fetch_add(1);
        post_recv(impl, conn);
    }
}

Server::~Server() {
    stop();
    join();
}

Error Server::start(const Config& cfg, RequestHandler* handler) {
    if (!winsock_ready()) return Error(Err::Internal, "WSAStartup failed");
    if (!handler) return Error(Err::InvalidArgument, "handler required");
    if (impl_) return Error(Err::Internal, "server already started");

    impl_ = std::make_unique<Impl>();
    impl_->cfg = cfg;
    impl_->handler = handler;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    std::string port = std::to_string(cfg.port);
    if (getaddrinfo(cfg.host.c_str(), port.c_str(), &hints, &result) != 0)
        return Error(Err::IoError, "getaddrinfo failed");

    SOCKET ls = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (ls == INVALID_SOCKET) {
        freeaddrinfo(result);
        return Error(Err::IoError, "socket() failed");
    }

    BOOL reuse = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    if (bind(ls, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
        freeaddrinfo(result);
        closesocket(ls);
        return Error(Err::IoError, "bind() failed on port " + port);
    }
    freeaddrinfo(result);

    if (listen(ls, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(ls);
        return Error(Err::IoError, "listen() failed");
    }

    impl_->listen_sock = ls;
    impl_->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0,
                                         (DWORD)cfg.io_threads);
    if (!impl_->iocp) {
        closesocket(ls);
        return Error(Err::IoError, "CreateIoCompletionPort failed");
    }

    impl_->running = true;
    for (uint64_t i = 0; i < cfg.io_threads; ++i)
        impl_->io_threads.emplace_back([this] { io_loop(impl_.get()); });
    for (uint64_t i = 0; i < cfg.worker_threads; ++i)
        impl_->worker_threads.emplace_back([this] { worker_loop(impl_.get()); });
    impl_->listener_thread = std::thread([this] { listener_loop(impl_.get()); });

    AST_LOG_INFO("server listening on " + cfg.host + ":" + port);
    return ok();
}

void Server::stop() {
    if (!impl_) return;
    if (impl_->stopping.exchange(true)) return;

    if (impl_->listen_sock != INVALID_SOCKET) {
        closesocket(impl_->listen_sock);
        impl_->listen_sock = INVALID_SOCKET;
    }

    std::vector<Connection*> all;
    {
        std::lock_guard<std::mutex> lk(impl_->conn_mu);
        for (auto& kv : impl_->conns) all.push_back(kv.second);
        impl_->conns.clear();
    }
    for (Connection* c : all) {
        c->closing.store(true);
        if (c->cancel) c->cancel->store(true);
        if (c->s != INVALID_SOCKET) {
            closesocket(c->s);
            c->s = INVALID_SOCKET;
        }
        c->release(); // release the table's reference
    }

    {
        std::lock_guard<std::mutex> lk(impl_->job_mu);
        impl_->running = false;
    }
    impl_->job_cv.notify_all();

    // Wake completion threads.
    for (size_t i = 0; i < impl_->io_threads.size(); ++i)
        PostQueuedCompletionStatus(impl_->iocp, 0, kSentinelKey, nullptr);
}

void Server::join() {
    if (!impl_) return;
    if (impl_->listener_thread.joinable()) impl_->listener_thread.join();
    for (auto& t : impl_->io_threads) if (t.joinable()) t.join();
    for (auto& t : impl_->worker_threads) if (t.joinable()) t.join();
    if (impl_->iocp != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->iocp);
        impl_->iocp = INVALID_HANDLE_VALUE;
    }
    impl_->connections.store(0);
}

uint64_t Server::connections() const { return impl_ ? impl_->connections.load() : 0; }
uint64_t Server::accepted() const { return impl_ ? impl_->accepted.load() : 0; }
uint64_t Server::rejected() const { return impl_ ? impl_->rejected.load() : 0; }
uint64_t Server::bytes_in() const { return impl_ ? impl_->bytes_in.load() : 0; }
uint64_t Server::bytes_out() const { return impl_ ? impl_->bytes_out.load() : 0; }
uint64_t Server::frames_handled() const { return impl_ ? impl_->frames_handled.load() : 0; }
uint64_t Server::queue_depth() const { return impl_ ? impl_->queued.load() : 0; }

} // namespace ast
