#include "common/platform/file.hpp"

#include <windows.h>

#include <algorithm>

namespace ast {

namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
} // namespace

File::File(File&& o) noexcept : h_(o.h_), path_(std::move(o.path_)) {
    o.h_ = nullptr;
}

File& File::operator=(File&& o) noexcept {
    if (this != &o) {
        close();
        h_ = o.h_;
        path_ = std::move(o.path_);
        o.h_ = nullptr;
    }
    return *this;
}

void File::close() {
    if (is_open()) {
        CloseHandle(h_);
        h_ = nullptr;
    }
}

Result<File> File::create(const std::string& path) {
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return Error(Err::IoError, "CreateFileW failed for " + path);
    return File(h, path);
}

Result<File> File::open_rw(const std::string& path) {
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return Error(Err::IoError, "CreateFileW failed for " + path);
    return File(h, path);
}

Result<File> File::open_read(const std::string& path) {
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return Error(Err::IoError, "CreateFileW failed for " + path);
    return File(h, path);
}

Result<uint64_t> File::write(const uint8_t* data, uint64_t len) {
    // Track cursor position explicitly via OVERLAPPED with offset = current size.
    uint64_t sz = 0;
    auto s = size();
    if (!s.ok()) return s.error();
    sz = s.value();

    OVERLAPPED ov{};
    ov.Offset = (DWORD)(sz & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)(sz >> 32);

    DWORD written = 0;
    if (!WriteFile(h_, data, (DWORD)len, &written, &ov)) {
        return Error(Err::IoError, "WriteFile failed");
    }
    return (uint64_t)written;
}

Result<uint64_t> File::write_at(const uint8_t* data, uint64_t len, uint64_t offset) {
    OVERLAPPED ov{};
    ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)(offset >> 32);
    DWORD written = 0;
    if (!WriteFile(h_, data, (DWORD)len, &written, &ov)) {
        return Error(Err::IoError, "WriteFile (at) failed");
    }
    return (uint64_t)written;
}

Result<uint64_t> File::read(uint8_t* dst, uint64_t len, uint64_t offset) {
    OVERLAPPED ov{};
    ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)(offset >> 32);
    DWORD got = 0;
    if (!ReadFile(h_, dst, (DWORD)len, &got, &ov)) {
        DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF) return (uint64_t)0;
        return Error(Err::IoError, "ReadFile failed");
    }
    return (uint64_t)got;
}

Error File::flush() {
    if (!is_open()) return ok();
    if (!FlushFileBuffers(h_))
        return Error(Err::IoError, "FlushFileBuffers failed");
    return ok();
}

Result<uint64_t> File::size() {
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h_, &sz))
        return Error(Err::IoError, "GetFileSizeEx failed");
    return (uint64_t)sz.QuadPart;
}

Error File::truncate(uint64_t new_size) {
    // Move the file pointer (which truncation uses) to new_size, then set EOF.
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)new_size;
    if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN))
        return Error(Err::IoError, "SetFilePointerEx failed");
    if (!SetEndOfFile(h_))
        return Error(Err::IoError, "SetEndOfFile failed");
    return ok();
}

// ---------------------------------------------------------------------------
// Directory helpers
// ---------------------------------------------------------------------------

Error make_dirs(const std::string& path) {
    if (path.empty()) return ok();
    std::wstring w = widen(path);
    // Ensure trailing separator handling; build up incrementally.
    std::wstring cur;
    size_t i = 0;
    // Skip drive/UNC prefix.
    if (w.size() >= 2 && w[1] == L':') {
        cur += w.substr(0, 3);
        i = 3;
    } else if (w.size() >= 2 && w[0] == L'\\' && w[1] == L'\\') {
        i = 2; // UNC server/share handled by CreateDirectory best-effort
    }
    for (; i <= w.size(); ++i) {
        if (i == w.size() || w[i] == L'\\' || w[i] == L'/') {
            if (!cur.empty()) {
                CreateDirectoryW(cur.c_str(), nullptr); // ignore EEXIST
            }
            if (i < w.size()) cur += w[i];
        } else {
            cur += w[i];
        }
    }
    DWORD attr = GetFileAttributesW(w.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        // Final attempt.
        if (!CreateDirectoryW(w.c_str(), nullptr)) {
            DWORD e = GetLastError();
            if (e != ERROR_ALREADY_EXISTS)
                return Error(Err::IoError, "cannot create directory " + path);
        }
    }
    return ok();
}

Error flush_directory(const std::string& dir_path) {
    HANDLE h = CreateFileW(widen(dir_path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // Directory flushing is best-effort; not fatal.
        return ok();
    }
    bool okflush = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    return okflush ? ok() : Error(Err::IoError, "FlushFileBuffers(dir) failed");
}

bool path_exists(const std::string& path) {
    DWORD attr = GetFileAttributesW(widen(path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

Error remove_file(const std::string& path) {
    if (!DeleteFileW(widen(path).c_str())) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) return ok();
        return Error(Err::IoError, "DeleteFileW failed for " + path);
    }
    return ok();
}

std::vector<std::string> list_files(const std::string& dir, const std::string& ext) {
    std::vector<std::string> out;
    std::wstring pattern = widen(dir);
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
    pattern += L'*';
    if (!ext.empty()) pattern += widen(ext);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(narrow(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

uint64_t disk_free_bytes(const std::string& path) {
    ULARGE_INTEGER free, total, totalFree;
    if (!GetDiskFreeSpaceExW(widen(path).c_str(), &free, &total, &totalFree)) return 0;
    return free.QuadPart;
}

uint64_t disk_total_bytes(const std::string& path) {
    ULARGE_INTEGER free, total, totalFree;
    if (!GetDiskFreeSpaceExW(widen(path).c_str(), &free, &total, &totalFree)) return 0;
    return total.QuadPart;
}

} // namespace ast
