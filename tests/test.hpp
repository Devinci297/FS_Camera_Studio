#pragma once
// Minimal self-registering test harness (no third-party dependency).

#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace fscs_test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

struct Failure {
    std::string message;
};

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline int& checks() {
    static int n = 0;
    return n;
}

[[noreturn]] inline void fail(const char* file, int line, const std::string& what) {
    std::ostringstream s;
    s << file << ":" << line << ": " << what;
    throw Failure{s.str()};
}

}  // namespace fscs_test

#define FSCS_CAT2(a, b) a##b
#define FSCS_CAT(a, b) FSCS_CAT2(a, b)
#define TEST_CASE(name)                                                                              \
    static void FSCS_CAT(test_fn_, __LINE__)();                                                      \
    static fscs_test::Registrar FSCS_CAT(test_reg_, __LINE__)(name, &FSCS_CAT(test_fn_, __LINE__)); \
    static void FSCS_CAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++fscs_test::checks();                                                   \
        if (!(cond)) fscs_test::fail(__FILE__, __LINE__, "CHECK(" #cond ")");    \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        ++fscs_test::checks();                                                                      \
        const auto& va_ = (a);                                                                      \
        const auto& vb_ = (b);                                                                      \
        if (!(va_ == vb_)) {                                                                        \
            std::ostringstream s_;                                                                  \
            s_ << "CHECK_EQ(" #a ", " #b ") got [" << va_ << "] vs [" << vb_ << "]";                \
            fscs_test::fail(__FILE__, __LINE__, s_.str());                                          \
        }                                                                                           \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                       \
    do {                                                                                            \
        ++fscs_test::checks();                                                                      \
        const double va_ = (a), vb_ = (b);                                                          \
        if (!(std::abs(va_ - vb_) <= (eps))) {                                                      \
            std::ostringstream s_;                                                                  \
            s_ << "CHECK_NEAR(" #a ", " #b ") got " << va_ << " vs " << vb_ << " (eps " << (eps) << ")"; \
            fscs_test::fail(__FILE__, __LINE__, s_.str());                                          \
        }                                                                                           \
    } while (0)
