#pragma once
#include <windows.h>
#include "audio.h"

// Genera el ícono de tray para un estado dado, dibujado en runtime con GDI.
// Verde = mic abierto, rojo = muteado, gris = sin dispositivo.
// El caller es dueño del HICON y debe liberarlo con DestroyIcon.
HICON CreateStateIcon(MicState state);
