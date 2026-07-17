#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace TextEncoding {

bool WideToUtf8(const wchar_t* input, std::string& output);
bool Utf8ToWide(const std::uint8_t* input, std::size_t inputLength,
                std::wstring& output);
bool Utf8ToWide(const std::string& input, std::wstring& output);

} // namespace TextEncoding
