#include "test_framework.hpp"

namespace ast {
namespace test {

State& state() {
    static State s;
    return s;
}

int run(const std::string& filter) {
    Registry& reg = Registry::instance();
    int total = 0;
    int passed = 0;
    int failed = 0;

    for (auto& t : reg.tests) {
        bool selected = false;
        if (filter == "all") selected = true;
        else if (filter == "fast") selected = !t.stress;
        else if (filter == "stress") selected = t.stress;
        else selected = (t.name.find(filter) != std::string::npos);

        if (!selected) continue;
        total++;
        state().current = t.name.c_str();
        state().checks = 0;
        state().failures = 0;

        printf("[ RUN ] %s\n", t.name.c_str());
        fflush(stdout);
        try {
            t.fn();
        } catch (const Failure& f) {
            state().failures++;
            fprintf(stderr, "    EXCEPTION: %s\n", f.what());
        } catch (const std::exception& e) {
            state().failures++;
            fprintf(stderr, "    EXCEPTION(std): %s\n", e.what());
        } catch (...) {
            state().failures++;
            fprintf(stderr, "    EXCEPTION(unknown)\n");
        }

        if (state().failures == 0) {
            passed++;
            printf("[  OK ] %s (%d checks)\n", t.name.c_str(), state().checks);
        } else {
            failed++;
            printf("[FAIL ] %s (%d checks, %d failures)\n", t.name.c_str(),
                   state().checks, state().failures);
        }
        fflush(stdout);
    }

    printf("\n%d test(s), %d passed, %d failed\n", total, passed, failed);
    return failed == 0 ? 0 : 1;
}

} // namespace test
} // namespace ast
