#include "ee_hot_expand_protocol.h"

#include <cstdio>
#include <sstream>
#include <string>

bool expect_response(
    const char* source,
    const char* expected_sequence,
    bool expected_success,
    const char* expected_result,
    const char* expected_error)
{
    bool passed = false;
    std::istringstream input(source);
    std::string result;
    std::string failure;
    const bool valid = hot_expand::parse_response(
        input, expected_sequence, &result, &failure);
    passed = valid == expected_success &&
        result == expected_result && failure == expected_error;
    if (!passed)
    {
        std::fprintf(stderr,
            "hot expand response test failed: sequence=%s "
            "valid=%u result=%s error=%s\n",
            expected_sequence, valid ? 1u : 0u,
            result.c_str(), failure.c_str());
    }
    return passed;
}

int main()
{
    bool passed = true;

    passed = expect_response(
        "1\nC:\\Dev\\PS2\\game-translated-live-0001.dll\n",
        "1", true, "C:\\Dev\\PS2\\game-translated-live-0001.dll",
        "") && passed;

    // Python's Path.write_text() previously translated LF to CRLF on
    // Windows, leaving the request sequence as "1\r" in the binary reader.
    passed = expect_response(
        "1\r\nC:\\Dev\\PS2\\game-translated-live-0001.dll\r\n",
        "1", true, "C:\\Dev\\PS2\\game-translated-live-0001.dll",
        "") && passed;

    passed = expect_response(
        "2\r\nSTOP\r\n",
        "2", true, "STOP", "") && passed;

    passed = expect_response(
        "3\r\nERROR: IR verification failed\r\n",
        "3", true, "ERROR: IR verification failed", "") && passed;

    // Never accept an old generation or incomplete response.
    passed = expect_response(
        "1\r\nC:\\Dev\\PS2\\game-translated-live-0001.dll\r\n",
        "2", false, "", "hot-expand response sequence mismatch or empty result"
    ) && passed;

    passed = expect_response(
        "4\r\n",
        "4", false, "", "malformed hot-expand response"
    ) && passed;

    passed = expect_response(
        "5\r\n\r\n",
        "5", false, "", "hot-expand response sequence mismatch or empty result"
    ) && passed;

    if (passed)
    {
        std::puts("ee_hot_expand_protocol: PASS");
    }
    return passed ? 0 : 1;
}
