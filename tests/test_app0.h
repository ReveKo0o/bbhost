// The game files some tests read: under the dump BBHOST_APP0 names (the folder
// that contains dvdroot_ps4). Without it, or without the file, the test skips
// (exit 77, ctest's SKIP_RETURN_CODE) - no game file is ever in the repository.
#pragma once
#include <cstdlib>
#include <string>

inline std::string test_app0_file(const char* relative) {
    const char* app0 = std::getenv("BBHOST_APP0");
    if (!app0 || !*app0) return std::string();  // no dump named: the test skips
    return std::string(app0) + "/" + relative;
}
constexpr int kTestSkip = 77;
