#include "pch.h"
#include "SettingsStore.h"

namespace mutemic {
namespace {

constexpr wchar_t kKey[] = L"Software\\MuteMic";

std::vector<Shortcut> DeserializeShortcuts(const std::wstring& data);

DWORD ReadDword(HKEY key, const wchar_t* name, DWORD fallback) {
    DWORD value = 0, size = sizeof(value), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        return value;
    }
    return fallback;
}

std::wstring ReadString(HKEY key, const wchar_t* name) {
    wchar_t buf[512] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS &&
        type == REG_SZ) {
        return buf;
    }
    return L"";
}

void WriteDword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

void WriteString(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

}  // namespace

Settings SettingsStore::Load() {
    Settings s;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return s;  // primera ejecución: defaults

    s.hotkeyVk = ReadDword(key, L"HotkeyVk", s.hotkeyVk);
    s.hotkeyScan = ReadDword(key, L"HotkeyScan", s.hotkeyScan);
    s.hotkeyMods = ReadDword(key, L"HotkeyMods", s.hotkeyMods);
    s.hotkeyName = ReadString(key, L"HotkeyName");
    s.hotkeyMode = ReadDword(key, L"HotkeyMode", 0);
    if (s.hotkeyMode > 2) s.hotkeyMode = 0;
    s.padButton = ReadDword(key, L"PadButton", 0);
    s.mouseButton = ReadDword(key, L"MouseButton", 0);

    // V2: lista de shortcuts. Si no existe, MIGRAR desde los valores legacy
    // (teclado + pad + mouse de la versión de bindings fijos).
    {
        wchar_t big[4096] = {};
        DWORD size = sizeof(big) - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(key, L"Shortcuts", nullptr, &type,
                             reinterpret_cast<BYTE*>(big), &size) == ERROR_SUCCESS &&
            type == REG_SZ && big[0] != L'\0') {
            s.shortcuts = DeserializeShortcuts(big);
        }
        if (s.shortcuts.empty()) {
            Shortcut kb;
            kb.name = s.hotkeyName.empty() ? L"Keyboard" : s.hotkeyName;
            kb.type = 0;
            kb.vk = s.hotkeyVk;
            kb.scan = s.hotkeyScan;
            kb.mods = s.hotkeyMods;
            kb.mode = s.hotkeyMode;
            s.shortcuts.push_back(kb);

            Shortcut pad;
            pad.name = L"Controller";
            pad.type = 2;
            pad.code = s.padButton;   // puede quedar sin bind: card default
            pad.mode = s.hotkeyMode;
            s.shortcuts.push_back(pad);

            Shortcut mouse;
            mouse.name = L"Mouse";
            mouse.type = 1;
            mouse.code = s.mouseButton;
            mouse.mode = s.hotkeyMode;
            s.shortcuts.push_back(mouse);
        }
    }
    s.deviceId = ReadString(key, L"DeviceId");
    s.playSound = ReadDword(key, L"PlaySound", 1) != 0;
    s.tooltipShowDevice = ReadDword(key, L"TooltipDevice", 1) != 0;
    s.tooltipShowShortcut = ReadDword(key, L"TooltipShortcut", 1) != 0;
    s.startInTray = ReadDword(key, L"StartInTray", 0) != 0;
    {
        std::wstring v = ReadString(key, L"SoundMuteFile");
        if (!v.empty()) s.soundMuteFile = v;
        v = ReadString(key, L"SoundUnmuteFile");
        if (!v.empty()) s.soundUnmuteFile = v;
    }
    s.soundVolume = ReadDword(key, L"SoundVolume", 60);
    if (s.soundVolume > 100) s.soundVolume = 100;
    s.theme = ReadDword(key, L"Theme", 0);
    if (s.theme > 1) s.theme = 0;
    s.glass = ReadDword(key, L"Glass", 0) != 0;
    s.frost = ReadDword(key, L"GlassFrost", 1) != 0;
    s.visualCue = ReadDword(key, L"VisualCue", 0);
    if (s.visualCue > 3) s.visualCue = 0;
    s.cueEdge = ReadDword(key, L"CueEdge", 1);
    if (s.cueEdge > 3) s.cueEdge = 1;

    RegCloseKey(key);
    return s;
}

// ── Serialización de shortcuts ──
// Campos con '\x1F' (unit sep), records con '\x1E' (record sep). El nombre
// se sanea para no contener los separadores.
namespace {

std::wstring SerializeShortcuts(const std::vector<Shortcut>& list) {
    std::wstring out;
    for (auto const& sc : list) {
        std::wstring name = sc.name;
        for (auto& ch : name)
            if (ch == L'\x1F' || ch == L'\x1E') ch = L' ';
        wchar_t buf[64];
        swprintf_s(buf, L"\x1F%u\x1F%u\x1F%u\x1F%u\x1F%u\x1F%u",
                   sc.type, sc.vk, sc.scan, sc.mods, sc.code, sc.mode);
        if (!out.empty()) out += L'\x1E';
        out += name + buf;
    }
    return out;
}

std::vector<Shortcut> DeserializeShortcuts(const std::wstring& data) {
    std::vector<Shortcut> list;
    size_t pos = 0;
    while (pos <= data.size() && !data.empty()) {
        size_t end = data.find(L'\x1E', pos);
        std::wstring rec = data.substr(pos, end == std::wstring::npos
                                                ? std::wstring::npos : end - pos);
        Shortcut sc;
        UINT* fields[6] = { &sc.type, &sc.vk, &sc.scan, &sc.mods, &sc.code, &sc.mode };
        size_t f = rec.find(L'\x1F');
        if (f != std::wstring::npos) {
            sc.name = rec.substr(0, f);
            size_t p = f;
            for (int i = 0; i < 6 && p != std::wstring::npos; ++i) {
                size_t next = rec.find(L'\x1F', p + 1);
                *fields[i] = static_cast<UINT>(
                    _wtoi(rec.substr(p + 1, next == std::wstring::npos
                                                 ? std::wstring::npos
                                                 : next - p - 1).c_str()));
                p = next;
            }
            if (sc.mode > 2) sc.mode = 0;
            if (sc.type > 2) sc.type = 0;
            list.push_back(std::move(sc));
        }
        if (end == std::wstring::npos) break;
        pos = end + 1;
    }
    return list;
}

}  // namespace

void SettingsStore::Save(const Settings& s) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    WriteDword(key, L"HotkeyVk", s.hotkeyVk);
    WriteDword(key, L"HotkeyScan", s.hotkeyScan);
    WriteDword(key, L"HotkeyMods", s.hotkeyMods);
    WriteString(key, L"HotkeyName", s.hotkeyName);
    WriteDword(key, L"HotkeyMode", s.hotkeyMode);
    WriteDword(key, L"PadButton", s.padButton);
    WriteDword(key, L"MouseButton", s.mouseButton);
    WriteString(key, L"Shortcuts", SerializeShortcuts(s.shortcuts));
    WriteString(key, L"DeviceId", s.deviceId);
    WriteDword(key, L"PlaySound", s.playSound ? 1 : 0);
    WriteDword(key, L"TooltipDevice", s.tooltipShowDevice ? 1 : 0);
    WriteDword(key, L"TooltipShortcut", s.tooltipShowShortcut ? 1 : 0);
    WriteDword(key, L"StartInTray", s.startInTray ? 1 : 0);
    WriteString(key, L"SoundMuteFile", s.soundMuteFile);
    WriteString(key, L"SoundUnmuteFile", s.soundUnmuteFile);
    WriteDword(key, L"SoundVolume", s.soundVolume);
    WriteDword(key, L"Theme", s.theme);
    WriteDword(key, L"Glass", s.glass ? 1 : 0);
    WriteDword(key, L"GlassFrost", s.frost ? 1 : 0);
    WriteDword(key, L"VisualCue", s.visualCue);
    WriteDword(key, L"CueEdge", s.cueEdge);

    RegCloseKey(key);
}

}  // namespace mutemic
