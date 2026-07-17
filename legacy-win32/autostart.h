#pragma once
#include <windows.h>

// Autostart vía HKCU\Software\Microsoft\Windows\CurrentVersion\Run.

// ¿Está registrado el autostart?
bool IsAutostartEnabled();

// Activa/desactiva el autostart. Devuelve true si la operación tuvo éxito.
bool SetAutostart(bool enable);
