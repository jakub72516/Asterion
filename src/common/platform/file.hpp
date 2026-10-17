#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/result.hpp"

namespace ast {

// Windows file abstraction. Buffered I/O (the OS file cache) with explicit
// FlushFileBuffers for durability. See docs/on-disk-format.md for the exact
// durability guarantees on Windows.
class File {
public:
    File() = default;
    ~File() { close(); }

    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& o) noexcept;
    File& operator=(File&& o) noexcept;

    // Create (or truncate) a file for read/write.
    static Result<File> create(const std::string& path);
    // Open an existing file for read/write (no truncation).
    static Result<File> open_rw(const std::string& path);
    // Open an existing file read-only.
    static Result<File> open_read(const std::string& path);

    bool is_open() const { return h_ != nullptr; }
    void close();

    // Append bytes at the current end-of-file cursor (write-through optional).
    Result<uint64_t> write(const uint8_t* data, uint64_t len);

    // Write at an explicit offset (used to rewrite segment headers).
    Result<uint64_t> write_at(const uint8_t* data, uint64_t len, uint64_t offset);

    // Read at an explicit offset (used for scans and recovery). Returns bytes read.
    Result<uint64_t> read(uint8_t* dst, uint64_t len, uint64_t offset);

    // Flush file data+metadata to stable storage (FlushFileBuffers).
    Error flush();

    // Current file size in bytes.
    Result<uint64_t> size();

    // Truncate to a given size (used to isolate corrupted trailing records).
    Error truncate(uint64_t new_size);

    const std::string& path() const { return path_; }

private:
    explicit File(void* h, std::string path) : h_(h), path_(std::move(path)) {}

    void* h_ = nullptr; // Windows HANDLE; nullptr means closed
    std::string path_;
};

// Directory helpers.
Error make_dirs(const std::string& path);            // mkdir -p
Error flush_directory(const std::string& dir_path);  // durable directory entry
bool path_exists(const std::string& path);
Error remove_file(const std::string& path);
std::vector<std::string> list_files(const std::string& dir, const std::string& ext);

// Free space (bytes) available on the volume containing `path`. Returns 0 on
// failure (callers treat 0 as "unknown", not "full").
uint64_t disk_free_bytes(const std::string& path);
uint64_t disk_total_bytes(const std::string& path);

} // namespace ast
