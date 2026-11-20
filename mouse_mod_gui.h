#ifndef MOUSE_MOD_GUI_H
#define MOUSE_MOD_GUI_H

#include <windows.h>
#include <stdint.h>

#define MAX_EXTRA_MOUSE_BINDS 3

// Acciones del mando de Xbox Original (verificadas en FUN_14026b3a0)
enum XboxButtonAction
{
    XACT_NONE       = 0,
    XACT_RT         = 1,
    XACT_LT         = 2,
    XACT_A          = 3,
    XACT_B          = 4,
    XACT_X          = 5,
    XACT_Y          = 6,
    XACT_BLACK      = 7,
    XACT_WHITE      = 8,
    XACT_L3         = 9,
    XACT_R3         = 10,
    XACT_DPAD_UP    = 11,
    XACT_DPAD_DOWN  = 12,
    XACT_DPAD_LEFT  = 13,
    XACT_DPAD_RIGHT = 14,
    XACT_START      = 15,
    XACT_BACK       = 16,
    XACT_COUNT      = 17
};

struct ShortcutDef
{
    int  vk;
    bool shift;
    bool ctrl;
    bool alt;
};

struct ExtraMouseBind
{
    int vk;     // VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2
    int action; // XboxButtonAction
};

struct ModConfig
{
    float sensX;                // Escala amigable: 1.00 = 0.0022 rad/px
    float sensY;                // Escala amigable: 1.00 = 0.0022 rad/px
    int   invertX;
    int   invertY;
    int   execCamOrbit;
    int   removeHostageLimit;
    int   debugLog;
    float walkSpeed;
	
    int   leftClickAction;      // Default: XACT_RT (Disparar)
    int   rightClickAction;     // Default: XACT_Y (Salto tactico)

    int            extraBindCount;
    ExtraMouseBind extraBinds[MAX_EXTRA_MOUSE_BINDS];

    ShortcutDef keyOpenMenu;    // Default: Shift + P
    ShortcutDef keyToggleMod;   // Default: Shift + 0
    ShortcutDef keySensUp;      // Default: +
    ShortcutDef keySensDown;    // Default: -
	ShortcutDef keyWalk; 
};

// Estado compartido entre el motor y la GUI
extern ModConfig     g_cfg;
extern volatile bool g_mouseEnabled;
extern volatile bool g_guiOpen;
extern HWND          g_guiHwnd;

// Constante de calibracion 1:1 verificada (1.00 en GUI = 0.0022 rad/px)
static const float BASE_SENS_RAD_PER_PX = 0.0022f;

// Funciones del motor llamadas por la GUI
void Mod_SaveConfig(void);
void Mod_ResetDefaults(void);
void Mod_NotifyGuiClosed(void);
void Mod_WriteLog(const char *fmt, ...);

// Utilidades de shortcuts y nombres compartidas
bool IsShortcutHeldNow(const ShortcutDef &sc);
void FormatShortcutText(const ShortcutDef &sc, char *outBuf, size_t outSize);
const char *GetXboxActionName(int action);
const char *GetMouseButtonName(int vk);

// Arranque del hilo de la GUI Win32
void Gui_StartThread(void);

#endif // MOUSE_MOD_GUI_H
