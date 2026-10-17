#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace ast {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
};

inline const char* log_level_name(LogLevel l) {
    switch (l) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO ";
    case LogLevel::Warn: return "WARN ";
    case LogLevel::Error: return "ERROR";
    default: return "OFF  ";
    }
}

// Minimal thread-safe logger. Writes to stderr and, optionally, a file.
// Never throws. All logging goes through configured directories only.
class Logger {
public:
    static Logger& instance();

    void init(LogLevel level, std::string file_path, bool also_stderr = true);
    void shutdown();

    LogLevel level() const { return level_; }
    void set_level(LogLevel l) { level_ = l; }

    void log(LogLevel l, std::string_view msg);

    bool enabled(LogLevel l) const { return l >= level_ && level_ != LogLevel::Off; }

private:
    Logger() = default;
    void write_line(LogLevel l, std::string_view msg);

    LogLevel level_ = LogLevel::Info;
    bool stderr_ = true;
    std::mutex mu_;
    void* file_ = nullptr; // FILE* but avoid including <cstdio> in header.
    std::string path_;
};

} // namespace ast

#define AST_LOG(lvl, msg)                                                     \
    do {                                                                      \
        auto& __lg = ::ast::Logger::instance();                               \
        if (__lg.enabled(::ast::LogLevel::lvl))                               \
            __lg.log(::ast::LogLevel::lvl, msg);                              \
    } while (0)

#define AST_LOG_INFO(msg) AST_LOG(Info, msg)
#define AST_LOG_WARN(msg) AST_LOG(Warn, msg)
#define AST_LOG_ERROR(msg) AST_LOG(Error, msg)
#define AST_LOG_DEBUG(msg) AST_LOG(Debug, msg)
