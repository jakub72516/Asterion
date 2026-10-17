#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "test_framework.hpp"
#include "test_util.hpp"

#include "common/clock.hpp"
#include "core/cluster.hpp"
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

static Config make_cluster_cfg(uint64_t id, uint16_t port, const std::string& dir,
                               uint16_t p1, uint16_t p2, uint16_t p3,
                               ConsistencyLevel cl = ConsistencyLevel::Quorum) {
    Config cfg;
    cfg.node_id = id;
    cfg.data_dir = dir;
    cfg.network.listen_host = "127.0.0.1";
    cfg.network.listen_port = port;
    cfg.network.io_threads = 2;
    cfg.network.worker_threads = 2;
    cfg.storage.segment_size_bytes = 1 << 20;
    cfg.replication.partition_count = 4;
    cfg.replication.replication_factor = 3;
    cfg.replication.consistency_level = cl;
    cfg.replication.peers = {
        {1, "127.0.0.1", p1},
        {2, "127.0.0.1", p2},
        {3, "127.0.0.1", p3},
    };
    return cfg;
}

static EventRecord make_event(uint64_t tenant, const std::string& key, uint64_t seq) {
    EventRecord e;
    e.tenant_id = tenant;
    e.key = key;
    e.sequence = seq;
    e.payload = {'p', 'a', 'y'};
    for (int i = 0; i < 16; ++i) e.event_id[i] = (uint8_t)(tenant ^ seq ^ i);
    return e;
}

TEST_CASE(cluster_replication_quorum) {
    SteadyClock::instance().init();
    TempDir tmp;
    uint16_t p1 = find_free_port(), p2 = find_free_port(), p3 = find_free_port();

    auto n1 = Node::create(make_cluster_cfg(1, p1, tmp.str() + "\\n1", p1, p2, p3));
    auto n2 = Node::create(make_cluster_cfg(2, p2, tmp.str() + "\\n2", p1, p2, p3));
    auto n3 = Node::create(make_cluster_cfg(3, p3, tmp.str() + "\\n3", p1, p2, p3));
    REQUIRE(n1.ok());
    REQUIRE(n2.ok());
    REQUIRE(n3.ok());
    REQUIRE((*n1)->start().ok());
    REQUIRE((*n2)->start().ok());
    REQUIRE((*n3)->start().ok());

    uint64_t tenant = 7;
    std::string key = "orders/42";
    uint32_t part = partition_hash(tenant, key) % 4;
    std::vector<uint64_t> ids = {1, 2, 3};
    uint64_t leader = ids[part % 3];
    uint64_t follower = (leader == 1) ? 2 : 1;

    auto port_of = [&](uint64_t id) { return id == 1 ? p1 : (id == 2 ? p2 : p3); };

    // Append to the leader.
    {
        Client::Config cc;
        cc.host = "127.0.0.1";
        cc.port = port_of(leader);
        auto cli = Client::connect(cc);
        REQUIRE(cli.ok());
        auto ar = cli->request(proto::AppendReq, proto::encode_append_request(make_event(tenant, key, 1)));
        REQUIRE(ar.ok());
        auto aresp = proto::decode_append_response(ar->body.data(), ar->body.size());
        REQUIRE(aresp.ok());
        CHECK_EQ(aresp->code, 0u);
    }

    // Read it back from a follower (replicated).
    {
        Client::Config cc;
        cc.host = "127.0.0.1";
        cc.port = port_of(follower);
        auto cli = Client::connect(cc);
        REQUIRE(cli.ok());
        proto::GetRequest g;
        g.tenant = tenant;
        g.key = key;
        auto gr = cli->request(proto::GetReq, proto::encode_get_request(g));
        REQUIRE(gr.ok());
        auto code = proto::decode_get_response_code(gr->body.data(), gr->body.size());
        REQUIRE(code.ok());
        CHECK_EQ(*code, 0u);
    }

    (*n1)->request_shutdown();
    (*n2)->request_shutdown();
    (*n3)->request_shutdown();
    (*n1)->stop();
    (*n2)->stop();
    (*n3)->stop();
    (*n1)->join();
    (*n2)->join();
    (*n3)->join();
}

TEST_CASE(cluster_catch_up_after_disconnect) {
    SteadyClock::instance().init();
    TempDir tmp;
    uint16_t p1 = find_free_port(), p2 = find_free_port(), p3 = find_free_port();

    // Start only the leader (node 1, ONE consistency so writes succeed solo)
    // and write some data.
    auto n1 = Node::create(make_cluster_cfg(1, p1, tmp.str() + "\\n1", p1, p2, p3, ConsistencyLevel::One));
    REQUIRE(n1.ok());
    REQUIRE((*n1)->start().ok());

    uint64_t tenant = 9;
    std::string key = "cust/7";
    uint32_t part = partition_hash(tenant, key) % 4;
    std::vector<uint64_t> ids = {1, 2, 3};
    uint64_t leader = ids[part % 3];

    // Write through the leader node (whatever it is) directly via its cluster,
    // since other nodes are not up yet. To keep it simple, pick a key whose
    // leader is node 1.
    while (leader != 1) {
        tenant++;
        part = partition_hash(tenant, key) % 4;
        leader = ids[part % 3];
    }

    {
        Client::Config cc;
        cc.host = "127.0.0.1";
        cc.port = p1;
        auto cli = Client::connect(cc);
        REQUIRE(cli.ok());
        for (int s = 1; s <= 5; ++s) {
            auto ar = cli->request(proto::AppendReq, proto::encode_append_request(make_event(tenant, key, s)));
            REQUIRE(ar.ok());
            auto aresp = proto::decode_append_response(ar->body.data(), ar->body.size());
            REQUIRE(aresp.ok());
            CHECK_EQ(aresp->code, 0u);
        }
    }

    // Now start node 2 and let it catch up from the leader.
    auto n2 = Node::create(make_cluster_cfg(2, p2, tmp.str() + "\\n2", p1, p2, p3));
    REQUIRE(n2.ok());
    REQUIRE((*n2)->start().ok());

    auto synced = (*n2)->cluster()->sync_from(part);
    REQUIRE(synced.ok());
    CHECK(synced.value() >= 5u);

    // Verify node 2 now has the data.
    auto got = (*n2)->store().get(tenant, key);
    REQUIRE(got.ok());
    REQUIRE(got->has_value());
    CHECK_EQ(got->value().sequence, 5u);

    (*n1)->request_shutdown();
    (*n2)->request_shutdown();
    (*n1)->stop();
    (*n2)->stop();
    (*n1)->join();
    (*n2)->join();
}

TEST_CASE(cluster_quorum_unavailable_honest) {
    SteadyClock::instance().init();
    TempDir tmp;
    uint16_t p1 = find_free_port(), p2 = find_free_port(), p3 = find_free_port();

    // Only node 1 is up; with RF=3 and QUORUM, a write cannot reach quorum.
    auto n1 = Node::create(make_cluster_cfg(1, p1, tmp.str() + "\\n1", p1, p2, p3));
    REQUIRE(n1.ok());
    REQUIRE((*n1)->start().ok());

    uint64_t tenant = 100;
    std::string key = "q";
    uint32_t part = partition_hash(tenant, key) % 4;
    std::vector<uint64_t> ids = {1, 2, 3};
    uint64_t leader = ids[part % 3];
    while (leader != 1) {
        tenant++;
        part = partition_hash(tenant, key) % 4;
        leader = ids[part % 3];
    }

    Client::Config cc;
    cc.host = "127.0.0.1";
    cc.port = p1;
    auto cli = Client::connect(cc);
    REQUIRE(cli.ok());
    auto ar = cli->request(proto::AppendReq, proto::encode_append_request(make_event(tenant, key, 1)));
    REQUIRE(ar.ok());
    auto aresp = proto::decode_append_response(ar->body.data(), ar->body.size());
    REQUIRE(aresp.ok());
    // Quorum cannot be satisfied -> the node must NOT report success.
    CHECK(aresp->code != 0u);

    (*n1)->request_shutdown();
    (*n1)->stop();
    (*n1)->join();
}
