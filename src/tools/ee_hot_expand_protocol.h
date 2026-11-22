#pragma once

#include <istream>
#include <string>

// The Python supervisor may write CRLF on Windows while the native runner
// deliberately reads a binary response file. std::getline removes '\n'
// but retains '\r'. Normalize both lines, including the request number,
// before checking the sequence; a failed comparison must never resume guest
// execution with an unverified translation library.
namespace hot_expand
{
inline bool parse_response(
    std::istream& input,
    const std::string& expected_sequence,
    std::string* result,
    std::string* failure)
{
    bool valid = false;
    if (result != nullptr && failure != nullptr)
    {
        result->clear();
        failure->clear();

        std::string received;
        std::string value;
        if (!std::getline(input, received) || !std::getline(input, value))
        {
            *failure = "malformed hot-expand response";
        }
        else
        {
            if (!received.empty() && received.back() == '\r')
            {
                received.pop_back();
            }
            if (!value.empty() && value.back() == '\r')
            {
                value.pop_back();
            }

            if (received != expected_sequence || value.empty())
            {
                *failure = "hot-expand response sequence mismatch or empty result";
            }
            else
            {
                *result = value;
                valid = true;
            }
        }
    }
    return valid;
}
}
