#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "test_framework.hpp"
#include "test_util.hpp"

#include "common/clock.hpp"
#include "core/node.hpp"
#include "net/client.hpp"
#include "protocol/protocol.hpp"

#include <chrono>
#include <thread>

using namespace ast;
using namespace ast::test;

static uint16_t find_free_port() {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = 0;
    bind(s, (sockaddr*)&sa, sizeof(sa));
    int len = sizeof(sa);
    getsockname(s, (sockaddr*)&sa, &len);
    uint16_t port = ntohs(sa.sin_port);
    closesocket(s);
    return port;
}

static Config make_node_config(const std::string& dir, uint16_t port) {
    Config cfg;
    cfg.node_id = 1;
    cfg.data_dir = dir;
    cfg.network.listen_host = "127.0.0.1";
    cfg.network.listen_port = port;
    cfg.network.io_threads = 2;
    cfg.network.worker_threads = 2;
    cfg.storage.segment_size_bytes = 1 << 20;
    return cfg;
}

static EventRecord test_event(uint64_t tenant, const std::string& key, uint64_t seq,
                              const std::string& payload) {
    EventRecord e;
    e.tenant_id = tenant;
    e.key = key;
    e.sequence = seq;
    e.payload.assign(payload.begin(), payload.end());
    for (int i = 0; i < 16; ++i) e.event_id[i] = (uint8_t)(tenant ^ seq ^ i);
    return e;
}

TEST_CASE(net_end_to_end) {
    SteadyClock::instance().init();
    TempDir tmp;
    uint16_t port = find_free_port();

    auto node = Node::create(make_node_config(tmp.str(), port));
    REQUIRE(node.ok());
    REQUIRE((*node)->start().ok());

    Client::Config cc;
    cc.host = "127.0.0.1";
    cc.port = port;
    auto cli = Client::connect(cc);
    REQUIRE(cli.ok());

    // Ping
    auto r = cli->request(proto::Ping, Buffer());
    REQUIRE(r.ok());
    CHECK_EQ((int)r->type, (int)proto::Pong);

    // Append
    auto ab = proto::encode_append_request(test_event(1, "k1", 1, "hello"));
    auto ar = cli->request(proto::AppendReq, ab);
    REQUIRE(ar.ok());
    CHECK_EQ((int)ar->type, (int)proto::AppendResp);
    auto aresp = proto::decode_append_response(ar->body.data(), ar->body.size());
    REQUIRE(aresp.ok());
    CHECK_EQ(aresp->code, 0u);
    CHECK_EQ(aresp->sequence, 1u);

    // Idempotent retry over the wire
    auto ar2 = cli->request(proto::AppendReq, ab);
    REQUIRE(ar2.ok());
    auto aresp2 = proto::decode_append_response(ar2->body.data(), ar2->body.size());
    REQUIRE(aresp2.ok());
    CHECK_EQ(aresp2->code, 1u); // duplicate

    // Get
    proto::GetRequest g;
    g.tenant = 1;
    g.key = "k1";
    auto gr = cli->request(proto::GetReq, proto::encode_get_request(g));
    REQUIRE(gr.ok());
    auto code = proto::decode_get_response_code(gr->body.data(), gr->body.size());
    REQUIRE(code.ok());
    CHECK_EQ(*code, 0u);

    // Scan
    proto::ScanRequest s;
    s.tenant = 1;
    s.from_ts = 0;
    s.to_ts = ~0ULL;
    s.max_results = 100;
    uint64_t n = 0;
    auto sr = cli->request_stream(proto::ScanReq, proto::encode_scan_request(s), proto::ScanEnd,
                                  [&](const proto::Frame& f) {
        if (f.type == proto::ScanResp) n++;
        return true;
    });
    REQUIRE(sr.ok());
    CHECK_EQ(n, 1u);

    // Status
    auto st = cli->request(proto::StatusReq, Buffer());
    REQUIRE(st.ok());
    CHECK_EQ((int)st->type, (int)proto::StatusResp);

    cli->close();
    (*node)->request_shutdown();
    (*node)->stop();
    (*node)->join();
}

TEST_CASE(net_oversized_frame_rejected) {
    SteadyClock::instance().init();
    TempDir tmp;
    uint16_t port = find_free_port();

    Config cfg = make_node_config(tmp.str(), port);
    cfg.network.max_frame_size = 4096; // shrink limit
    auto node = Node::create(cfg);
    REQUIRE(node.ok());
    REQUIRE((*node)->start().ok());

    Client::Config cc;
    cc.host = "127.0.0.1";
    cc.port = port;
    cc.max_frame_size = 1024 * 1024;
    auto cli = Client::connect(cc);
    REQUIRE(cli.ok());

    // A large payload that exceeds the server's frame limit.
    std::string big(8 * 1024, 'x');
    auto ab = proto::encode_append_request(test_event(1, "k", 1, big));
    auto ar = cli->request(proto::AppendReq, ab);
    // The server closes the connection on an oversized frame; the request will
    // fail or return an error response. Either way it must not crash.
    if (ar.ok()) {
        CHECK_EQ((int)ar->type, (int)proto::ErrorResp);
    }

    (*node)->request_shutdown();
    (*node)->stop();
    (*node)->join();
}
