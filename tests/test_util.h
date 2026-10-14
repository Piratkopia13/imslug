// imslug tests — tiny shared harness.
//
// No external test framework: each suite is a plain main() that uses the
// TEST_CHECK macros, prints failures with file/line, and returns nonzero when
// anything failed (ctest-friendly, same shape as tests/test_imslug.cpp).
//
// Test assets live in tests/fonts/; the directory is injected by CMake as
// IMSLUG_TEST_FONT_DIR so suites do not depend on the working directory.

#pragma once

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS  // std::fopen is fine in tests
#endif

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#ifndef IMSLUG_TEST_FONT_DIR
#define IMSLUG_TEST_FONT_DIR "tests/fonts"
#endif

#define IMSLUG_TEST_FONT_PATH(_name) (IMSLUG_TEST_FONT_DIR "/" _name)

namespace imslug_test
{
    // Debug builds: MSVC's assert() pops a modal "Debug Error!" dialog and
    // blocks headless test runs (the debug suite hung on the stress test's
    // deliberate invariant poke). Report asserts to stderr instead, so a
    // violated internal invariant fails the process instead of waiting for a
    // click. No-op outside MSVC Debug.
    inline void SetAssertToStderr()
    {
#if defined(_MSC_VER) && defined(_DEBUG)
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    }

    // Static-init guard: every test translation unit that includes this header
    // gets the behavior before any test code runs.
    namespace detail
    {
        [[maybe_unused]] const bool kAssertToStderrSet = (SetAssertToStderr(), true);
    }  // namespace detail

    inline int& FailureCount()
    {
        static int count = 0;
        return count;
    }

    inline void ReportFailure(const char* file, int line, const char* format, ...)
    {
        ++FailureCount();
        std::printf("  FAIL %s(%d): ", file, line);
        va_list args;
        va_start(args, format);
        std::vprintf(format, args);
        va_end(args);
        std::printf("\n");
    }

    inline bool Check(bool condition, const char* expression, const char* file, int line)
    {
        if (!condition)
            ReportFailure(file, line, "%s", expression);
        return condition;
    }

    // Loads a whole file into a malloc'd buffer; returns nullptr on failure and
    // sets *outSize. Caller frees with std::free.
    inline unsigned char* LoadFile(const char* path, int* outSize)
    {
        std::FILE* file = std::fopen(path, "rb");
        if (file == nullptr)
            return nullptr;
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        if (size <= 0)
        {
            std::fclose(file);
            return nullptr;
        }
        unsigned char* data = (unsigned char*)std::malloc((size_t)size);
        if (data == nullptr)
        {
            std::fclose(file);
            return nullptr;
        }
        const size_t read = std::fread(data, 1, (size_t)size, file);
        std::fclose(file);
        if (read != (size_t)size)
        {
            std::free(data);
            return nullptr;
        }
        *outSize = (int)size;
        return data;
    }

    // Loads one of the committed test fonts by file name (tests/fonts/).
    inline unsigned char* LoadFontFile(const char* fileName, int* outSize)
    {
        char path[512];
        std::snprintf(path, sizeof(path), "%s/%s", IMSLUG_TEST_FONT_DIR, fileName);
        return LoadFile(path, outSize);
    }

    // Prints the summary and returns the process exit code.
    inline int Finish(const char* suiteName)
    {
        if (FailureCount() == 0)
        {
            std::printf("%s: OK\n", suiteName);
            return 0;
        }
        std::printf("%s: FAILED (%d failed checks)\n", suiteName, FailureCount());
        return 1;
    }
}  // namespace imslug_test

#define TEST_CHECK(_expr) ::imslug_test::Check((_expr) != false, #_expr, __FILE__, __LINE__)

#define TEST_CHECKF(_expr, ...)                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!((_expr) != false))                                                                                       \
            ::imslug_test::ReportFailure(__FILE__, __LINE__, __VA_ARGS__);                                             \
    } while (0)
