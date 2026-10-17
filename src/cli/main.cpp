#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/clock.hpp"
#include "net/client.hpp"
#include "protocol/protocol.hpp"
#include "storage/record.hpp"

using namespace ast;

namespace {

void print_usage() {
    printf("usage: asterion-cli <host> <port> <command> [args]\n");
    printf("commands:\n");
    printf("  ping\n");
    printf("  status\n");
    printf("  append <tenant> <key> <sequence> <payload> [event_id_hex]\n");
    printf("  delete <tenant> <key> <sequence> [event_id_hex]\n");
    printf("  get <tenant> <key>\n");
    printf("  scan <tenant> <from_ts_us> <to_ts_us> [max_results]\n");
    printf("  shutdown\n");
}

Result<Client> connect(int argc, char** argv, int* used) {
    if (argc < 3) return Error(Err::InvalidArgument, "host and port required");
    Client::Config c;
    c.host = argv[0];
    c.port = (uint16_t)atoi(argv[1]);
    *used = 2;
    return Client::connect(c);
}

void print_event(const EventRecord& rec) {
    printf("tenant=%llu key=%s seq=%llu ts=%llu deleted=%d event_id=%s payload=%zu bytes\n",
           (unsigned long long)rec.tenant_id, rec.key.c_str(),
           (unsigned long long)rec.sequence, (unsigned long long)rec.timestamp,
           rec.deleted ? 1 : 0, event_id_to_hex(rec.event_id).c_str(), rec.payload.size());
}

} // namespace

int main(int argc, char** argv) {
    SteadyClock::instance().init();

    if (argc < 4) {
        print_usage();
        return 2;
    }

    const char* host = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);
    const char* cmd = argv[3];

    Client::Config cc;
    cc.host = host;
    cc.port = port;
    auto client = Client::connect(cc);
    if (!client.ok()) {
        fprintf(stderr, "connect error: %s\n", client.error().to_string().c_str());
        return 1;
    }

    std::string command = cmd;
    if (command == "ping") {
        auto r = client->request(proto::Ping, Buffer());
        if (!r.ok()) { fprintf(stderr, "error: %s\n", r.error().to_string().c_str()); return 1; }
        printf("pong\n");
    } else if (command == "status") {
        auto r = client->request(proto::StatusReq, Buffer());
        if (!r.ok()) { fprintf(stderr, "error: %s\n", r.error().to_string().c_str()); return 1; }
        printf("%s\n", std::string(r->body.begin(), r->body.end()).c_str());
    } else if (command == "append" || command == "delete") {
        if (argc < 8) {
            fprintf(stderr, "usage: asterion-cli <host> <port> %s <tenant> <key> <seq> <payload> [event_id]\n",
                    command.c_str());
            return 2;
        }
        EventRecord rec;
        rec.tenant_id = (uint64_t)strtoull(argv[4], nullptr, 10);
        rec.key = argv[5];
        rec.sequence = (uint64_t)strtoull(argv[6], nullptr, 10);
        rec.payload.assign(argv[7], argv[7] + strlen(argv[7]));
        rec.deleted = (command == "delete");
        if (command == "delete") rec.payload.clear();
        if (argc >= 9) {
            auto id = event_id_from_hex(argv[8]);
            if (!id.ok()) { fprintf(stderr, "bad event_id: %s\n", argv[8]); return 2; }
            rec.event_id = id.value();
        } else {
            rec.event_id = make_random_event_id();
        }

        auto req = proto::encode_append_request(rec);
        auto r = client->request(proto::AppendReq, req);
        if (!r.ok()) { fprintf(stderr, "error: %s\n", r.error().to_string().c_str()); return 1; }
        if (r->type == proto::ErrorResp) {
            fprintf(stderr, "server error\n");
            return 1;
        }
        auto resp = proto::decode_append_response(r->body.data(), r->body.size());
        if (!resp.ok()) { fprintf(stderr, "bad response\n"); return 1; }
        if (resp->code == 0) printf("ok seq=%llu\n", (unsigned long long)resp->sequence);
        else if (resp->code == 1) printf("duplicate seq=%llu\n", (unsigned long long)resp->sequence);
        else printf("error code=%u\n", resp->code);
    } else if (command == "get") {
        if (argc < 6) {
            fprintf(stderr, "usage: asterion-cli <host> <port> get <tenant> <key>\n");
            return 2;
        }
        proto::GetRequest req;
        req.tenant = (uint64_t)strtoull(argv[4], nullptr, 10);
        req.key = argv[5];
        auto body = proto::encode_get_request(req);
        auto r = client->request(proto::GetReq, body);
        if (!r.ok()) { fprintf(stderr, "error: %s\n", r.error().to_string().c_str()); return 1; }
        if (r->type == proto::ErrorResp) { fprintf(stderr, "server error\n"); return 1; }
        auto code = proto::decode_get_response_code(r->body.data(), r->body.size());
        if (!code.ok()) { fprintf(stderr, "bad response\n"); return 1; }
        if (*code == 1) { printf("not found\n"); return 0; }
        if (*code == 2) { printf("deleted (tombstone)\n"); return 0; }
        auto ev = proto::decode_get_response_event(r->body.data(), r->body.size(), 1 << 20, 1 << 20);
        if (!ev.ok()) { fprintf(stderr, "bad response\n"); return 1; }
        print_event(ev.value());
    } else if (command == "scan") {
        if (argc < 7) {
            fprintf(stderr, "usage: asterion-cli <host> <port> scan <tenant> <from_ts> <to_ts> [max]\n");
            return 2;
        }
        proto::ScanRequest req;
        req.tenant = (uint64_t)strtoull(argv[4], nullptr, 10);
        req.from_ts = (uint64_t)strtoull(argv[5], nullptr, 10);
        req.to_ts = (uint64_t)strtoull(argv[6], nullptr, 10);
        req.max_results = argc >= 8 ? (uint32_t)atoi(argv[7]) : 1000;
        auto body = proto::encode_scan_request(req);

        uint64_t n = 0;
        auto r = client->request_stream(proto::ScanReq, body, proto::ScanEnd,
                                        [&](const proto::Frame& f) {
            if (f.type == proto::ScanResp) {
                auto ev = proto::decode_scan_response(f.body.data(), f.body.size(), 1 << 20, 1 << 20);
                if (ev.ok()) { print_event(ev.value()); n++; }
            } else if (f.type == proto::ScanEnd) {
                auto reason = proto::decode_scan_end(f.body.data(), f.body.size());
                if (reason.ok()) {
                    const char* rname = *reason == 0 ? "complete" : (*reason == 1 ? "limit" : "cancelled");
                    printf("-- scan end (%s), %llu result(s)\n", rname, (unsigned long long)n);
                }
            }
            return true;
        });
        if (!r.ok()) { fprintf(stderr, "error: %s\n", r.error().to_string().c_str()); return 1; }
    } else if (command == "shutdown") {
        auto r = client->request(proto::ShutdownReq, Buffer());
        if (!r.ok()) { fprintf(stderr, "error: %s\n", r.error().to_string().c_str()); return 1; }
        printf("shutdown sent\n");
    } else {
        fprintf(stderr, "unknown command: %s\n", command.c_str());
        return 2;
    }

    return 0;
}
