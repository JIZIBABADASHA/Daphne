#pragma once
// UTF-8 <-> UTF-16 helpers (Windows APIs are UTF-16; the engine uses UTF-8).

#include <string>

namespace daphne {

std::string to_utf8(const std::wstring& w);
std::wstring to_wide(const std::string& s);

}  // namespace daphne
