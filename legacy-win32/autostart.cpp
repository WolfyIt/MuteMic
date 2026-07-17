#include "autostart.h"

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
            // Ruta entre comillas por si contiene espacios.
            wchar_t quoted[MAX_PATH + 2] = {};
            wsprintfW(quoted, L"\"%s\"", path);
            DWORD bytes = static_cast<DWORD>((lstrlenW(quoted) + 1) * sizeof(wchar_t));
            ok = RegSetValueExW(key, kValueName, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(quoted), bytes) == ERROR_SUCCESS;
        }
    } else {
        LONG result = RegDeleteValueW(key, kValueName);
        ok = (result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND);
    }

    RegCloseKey(key);
    return ok;
}
