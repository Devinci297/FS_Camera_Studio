#include "test.hpp"

#include <cstring>

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int passed = 0, failed = 0;
    for (const auto& c : fscs_test::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        try {
            c.fn();
            ++passed;
        } catch (const fscs_test::Failure& f) {
            ++failed;
            std::printf("FAIL %s\n  %s\n", c.name, f.message.c_str());
        } catch (const std::exception& e) {
            ++failed;
            std::printf("FAIL %s\n  unexpected exception: %s\n", c.name, e.what());
        }
    }
    std::printf("%d passed, %d failed, %d checks\n", passed, failed, fscs_test::checks());
    return failed == 0 ? 0 : 1;
}
