#include "common/logging.hpp"

#include <cstdio>

#include "common/clock.hpp"

namespace ast {

Logger& Logger::instance() {
    static Logger l;
    return l;
}

void Logger::init(LogLevel level, std::string file_path, bool also_stderr) {
    std::lock_guard<std::mutex> g(mu_);
    level_ = level;
    stderr_ = also_stderr;
    path_ = std::move(file_path);
    if (!path_.empty()) {
        FILE* f = nullptr;
        if (fopen_s(&f, path_.c_str(), "ab") == 0) {
            file_ = f;
        }
    }
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> g(mu_);
    if (file_) {
        fflush(static_cast<FILE*>(file_));
        fclose(static_cast<FILE*>(file_));
        file_ = nullptr;
    }
}

void Logger::log(LogLevel l, std::string_view msg) {
    if (!enabled(l)) return;
    write_line(l, msg);
}

void Logger::write_line(LogLevel l, std::string_view msg) {
    char ts[64];
    uint64_t us = wall_time_us();
    uint64_t s = us / 1'000'000;
    uint64_t micro = us % 1'000'000;
    snprintf(ts, sizeof(ts), "%llu.%06llu", (unsigned long long)s, (unsigned long long)micro);

    std::lock_guard<std::mutex> g(mu_);
    if (stderr_) {
        fprintf(stderr, "[%s] %s %.*s\n", ts, log_level_name(l), (int)msg.size(), msg.data());
    }
    if (file_) {
        FILE* f = static_cast<FILE*>(file_);
        fprintf(f, "[%s] %s %.*s\n", ts, log_level_name(l), (int)msg.size(), msg.data());
        fflush(f);
    }
}

} // namespace ast
