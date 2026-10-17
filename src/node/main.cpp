#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include <windows.h>

#include "common/clock.hpp"
#include "common/config.hpp"
#include "common/logging.hpp"
#include "core/node.hpp"

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI ctrl_handler(DWORD type) {
    (void)type;
    g_stop.store(true);
    return TRUE; // signal handled; node performs graceful shutdown in main loop
}

} // namespace

int main(int argc, char** argv) {
    ast::SteadyClock::instance().init();

    if (argc < 2) {
        fprintf(stderr, "usage: asterion-node <config-file>\n");
        return 2;
    }

    std::string config_path = argv[1];
    auto cfg = ast::Config::load(config_path);
    if (!cfg.ok()) {
        fprintf(stderr, "config error: %s\n", cfg.error().to_string().c_str());
        return 2;
    }

    // Resolve relative paths against the config file's directory.
    std::string config_dir = std::filesystem::path(config_path).parent_path().string();
    cfg->resolve_paths(config_dir);

    ast::Logger::instance().init(cfg->parse_log_level(), cfg->log_file, true);
    AST_LOG_INFO("asterion-node starting (node_id=" + std::to_string(cfg->node_id) + ")");

    auto node = ast::Node::create(cfg.value());
    if (!node.ok()) {
        fprintf(stderr, "node init error: %s\n", node.error().to_string().c_str());
        AST_LOG_ERROR("node init error: " + node.error().to_string());
        return 2;
    }

    auto start_err = (*node)->start();
    if (!start_err.ok()) {
        fprintf(stderr, "node start error: %s\n", start_err.to_string().c_str());
        AST_LOG_ERROR("node start error: " + start_err.to_string());
        return 2;
    }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    // Main loop: wait for a shutdown request (CLI or Ctrl+C).
    while (!g_stop.load() && !(*node)->shutdown_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    AST_LOG_INFO("shutdown requested; draining...");
    (*node)->request_shutdown();
    (*node)->stop();
    (*node)->join();

    ast::Logger::instance().shutdown();
    AST_LOG_INFO("asterion-node stopped");
    return 0;
}
