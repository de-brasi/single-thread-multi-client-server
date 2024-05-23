#pragma once

// A ~100 line test harness. The project deliberately has no external
// dependencies, so building and running the tests never needs more than a
// compiler and make.

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

inline int g_tests_run = 0;
inline int g_tests_failed = 0;
inline int g_failures_in_current_test = 0;

inline std::string ToString(const std::string& value) { return "\"" + value + "\""; }
inline std::string ToString(bool value) { return value ? "true" : "false"; }

template <typename T>
std::string ToString(const T& value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

template <typename T>
std::string ToString(const std::vector<T>& values) {
    std::string result = "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            result += ", ";
        }
        result += ToString(values[i]);
    }
    return result + "]";
}

inline void ReportFailure(const char* file, int line, const std::string& message) {
    std::printf("         %s:%d\n         %s\n", file, line, message.c_str());
    ++g_failures_in_current_test;
}

inline void RunTest(const char* name, void (*test_function)()) {
    ++g_tests_run;
    g_failures_in_current_test = 0;
    std::printf("[ RUN    ] %s\n", name);
    std::fflush(stdout);

    test_function();

    if (g_failures_in_current_test == 0) {
        std::printf("[     OK ] %s\n", name);
    } else {
        ++g_tests_failed;
        std::printf("[ FAILED ] %s (%d check(s) failed)\n", name, g_failures_in_current_test);
    }
    std::fflush(stdout);
}

inline int Summary() {
    std::printf("\n%d test(s) run, %d failed.\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}

}  // namespace testing

#define RUN_TEST(test_function) testing::RunTest(#test_function, test_function)

#define CHECK_TRUE(expression)                                                          \
    do {                                                                                \
        if (!(expression)) {                                                            \
            testing::ReportFailure(__FILE__, __LINE__,                                  \
                                   std::string("expected to be true: ") + #expression); \
        }                                                                               \
    } while (false)

#define CHECK_FALSE(expression)                                                          \
    do {                                                                                 \
        if ((expression)) {                                                              \
            testing::ReportFailure(__FILE__, __LINE__,                                   \
                                   std::string("expected to be false: ") + #expression); \
        }                                                                                \
    } while (false)

#define CHECK_EQ(actual, expected)                                                          \
    do {                                                                                    \
        const auto& check_actual = (actual);                                                \
        const auto& check_expected = (expected);                                            \
        if (!(check_actual == check_expected)) {                                            \
            testing::ReportFailure(__FILE__, __LINE__,                                      \
                                   std::string(#actual) + " is " +                          \
                                       testing::ToString(check_actual) + ", expected " +    \
                                       testing::ToString(check_expected));                  \
        }                                                                                   \
    } while (false)
