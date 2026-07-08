// Tiny zero-dependency test harness.
//
// The Phase 2 brief says "dependency-free beyond toml++", so we do NOT pull in
// GoogleTest/Catch2. This gives us CHECK/EXPECT-style assertions, a TEST()
// registration macro, and a main() that runs everything and reports a summary
// with a non-zero exit on failure (so CTest sees a red test).
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace hctest {

struct Case {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

// Per-run failure counter for the case currently executing.
inline int& current_failures() {
  static int f = 0;
  return f;
}

struct Registrar {
  Registrar(std::string name, std::function<void()> fn) {
    registry().push_back({std::move(name), std::move(fn)});
  }
};

}  // namespace hctest

#define HC_CONCAT_(a, b) a##b
#define HC_CONCAT(a, b) HC_CONCAT_(a, b)

// TEST(name) { ... body ... }
#define TEST(name)                                                        \
  static void name();                                                     \
  static ::hctest::Registrar HC_CONCAT(reg_, name){#name, name};          \
  static void name()

// Report a failure in the current case without aborting the whole run.
#define HC_FAIL(msg)                                                       \
  do {                                                                     \
    std::fprintf(stderr, "    FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
    ++::hctest::current_failures();                                        \
  } while (0)

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) HC_FAIL("CHECK(" #cond ")");                              \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    auto va_ = (a);                                                        \
    auto vb_ = (b);                                                        \
    if (!(va_ == vb_)) HC_FAIL("CHECK_EQ(" #a ", " #b ")");                \
  } while (0)

#ifndef HC_NO_MAIN
int main() {
  int failed_cases = 0;
  for (auto& c : ::hctest::registry()) {
    ::hctest::current_failures() = 0;
    std::printf("[ RUN  ] %s\n", c.name.c_str());
    c.fn();
    if (::hctest::current_failures() == 0) {
      std::printf("[  OK  ] %s\n", c.name.c_str());
    } else {
      std::printf("[ FAIL ] %s (%d checks failed)\n", c.name.c_str(),
                  ::hctest::current_failures());
      ++failed_cases;
    }
  }
  std::printf("\n%zu cases, %d failed\n", ::hctest::registry().size(),
              failed_cases);
  return failed_cases == 0 ? 0 : 1;
}
#endif
