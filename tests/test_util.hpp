#pragma once

#include <filesystem>
#include <string>

#include "test_framework.hpp"

namespace ast {
namespace test {

// RAII temp directory under the system temp dir, removed on destruction.
class TempDir {
public:
    TempDir() {
        static int counter = 0;
        auto base = std::filesystem::temp_directory_path();
        path_ = base / ("asterion_test_" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }
    std::string str() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

} // namespace test
} // namespace ast
