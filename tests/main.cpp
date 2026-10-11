#include "test.hpp"

#include <cstring>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#include <cstdlib>
#endif

int main(int argc, char** argv) {
#if defined(_MSC_VER) && defined(_DEBUG)
    // Automated tests must fail into the log, never open a blocking desktop dialog.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
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
