#pragma once

#include <string>
#include <variant>
#include <optional>

namespace ast {

enum class Err {
    Ok = 0,
    NotFound,
    AlreadyExists,
    Conflict,
    Corrupt,
    IoError,
    InvalidArgument,
    OutOfMemory,
    LimitExceeded,
    Busy,
    Shutdown,
    Unsupported,
    NotLeader,
    QuorumUnavailable,
    TimedOut,
    Internal,
};

inline const char* err_name(Err e) {
    switch (e) {
    case Err::Ok: return "ok";
    case Err::NotFound: return "not_found";
    case Err::AlreadyExists: return "already_exists";
    case Err::Conflict: return "conflict";
    case Err::Corrupt: return "corrupt";
    case Err::IoError: return "io_error";
    case Err::InvalidArgument: return "invalid_argument";
    case Err::OutOfMemory: return "out_of_memory";
    case Err::LimitExceeded: return "limit_exceeded";
    case Err::Busy: return "busy";
    case Err::Shutdown: return "shutdown";
    case Err::Unsupported: return "unsupported";
    case Err::NotLeader: return "not_leader";
    case Err::QuorumUnavailable: return "quorum_unavailable";
    case Err::TimedOut: return "timed_out";
    case Err::Internal: return "internal";
    }
    return "unknown";
}

struct Error {
    Err code = Err::Ok;
    std::string message;

    Error() = default;
    Error(Err c, std::string msg = "") : code(c), message(std::move(msg)) {}

    bool ok() const { return code == Err::Ok; }
    explicit operator bool() const { return code == Err::Ok; }

    std::string to_string() const {
        if (message.empty()) return err_name(code);
        return std::string(err_name(code)) + ": " + message;
    }
};

inline Error ok() { return Error{}; }

// Result<T, Error>: a value or an error. No exceptions in the hot path.
template <typename T>
class Result {
public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Error err) : storage_(std::in_place_index<1>, std::move(err)) {}

    bool ok() const { return storage_.index() == 0; }
    explicit operator bool() const { return ok(); }

    T& value() { return std::get<0>(storage_); }
    const T& value() const { return std::get<0>(storage_); }
    T* operator->() { return &std::get<0>(storage_); }
    const T* operator->() const { return &std::get<0>(storage_); }
    T& operator*() { return std::get<0>(storage_); }
    const T& operator*() const { return std::get<0>(storage_); }

    const Error& error() const { return std::get<1>(storage_); }
    Err code() const { return std::get<1>(storage_).code; }

    T value_or(T fallback) const {
        return ok() ? std::get<0>(storage_) : std::move(fallback);
    }

private:
    std::variant<T, Error> storage_;
};

template <>
class Result<void> {
public:
    Result() : err_(Error{}) {}
    Result(Error err) : err_(std::move(err)) {}

    bool ok() const { return err_.ok(); }
    explicit operator bool() const { return ok(); }
    const Error& error() const { return err_; }
    Err code() const { return err_.code; }

private:
    Error err_;
};

template <typename T>
Result<T> make_result(T value) { return Result<T>(std::move(value)); }

} // namespace ast
