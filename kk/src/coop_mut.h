/*
MIT License

Copyright (c) 2026 cranebeaker and contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
// Adapted from coop-anything b877c9e, tools/mutants/coop_mut.h.
// Runtime-switchable mutation twins for the isolated KK test target.
//
// A "twin" is a deliberately broken variant of one piece of your code. A test that guards that code must fail when the
// twin is on, so a twin that does not make a test fail shows a test that proves nothing.
//
// Old way: one compile per twin (-DMY_TWIN_28 ...), so 40 twins meant 40 builds.
// This way: build the code under test ONCE with COOP_RUNTIME_MUT defined and choose the twin when the test program starts:
//
//     set COOP_MUT=add_wrong_operator && my_tests.exe        (environment variable, a name or a number)
//     my_tests.exe --mut add_wrong_operator                   (command line, if main calls coop_mut::init_from_args)
//
// In the code under test:
//
//     if (COOP_MUT_IF("add_wrong_operator")) return a - b;    // the broken variant
//     if (!COOP_MUT_IF("skip_range_check")) { checkRange(x); } // code the twin removes
//
// Without COOP_RUNTIME_MUT (your shipped build) COOP_MUT_IF(x) is the constant false: the broken variants are dead code that the
// optimiser deletes, nothing reads the environment, and nothing in the shipped binary can switch a twin on. Define
// COOP_RUNTIME_MUT only for the test builds of your libraries.
//
// One twin is active at a time. A name is any text without spaces; a number works too (COOP_MUT_IF(3) with COOP_MUT=3).
// The value "0" or an empty value means no twin. set() is for single-threaded test setup, call it before other threads start.
#pragma once

#if defined(COOP_RUNTIME_MUT) && !defined(KK_COOP_MUTATION_TEST)
#error Mutation switches are permitted only in the isolated KK mutation test target
#endif

#ifdef COOP_RUNTIME_MUT
#include <cstdlib>
#include <cstring>
#include <string>

namespace coop_mut {

inline std::string readEnv() {
#if defined(_MSC_VER)
    char* buf = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&buf, &len, "COOP_MUT") == 0 && buf) {
        out = buf;
        std::free(buf);
    }
    return out;
#else
    const char* e = std::getenv("COOP_MUT");
    return e ? std::string(e) : std::string();
#endif
}

inline std::string& slot() {
    static std::string v = readEnv();
    return v;
}

inline const std::string& current() { return slot(); }
inline void set(const std::string& value) { slot() = value; }
inline bool any() { return !slot().empty() && slot() != "0"; }
inline bool is(const char* name) { return any() && slot() == name; }
inline bool is(int number) { return any() && slot() == std::to_string(number); }

// Optional: let a test main accept `--mut NAME` or `--mut=NAME`. The pair is blanked out in argv so a positional argument parser
// does not see it. Returns the active twin.
inline const std::string& init_from_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mut") == 0 && i + 1 < argc) {
            set(argv[i + 1]);
            argv[i][0] = '\0';
            argv[i + 1][0] = '\0';
            break;
        }
        if (std::strncmp(argv[i], "--mut=", 6) == 0) {
            set(argv[i] + 6);
            argv[i][0] = '\0';
            break;
        }
    }
    return current();
}

}  // namespace coop_mut

#define COOP_MUT_IF(x) (::coop_mut::is(x))
#define COOP_MUT_ANY() (::coop_mut::any())
#else
#define COOP_MUT_IF(x) (false)
#define COOP_MUT_ANY() (false)
#endif
