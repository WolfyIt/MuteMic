#include "pch.h"
#include "Autostart.h"

namespace mutemic {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"MuteMic";

}  // namespace

bool IsAutostartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    LONG result = RegQueryValueExW(key, kValueName, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool SetAutostart(bool enable) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;

    bool ok = false;
    if (enable) {
        wchar_t path[MAX_PATH] = {};
        DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::wstring quoted = L"\"" + std::wstring(path) + L"\"";
            DWORD bytes = static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t));
            ok = RegSetValueExW(key, kValueName, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(quoted.c_str()),
                                bytes) == ERROR_SUCCESS;
        }
    } else {
        LONG result = RegDeleteValueW(key, kValueName);
        ok = (result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND);
    }

    RegCloseKey(key);
    return ok;
}

}  // namespace mutemic
