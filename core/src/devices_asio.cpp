#include "daphne/engine/devices.h"

#include <windows.h>

#include "daphne/engine/text.h"

namespace daphne {
namespace {

std::wstring read_string(HKEY key, const wchar_t* value) {
    wchar_t buf[512];
    DWORD size = sizeof buf;
    DWORD type = 0;
    if (RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) != ERROR_SUCCESS ||
        type != REG_SZ)
        return {};
    buf[std::min<size_t>(size / sizeof(wchar_t), 511)] = L'\0';
    return buf;
}

}  // namespace

std::vector<AsioDriver> list_asio_drivers(std::string* error) {
    std::vector<AsioDriver> result;
    HKEY root = nullptr;
    // 64-bit process: native view lists 64-bit drivers.
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) {
        if (error) *error = "no ASIO drivers registered (HKLM\\SOFTWARE\\ASIO not found)";
        return result;
    }
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD len = 256;
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY sub = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;
        AsioDriver d;
        d.name = to_utf8(name);
        d.clsid = to_utf8(read_string(sub, L"CLSID"));
        d.description = to_utf8(read_string(sub, L"Description"));
        RegCloseKey(sub);
        result.push_back(std::move(d));
    }
    RegCloseKey(root);
    return result;
}

}  // namespace daphne
