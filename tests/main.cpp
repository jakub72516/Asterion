#include <cstdio>
#include <cstring>
#include <string>

#include "test_framework.hpp"

int main(int argc, char** argv) {
    std::string filter = "fast";
    if (argc > 1) {
        std::string a = argv[1];
        if (a == "--all") filter = "all";
        else if (a == "--stress") filter = "stress";
        else if (a == "--fast") filter = "fast";
        else if (a == "--list") {
            for (auto& t : ast::test::Registry::instance().tests)
                printf("%s%s\n", t.name.c_str(), t.stress ? "  [stress]" : "");
            return 0;
        } else {
            filter = a; // substring match
        }
    }
    return ast::test::run(filter);
}
