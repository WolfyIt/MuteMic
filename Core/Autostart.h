#pragma once
#include <windows.h>

namespace mutemic {

// Autostart vía HKCU\Software\Microsoft\Windows\CurrentVersion\Run.
bool IsAutostartEnabled();
bool SetAutostart(bool enable);

}  // namespace mutemic
