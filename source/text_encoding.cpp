#include "text_encoding.h"
#include <windows.h>
#include <limits>

namespace TextEncoding {

bool WideToUtf8(const wchar_t* input, std::string& output)
{
    output.clear();
    if (!input)
        return false;

    // The input is NUL-terminated. Passing -1 avoids a separate length scan;
    // the size returned by WideCharToMultiByte includes the terminator.
    int outputLength = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input, -1,
        nullptr, 0, nullptr, nullptr);
    if (outputLength <= 0)
        return false;

    output.resize(static_cast<std::size_t>(outputLength));
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, input, -1,
            output.data(), outputLength, nullptr, nullptr) != outputLength) {
        output.clear();
        return false;
    }

    output.pop_back();
    return true;
}

bool Utf8ToWide(const std::uint8_t* input, std::size_t inputLength,
                std::wstring& output)
{
    output.clear();
    if (inputLength == 0)
        return true;
    if (!input ||
        inputLength > static_cast<std::size_t>(
            (std::numeric_limits<int>::max)())) {
        return false;
    }

    int byteLength = static_cast<int>(inputLength);
    const char* bytes = reinterpret_cast<const char*>(input);
    int outputLength = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, bytes, byteLength, nullptr, 0);
    if (outputLength <= 0)
        return false;

    output.resize(static_cast<std::size_t>(outputLength));
    return MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, bytes, byteLength,
        output.data(), outputLength) == outputLength;
}

bool Utf8ToWide(const std::string& input, std::wstring& output)
{
    return Utf8ToWide(
        reinterpret_cast<const std::uint8_t*>(input.data()),
        input.size(), output);
}

} // namespace TextEncoding
