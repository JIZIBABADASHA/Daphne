#pragma once
// UTF-8 <-> wide string helpers. Windows APIs are UTF-16 (wchar_t); elsewhere wchar_t is UTF-32,
// used only where the C API takes wchar_t paths. The engine itself uses UTF-8 and std::filesystem::path.

#include <string>

namespace daphne {

std::string to_utf8(const std::wstring& w);
std::wstring to_wide(const std::string& s);

}  // namespace daphne
