#pragma once

#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ast {
namespace test {

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct TestEntry {
    std::string name;
    std::function<void()> fn;
    bool stress = false;
};

struct Registry {
    std::vector<TestEntry> tests;
    static Registry& instance() {
        static Registry r;
        return r;
    }
};

struct Registration {
    Registration(const char* name, std::function<void()> fn, bool stress) {
        Registry::instance().tests.push_back({name, std::move(fn), stress});
    }
};

struct State {
    int checks = 0;
    int failures = 0;
    const char* current = "";
};
State& state();

inline void report_failure(const std::string& msg) {
    state().failures++;
    fprintf(stderr, "    FAIL [%s] %s\n", state().current, msg.c_str());
}

int run(const std::string& filter);

} // namespace test
} // namespace ast

#define AST_STRINGIFY2(x) #x
#define AST_STRINGIFY(x) AST_STRINGIFY2(x)

#define TEST_CASE(name)                                                        \
    static void name();                                                        \
    static ::ast::test::Registration ast_reg_##name(#name, name, false);       \
    static void name()

#define TEST_CASE_STRESS(name)                                                 \
    static void name();                                                        \
    static ::ast::test::Registration ast_reg_##name(#name, name, true);        \
    static void name()

#define CHECK(cond)                                                            \
    do {                                                                       \
        ::ast::test::state().checks++;                                         \
        if (!(cond))                                                           \
            ::ast::test::report_failure("CHECK failed: " #cond " at " __FILE__ \
                                        ":" AST_STRINGIFY(__LINE__));          \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        ::ast::test::state().checks++;                                         \
        auto va = (a);                                                         \
        auto vb = (b);                                                         \
        if (!(va == vb))                                                       \
            ::ast::test::report_failure("CHECK_EQ failed: " #a " == " #b       \
                                        " at " __FILE__ ":" AST_STRINGIFY(__LINE__)); \
    } while (0)

#define CHECK_ERR(expr)                                                        \
    do {                                                                       \
        auto __r = (expr);                                                     \
        ::ast::test::state().checks++;                                         \
        if (__r.ok())                                                          \
            ::ast::test::report_failure("expected error: " #expr " at " __FILE__ \
                                        ":" AST_STRINGIFY(__LINE__));          \
    } while (0)

#define REQUIRE(cond)                                                          \
    do {                                                                       \
        ::ast::test::state().checks++;                                         \
        if (!(cond)) {                                                         \
            ::ast::test::report_failure("REQUIRE failed: " #cond " at " __FILE__ \
                                        ":" AST_STRINGIFY(__LINE__));          \
            return;                                                            \
        }                                                                      \
    } while (0)
