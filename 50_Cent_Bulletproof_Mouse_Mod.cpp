#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include "mouse_mod_gui.h"

// Estructura oficial RUNTIME_FUNCTION de Windows x64 (.pdata)
typedef struct _MY_RUNTIME_FUNCTION {
    DWORD BeginAddress;
    DWORD EndAddress;
    DWORD UnwindData;
} MY_RUNTIME_FUNCTION;

typedef MY_RUNTIME_FUNCTION* (WINAPI *RtlLookupFunctionEntry_t)(DWORD64 ControlPc, PDWORD64 ImageBase, void *HistoryTable);

// Punteros 100% dinamicos (Cero direcciones fijas: funciona en V1, V2, V3...)
static uintptr_t ADDR_XBOX_RAM_BASE_PTR = 0;
static uintptr_t ADDR_REG_ECX_THIS      = 0;
static uintptr_t ADDR_REG_ESP           = 0;
static uintptr_t CALLSITE_INPUT         = 0;
static uintptr_t CALLSITE_CAM_ANGLES    = 0;

static uintptr_t g_imgBase   = 0;
static uintptr_t g_imgEnd    = 0;
static uintptr_t g_textStart = 0;
static size_t    g_textSize  = 0;

// Firma exacta de FUN_input (4 parametros + retorno uint64_t, verificada en FUN_14026b3a0)
typedef uint64_t (*InputFunc_t)(uint8_t *param_1, uint64_t param_2, uint64_t param_3, uint8_t param_4);
typedef void (*XboxVoidFunc_t)(void);

static InputFunc_t    Real_FUN_input  = NULL;
static XboxVoidFunc_t Real_FUN_camera = NULL;

// Estado global compartido con la GUI (arranca siempre 100% apagado)
ModConfig     g_cfg          = {};
volatile bool g_mouseEnabled = false;
volatile bool g_guiOpen      = false;
HWND          g_guiHwnd      = NULL;

// Rutas absolutas junto al ejecutable
static char g_exeDir[MAX_PATH]  = {0};
static char g_iniPath[MAX_PATH] = {0};
static char g_logPath[MAX_PATH] = {0};

// Estado interno de camara y cursor
static ULONGLONG g_lastCamTick         = 0;
static ULONGLONG g_lastCamFrameTick    = 0;
static bool      g_cursorHidden        = false;
static LONG      g_threadCreated       = 0;
static int32_t   g_lastCamMode         = -999;
static float     g_savedMode0Pitch     = 0.0f;
static bool      g_justReturnedToMode0 = false;
static int       g_camMoveLogCount     = 0;
static bool      g_camHookAliveLogged  = false;

// ============================================================================
// PROXY REAL A System32\xinput1_4.dll (No rompe mandos fisicos / Steam Deck)
// ============================================================================
typedef DWORD (WINAPI *XInputGetState_t)(DWORD dwUserIndex, void *pState);
typedef DWORD (WINAPI *XInputSetState_t)(DWORD dwUserIndex, void *pVibration);

static HMODULE          g_realXInput         = NULL;
static XInputGetState_t g_realXInputGetState = NULL;
static XInputSetState_t g_realXInputSetState = NULL;
static LONG             g_xinputLoaded       = 0;

static void EnsureRealXInputLoaded()
{
    if (InterlockedCompareExchange(&g_xinputLoaded, 1, 0) == 0)
    {
        char sysDir[MAX_PATH] = {0};
        if (GetSystemDirectoryA(sysDir, MAX_PATH) > 0)
        {
            char dllPath[MAX_PATH] = {0};
            snprintf(dllPath, sizeof(dllPath), "%s\\xinput1_4.dll", sysDir);
            g_realXInput = LoadLibraryA(dllPath);
            if (g_realXInput)
            {
                g_realXInputGetState = (XInputGetState_t)GetProcAddress(g_realXInput, "XInputGetState");
                g_realXInputSetState = (XInputSetState_t)GetProcAddress(g_realXInput, "XInputSetState");
            }
        }
    }
}

// ============================================================================
// UTILIDADES DE LOG, SHORTCUTS Y NOMBRES
// ============================================================================
static void InitFilePaths()
{
    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char *lastSlash = strrchr(exePath, '\\');
    if (lastSlash)
    {
        size_t len = (size_t)(lastSlash - exePath);
        if (len >= MAX_PATH) len = MAX_PATH - 1;
        memcpy(g_exeDir, exePath, len);
        g_exeDir[len] = '\0';
    }
    else
    {
        strcpy(g_exeDir, ".");
    }
    snprintf(g_iniPath, sizeof(g_iniPath), "%s\\mouse_mod.ini", g_exeDir);
    snprintf(g_logPath, sizeof(g_logPath), "%s\\mod_log.txt", g_exeDir);
}

static void FormatSensIni(float val, char *buf, size_t sz)
{
    int milli = (int)(val * 1000.0f + 0.5f);
    if ((milli % 10) != 0)
        snprintf(buf, sz, "%.3f", val);
    else
        snprintf(buf, sz, "%.2f", val);
}

void Mod_WriteLog(const char *fmt, ...)
{
    if (!g_cfg.debugLog) return;
    if (g_logPath[0] == '\0') InitFilePaths();
    FILE *f = fopen(g_logPath, "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fflush(f);
    fclose(f);
}

const char *GetXboxActionName(int action)
{
    switch (action)
    {
        case XACT_NONE:       return "None";
        case XACT_RT:         return "RT (Shoot / Right Trigger)";
        case XACT_LT:         return "LT (Aim / Left Trigger)";
        case XACT_A:          return "A";
        case XACT_B:          return "B";
        case XACT_X:          return "X";
        case XACT_Y:          return "Y (Tactical / Jump)";
        case XACT_BLACK:      return "Black";
        case XACT_WHITE:      return "White";
        case XACT_L3:         return "L3 (Left Stick Click)";
        case XACT_R3:         return "R3 (Right Stick Click)";
        case XACT_DPAD_UP:    return "D-Pad Up";
        case XACT_DPAD_DOWN:  return "D-Pad Down";
        case XACT_DPAD_LEFT:  return "D-Pad Left";
        case XACT_DPAD_RIGHT: return "D-Pad Right";
        case XACT_START:      return "Start";
        case XACT_BACK:       return "Back";
        default:              return "None";
    }
}

const char *GetMouseButtonName(int vk)
{
    switch (vk)
    {
        case VK_LBUTTON:  return "Left Click";
        case VK_RBUTTON:  return "Right Click";
        case VK_MBUTTON:  return "Middle Click";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        default:          return "Mouse Btn";
    }
}

static void GetVkDisplayName(int vk, char *outBuf, size_t outSize)
{
    if (vk >= 'A' && vk <= 'Z') { snprintf(outBuf, outSize, "%c", vk); return; }
    if (vk >= '0' && vk <= '9') { snprintf(outBuf, outSize, "%c", vk); return; }
    if (vk >= VK_F1 && vk <= VK_F12) { snprintf(outBuf, outSize, "F%d", (vk - VK_F1) + 1); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(outBuf, outSize, "Num %d", vk - VK_NUMPAD0); return; }

    switch (vk)
    {
        case VK_OEM_PLUS:  case VK_ADD:      snprintf(outBuf, outSize, "+"); break;
        case VK_OEM_MINUS: case VK_SUBTRACT: snprintf(outBuf, outSize, "-"); break;
        case VK_SPACE:     snprintf(outBuf, outSize, "Space"); break;
        case VK_TAB:       snprintf(outBuf, outSize, "Tab"); break;
        case VK_RETURN:    snprintf(outBuf, outSize, "Enter"); break;
        case VK_BACK:      snprintf(outBuf, outSize, "Backspace"); break;
        case VK_INSERT:    snprintf(outBuf, outSize, "Insert"); break;
        case VK_DELETE:    snprintf(outBuf, outSize, "Delete"); break;
        case VK_HOME:      snprintf(outBuf, outSize, "Home"); break;
        case VK_END:       snprintf(outBuf, outSize, "End"); break;
        case VK_PRIOR:     snprintf(outBuf, outSize, "PgUp"); break;
        case VK_NEXT:      snprintf(outBuf, outSize, "PgDn"); break;
        case VK_UP:        snprintf(outBuf, outSize, "Up"); break;
        case VK_DOWN:      snprintf(outBuf, outSize, "Down"); break;
        case VK_LEFT:      snprintf(outBuf, outSize, "Left"); break;
        case VK_RIGHT:     snprintf(outBuf, outSize, "Right"); break;
        case VK_CAPITAL:   snprintf(outBuf, outSize, "CapsLock"); break;
        case VK_OEM_3:     snprintf(outBuf, outSize, "~"); break;
        default:
        {
            UINT scan = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
            char name[32] = {0};
            if (scan && GetKeyNameTextA((LONG)(scan << 16), name, sizeof(name)) > 0)
                snprintf(outBuf, outSize, "%s", name);
            else
                snprintf(outBuf, outSize, "Key 0x%02X", vk);
            break;
        }
    }
}

void FormatShortcutText(const ShortcutDef &sc, char *outBuf, size_t outSize)
{
    char keyName[32];
    GetVkDisplayName(sc.vk, keyName, sizeof(keyName));
    snprintf(outBuf, outSize, "%s%s%s%s",
             sc.ctrl  ? "Ctrl + "  : "",
             sc.alt   ? "Alt + "   : "",
             sc.shift ? "Shift + " : "",
             keyName);
}

bool IsShortcutHeldNow(const ShortcutDef &sc)
{
    if (sc.vk == 0) return false;

    bool shiftHeld = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
    bool ctrlHeld  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool altHeld   = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;

    if (shiftHeld != sc.shift || ctrlHeld != sc.ctrl || altHeld != sc.alt)
        return false;

    if ((GetAsyncKeyState(sc.vk) & 0x8000) != 0) return true;

    if (sc.vk == '0' && (GetAsyncKeyState(VK_NUMPAD0) & 0x8000)) return true;
    if (sc.vk == VK_OEM_PLUS && (GetAsyncKeyState(VK_ADD) & 0x8000)) return true;
    if (sc.vk == VK_OEM_MINUS && (GetAsyncKeyState(VK_SUBTRACT) & 0x8000)) return true;

    return false;
}

// ============================================================================
// CONFIGURACION (mouse_mod.ini)
// ============================================================================
void Mod_ResetDefaults(void)
{
    g_cfg.sensX              = 1.00f;
    g_cfg.sensY              = 1.00f;
    g_cfg.invertX            = 0;
    g_cfg.invertY            = 0;
    g_cfg.execCamOrbit       = 1;
    g_cfg.removeHostageLimit = 1;
    g_cfg.debugLog           = 0;
    g_cfg.walkSpeed          = 0.56f;

    g_cfg.leftClickAction    = XACT_RT;
    g_cfg.rightClickAction   = XACT_Y;
    g_cfg.extraBindCount     = 0;
    memset(g_cfg.extraBinds, 0, sizeof(g_cfg.extraBinds));

    g_cfg.keyOpenMenu  = { 'P', true, false, false };           // Shift + P
    g_cfg.keyToggleMod = { '0', true, false, false };           // Shift + 0
    g_cfg.keySensUp    = { VK_OEM_PLUS, false, false, false };  // +
    g_cfg.keySensDown  = { VK_OEM_MINUS, false, false, false }; // -
    g_cfg.keyWalk      = { 'Z', false, false, false };          // Z
}

static void SaveShortcutIni(const char *prefix, const ShortcutDef &sc)
{
    char key[48], val[32];
    snprintf(key, sizeof(key), "%sVK", prefix);
    snprintf(val, sizeof(val), "%d", sc.vk);
    WritePrivateProfileStringA("Shortcuts", key, val, g_iniPath);

    snprintf(key, sizeof(key), "%sMod", prefix);
    int mod = (sc.shift ? 1 : 0) | (sc.ctrl ? 2 : 0) | (sc.alt ? 4 : 0);
    snprintf(val, sizeof(val), "%d", mod);
    WritePrivateProfileStringA("Shortcuts", key, val, g_iniPath);
}

static void LoadShortcutIni(const char *prefix, ShortcutDef *sc)
{
    char key[48];
    snprintf(key, sizeof(key), "%sVK", prefix);
    int defMod = (sc->shift ? 1 : 0) | (sc->ctrl ? 2 : 0) | (sc->alt ? 4 : 0);
    sc->vk = GetPrivateProfileIntA("Shortcuts", key, sc->vk, g_iniPath);

    snprintf(key, sizeof(key), "%sMod", prefix);
    int mod = GetPrivateProfileIntA("Shortcuts", key, defMod, g_iniPath);
    sc->shift = (mod & 1) != 0;
    sc->ctrl  = (mod & 2) != 0;
    sc->alt   = (mod & 4) != 0;
}

void Mod_SaveConfig(void)
{
    if (g_iniPath[0] == '\0') InitFilePaths();

    char buf[32];
    FormatSensIni(g_cfg.sensX, buf, sizeof(buf));
    WritePrivateProfileStringA("Mouse", "SensitivityX", buf, g_iniPath);
    FormatSensIni(g_cfg.sensY, buf, sizeof(buf));
    WritePrivateProfileStringA("Mouse", "SensitivityY", buf, g_iniPath);

    WritePrivateProfileStringA("Mouse", "InvertX", g_cfg.invertX ? "1" : "0", g_iniPath);
    WritePrivateProfileStringA("Mouse", "InvertY", g_cfg.invertY ? "1" : "0", g_iniPath);
    WritePrivateProfileStringA("Mouse", "RemoveHostagePitchLimit", g_cfg.removeHostageLimit ? "1" : "0", g_iniPath);
    WritePrivateProfileStringA("Mouse", "ExecCamOrbit", g_cfg.execCamOrbit ? "1" : "0", g_iniPath);

    snprintf(buf, sizeof(buf), "%.2f", g_cfg.walkSpeed);
    WritePrivateProfileStringA("Mouse", "WalkSpeed", buf, g_iniPath);

    snprintf(buf, sizeof(buf), "%d", g_cfg.leftClickAction);
    WritePrivateProfileStringA("Binds", "LeftClickAction", buf, g_iniPath);
    snprintf(buf, sizeof(buf), "%d", g_cfg.rightClickAction);
    WritePrivateProfileStringA("Binds", "RightClickAction", buf, g_iniPath);
    snprintf(buf, sizeof(buf), "%d", g_cfg.extraBindCount);
    WritePrivateProfileStringA("Binds", "ExtraBindCount", buf, g_iniPath);

    for (int i = 0; i < MAX_EXTRA_MOUSE_BINDS; i++)
    {
        char kVk[32], kAct[32];
        snprintf(kVk, sizeof(kVk), "Extra%dVK", i);
        snprintf(kAct, sizeof(kAct), "Extra%dAction", i);
        if (i < g_cfg.extraBindCount)
        {
            snprintf(buf, sizeof(buf), "%d", g_cfg.extraBinds[i].vk);
            WritePrivateProfileStringA("Binds", kVk, buf, g_iniPath);
            snprintf(buf, sizeof(buf), "%d", g_cfg.extraBinds[i].action);
            WritePrivateProfileStringA("Binds", kAct, buf, g_iniPath);
        }
        else
        {
            WritePrivateProfileStringA("Binds", kVk, "0", g_iniPath);
            WritePrivateProfileStringA("Binds", kAct, "0", g_iniPath);
        }
    }

    SaveShortcutIni("OpenMenu",  g_cfg.keyOpenMenu);
    SaveShortcutIni("ToggleMod", g_cfg.keyToggleMod);
    SaveShortcutIni("SensUp",    g_cfg.keySensUp);
    SaveShortcutIni("SensDown",  g_cfg.keySensDown);
    SaveShortcutIni("Walk",      g_cfg.keyWalk);

    WritePrivateProfileStringA("Misc", "DebugLog", g_cfg.debugLog ? "1" : "0", g_iniPath);
}

static void LoadOrCreateConfig()
{
    InitFilePaths();
    Mod_ResetDefaults();
    g_mouseEnabled = false;

    FILE *f = fopen(g_iniPath, "r");
    if (!f)
    {
        Mod_SaveConfig();
        return;
    }
    fclose(f);

    char buf[32] = {0};
    GetPrivateProfileStringA("Mouse", "SensitivityX", "1.00", buf, sizeof(buf), g_iniPath);
    float sx = (float)atof(buf);
    GetPrivateProfileStringA("Mouse", "SensitivityY", "1.00", buf, sizeof(buf), g_iniPath);
    float sy = (float)atof(buf);

    if (sx > 0.0f && sx < 0.05f) sx = sx / BASE_SENS_RAD_PER_PX;
    if (sy > 0.0f && sy < 0.05f) sy = sy / BASE_SENS_RAD_PER_PX;
    if (sx < 0.01f) sx = 1.00f;
    if (sy < 0.01f) sy = 1.00f;
    if (sx > 100.0f) sx = 100.0f;
    if (sy > 100.0f) sy = 100.0f;
    g_cfg.sensX = sx;
    g_cfg.sensY = sy;

    g_cfg.invertX            = GetPrivateProfileIntA("Mouse", "InvertX", 0, g_iniPath);
    g_cfg.invertY            = GetPrivateProfileIntA("Mouse", "InvertY", 0, g_iniPath);
    g_cfg.removeHostageLimit = GetPrivateProfileIntA("Mouse", "RemoveHostagePitchLimit", 1, g_iniPath);
    g_cfg.execCamOrbit       = GetPrivateProfileIntA("Mouse", "ExecCamOrbit", 1, g_iniPath);

    GetPrivateProfileStringA("Mouse", "WalkSpeed", "0.56", buf, sizeof(buf), g_iniPath);
    float wspd = (float)atof(buf);
    if (wspd < 0.34f) wspd = 0.34f;
    if (wspd > 1.00f) wspd = 1.00f;
    g_cfg.walkSpeed = wspd;

    g_cfg.leftClickAction  = GetPrivateProfileIntA("Binds", "LeftClickAction", XACT_RT, g_iniPath);
    g_cfg.rightClickAction = GetPrivateProfileIntA("Binds", "RightClickAction", XACT_Y, g_iniPath);
    if (g_cfg.leftClickAction < 0 || g_cfg.leftClickAction >= XACT_COUNT) g_cfg.leftClickAction = XACT_RT;
    if (g_cfg.rightClickAction < 0 || g_cfg.rightClickAction >= XACT_COUNT) g_cfg.rightClickAction = XACT_Y;

    int extraCnt = GetPrivateProfileIntA("Binds", "ExtraBindCount", 0, g_iniPath);
    if (extraCnt < 0) extraCnt = 0;
    if (extraCnt > MAX_EXTRA_MOUSE_BINDS) extraCnt = MAX_EXTRA_MOUSE_BINDS;
    g_cfg.extraBindCount = 0;
    for (int i = 0; i < extraCnt; i++)
    {
        char kVk[32], kAct[32];
        snprintf(kVk, sizeof(kVk), "Extra%dVK", i);
        snprintf(kAct, sizeof(kAct), "Extra%dAction", i);
        int vk  = GetPrivateProfileIntA("Binds", kVk, 0, g_iniPath);
        int act = GetPrivateProfileIntA("Binds", kAct, XACT_NONE, g_iniPath);
        if (vk != 0 && act >= 0 && act < XACT_COUNT)
        {
            g_cfg.extraBinds[g_cfg.extraBindCount].vk     = vk;
            g_cfg.extraBinds[g_cfg.extraBindCount].action = act;
            g_cfg.extraBindCount++;
        }
    }

    LoadShortcutIni("OpenMenu",  &g_cfg.keyOpenMenu);
    LoadShortcutIni("ToggleMod", &g_cfg.keyToggleMod);
    LoadShortcutIni("SensUp",    &g_cfg.keySensUp);
    LoadShortcutIni("SensDown",  &g_cfg.keySensDown);
    LoadShortcutIni("Walk",      &g_cfg.keyWalk);

    g_cfg.debugLog = GetPrivateProfileIntA("Misc", "DebugLog", 0, g_iniPath);
}

void Mod_NotifyGuiClosed(void)
{
    g_lastCamTick = 0;
}

// ============================================================================
// ESCANER UNIVERSAL (CONSTANTES XBOX + .PDATA CHAININFO + PLAN B INPUT)
// ============================================================================
static bool InitExecutableSections()
{
    HMODULE hMod = GetModuleHandleA(NULL);
    if (!hMod) return false;

    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)hMod;
    IMAGE_NT_HEADERS *nt  = (IMAGE_NT_HEADERS *)((uint8_t *)hMod + dos->e_lfanew);

    g_imgBase = (uintptr_t)hMod;
    g_imgEnd  = g_imgBase + nt->OptionalHeader.SizeOfImage;

    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        if (strncmp((char *)sec[i].Name, ".text", 5) == 0)
        {
            g_textStart = g_imgBase + sec[i].VirtualAddress;
            g_textSize  = sec[i].Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

static bool IsWritableDataAddress(uintptr_t addr)
{
    if (addr < g_imgBase || addr >= g_imgEnd) return false;

    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_imgBase;
    IMAGE_NT_HEADERS *nt  = (IMAGE_NT_HEADERS *)((uint8_t *)g_imgBase + dos->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);

    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        uintptr_t secStart = g_imgBase + sec[i].VirtualAddress;
        DWORD rawSize = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        uintptr_t secEnd = secStart + rawSize;
        if (addr >= secStart && addr < secEnd)
        {
            return (sec[i].Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        }
    }
    return false;
}

static uintptr_t ResolveRootFunctionStart(MY_RUNTIME_FUNCTION *rt, DWORD64 imageBase, const char *label)
{
    if (!rt) return 0;

    MY_RUNTIME_FUNCTION cur = *rt;
    for (int depth = 0; depth < 32; depth++)
    {
        DWORD uwRva = cur.UnwindData;
        if (uwRva == 0) break;

        if (uwRva & 1u)
        {
            uintptr_t targetAddr = (uintptr_t)imageBase + (uintptr_t)(uwRva & ~1u);
            if (targetAddr < g_imgBase || targetAddr + sizeof(MY_RUNTIME_FUNCTION) > g_imgEnd)
                break;

            MY_RUNTIME_FUNCTION *nextRt = (MY_RUNTIME_FUNCTION *)targetAddr;
            Mod_WriteLog("[UNIVERSAL] %s .pdata redirect #%d: 0x%llX -> 0x%llX\n",
                         label, depth + 1,
                         (unsigned long long)(imageBase + cur.BeginAddress),
                         (unsigned long long)(imageBase + nextRt->BeginAddress));
            if (nextRt->BeginAddress == cur.BeginAddress && nextRt->UnwindData == cur.UnwindData)
                break;
            cur = *nextRt;
            continue;
        }

        uintptr_t uwAddr = (uintptr_t)imageBase + (uintptr_t)uwRva;
        if (uwAddr < g_imgBase || uwAddr + 4 > g_imgEnd)
            break;

        const uint8_t *uw = (const uint8_t *)uwAddr;
        uint8_t flags = (uw[0] >> 3) & 0x1Fu;
        uint8_t countOfCodes = uw[2];

        if ((flags & 0x04u) == 0)
            break;

        uint32_t alignedCodes = ((uint32_t)countOfCodes + 1u) & ~1u;
        uintptr_t chainRtAddr = uwAddr + 4u + (uintptr_t)alignedCodes * 2u;
        if (chainRtAddr < g_imgBase || chainRtAddr + sizeof(MY_RUNTIME_FUNCTION) > g_imgEnd)
            break;

        MY_RUNTIME_FUNCTION *parentRt = (MY_RUNTIME_FUNCTION *)chainRtAddr;
        Mod_WriteLog("[UNIVERSAL] %s .pdata chain #%d: 0x%llX -> 0x%llX\n",
                     label, depth + 1,
                     (unsigned long long)(imageBase + cur.BeginAddress),
                     (unsigned long long)(imageBase + parentRt->BeginAddress));

        if (parentRt->BeginAddress == cur.BeginAddress && parentRt->UnwindData == cur.UnwindData)
            break;

        cur = *parentRt;
    }

    return (uintptr_t)(imageBase + cur.BeginAddress);
}

static uintptr_t FindCallToFunction(uintptr_t targetFunc)
{
    uint8_t *data = (uint8_t *)g_textStart;
    for (size_t i = 0; i + 5 <= g_textSize; i++)
    {
        if (data[i] == 0xE8)
        {
            int32_t rel = *(int32_t *)(data + i + 1);
            uintptr_t callAddr = g_textStart + i;
            if ((uintptr_t)((int64_t)(callAddr + 5) + (int64_t)rel) == targetFunc)
            {
                return callAddr;
            }
        }
    }
    return 0;
}

static uintptr_t FindInputMarkerPlanB(RtlLookupFunctionEntry_t fnRtlLookup)
{
    uint8_t *text = (uint8_t *)g_textStart;
    for (size_t i = 0; i + 2048 <= g_textSize; i += 2)
    {
        if (text[i] == 0x01 && text[i + 1] == 0x80)
        {
            int cnt8001 = 0, cnt7FFF = 0;
            size_t winEnd = (i + 1536 < g_textSize - 2) ? (i + 1536) : (g_textSize - 2);
            for (size_t j = i; j < winEnd; j++)
            {
                if (text[j] == 0x01 && text[j + 1] == 0x80) cnt8001++;
                if (text[j] == 0xFF && text[j + 1] == 0x7F) cnt7FFF++;
            }
            if (cnt8001 >= 4 && cnt7FFF >= 4)
            {
                DWORD64 inBase = 0;
                MY_RUNTIME_FUNCTION *rt = fnRtlLookup((DWORD64)(g_textStart + i), &inBase, NULL);
                if (rt)
                {
                    uintptr_t funcStart = ResolveRootFunctionStart(rt, inBase, "InputPlanB");
                    uintptr_t funcEnd   = (uintptr_t)(inBase + rt->EndAddress);
                    if (funcEnd > funcStart && (funcEnd - funcStart) < 0x8000)
                    {
                        int analogWrites = 0;
                        uint8_t *fn = (uint8_t *)funcStart;
                        size_t fnLen = funcEnd - funcStart;
                        for (size_t k = 0; k + 4 <= fnLen; k++)
                        {
                            if (fn[k] == 0xC6 && (fn[k + 1] & 0xF8) == 0x40 &&
                                fn[k + 2] >= 2 && fn[k + 2] <= 9 && fn[k + 3] == 0xFF)
                            {
                                analogWrites++;
                            }
                        }
                        if (analogWrites >= 6)
                        {
                            Mod_WriteLog("[UNIVERSAL] Input localizado via Plan B (4 ejes 0x8001/0x7FFF + %d botones 0xFF)\n", analogWrites);
                            return g_textStart + i;
                        }
                    }
                }
                i = winEnd;
            }
        }
    }
    return 0;
}

static bool ScanUniversalAddresses()
{
    if (!InitExecutableSections())
    {
        Mod_WriteLog("[ERROR] No se pudo leer la seccion .text del ejecutable.\n");
        return false;
    }

    Mod_WriteLog("[UNIVERSAL] ImageBase=0x%llX | .text=0x%llX (size=0x%llX)\n",
                 (unsigned long long)g_imgBase,
                 (unsigned long long)g_textStart,
                 (unsigned long long)g_textSize);

    RtlLookupFunctionEntry_t fnRtlLookup = (RtlLookupFunctionEntry_t)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "RtlLookupFunctionEntry");
    if (!fnRtlLookup)
    {
        Mod_WriteLog("[ERROR] No se encontro RtlLookupFunctionEntry en kernel32.dll.\n");
        return false;
    }

    uint8_t *text = (uint8_t *)g_textStart;

    // 1. CAZAR FUNCION DE CAMARA 3D (icall_sites .xbe: 0x00094633 y 0x0009464F)
    uintptr_t markerAddr = 0;
    for (size_t i = 0; i + 4 <= g_textSize; i++)
    {
        if (text[i] == 0x33 && text[i + 1] == 0x46 && text[i + 2] == 0x09 && text[i + 3] == 0x00)
        {
            size_t endScan = (i + 2048 < g_textSize - 4) ? (i + 2048) : (g_textSize - 4);
            for (size_t j = i + 4; j < endScan; j++)
            {
                if (text[j] == 0x4F && text[j + 1] == 0x46 && text[j + 2] == 0x09 && text[j + 3] == 0x00)
                {
                    markerAddr = g_textStart + i;
                    break;
                }
            }
            if (markerAddr != 0) break;
        }
    }

    if (markerAddr == 0)
    {
        Mod_WriteLog("[ERROR] No se encontro el marcador Xbox 0x00094633/0x0009464F en .text.\n");
        return false;
    }
    Mod_WriteLog("[UNIVERSAL] Marcador Xbox 0x94633 hallado en: 0x%llX\n", (unsigned long long)markerAddr);

    // 1A. Extraer icall_site -> ESP (-0x60) y ECX (-0x70)
    uintptr_t icallGlobalAddr = 0;
    uint8_t *m = (uint8_t *)markerAddr;
    if (markerAddr >= g_textStart + 6 && m[-6] == 0xC7 && m[-5] == 0x05)
    {
        int32_t rel = *(int32_t *)(m - 4);
        icallGlobalAddr = (uintptr_t)((int64_t)(markerAddr + 4) + (int64_t)rel);
    }
    else
    {
        for (int off = 4; off < 32; off++)
        {
            if (m[off] == 0x89 && ((m[off + 1] & 0xC7) == 0x05))
            {
                int32_t rel = *(int32_t *)(m + off + 2);
                uintptr_t cand = (uintptr_t)((int64_t)(markerAddr + off + 6) + (int64_t)rel);
                if (cand > g_textStart + g_textSize && cand < g_imgEnd && (cand & 3) == 0)
                {
                    icallGlobalAddr = cand;
                    break;
                }
            }
        }
    }

    if (icallGlobalAddr != 0)
    {
        ADDR_REG_ESP      = icallGlobalAddr - 0x60ULL;
        ADDR_REG_ECX_THIS = icallGlobalAddr - 0x70ULL;
        Mod_WriteLog("[UNIVERSAL] icall_site=0x%llX -> ESP=0x%llX | ECX=0x%llX\n",
                     (unsigned long long)icallGlobalAddr,
                     (unsigned long long)ADDR_REG_ESP,
                     (unsigned long long)ADDR_REG_ECX_THIS);
    }
    else
    {
        Mod_WriteLog("[ERROR] No se pudo extraer icall_site desde 0x%llX.\n", (unsigned long long)markerAddr);
        return false;
    }

    // 1B. Inicio raiz de funcion de camara siguiendo UNW_FLAG_CHAININFO
    DWORD64 lookupBase = 0;
    MY_RUNTIME_FUNCTION *camRt = fnRtlLookup((DWORD64)markerAddr, &lookupBase, NULL);
    if (!camRt)
    {
        Mod_WriteLog("[ERROR] RtlLookupFunctionEntry fallo para camara en 0x%llX.\n", (unsigned long long)markerAddr);
        return false;
    }

    uintptr_t camFuncStart = ResolveRootFunctionStart(camRt, lookupBase, "Camara");
    Mod_WriteLog("[UNIVERSAL] Funcion Camara raiz (.pdata): 0x%llX\n", (unsigned long long)camFuncStart);

    // 1C. Extraer ADDR_XBOX_RAM_BASE_PTR del prologo (escaneando hasta 512 bytes)
    uint8_t *cf = (uint8_t *)camFuncStart;
    uintptr_t fallbackRamBase = 0;
    size_t maxPrologScan = 512;
    if (camFuncStart + maxPrologScan > g_textStart + g_textSize)
        maxPrologScan = (g_textStart + g_textSize) - camFuncStart;

    for (size_t k = 0; k + 10 <= maxPrologScan; k++)
    {
        if ((cf[k] == 0x48 || cf[k] == 0x4C) && cf[k + 1] == 0x8B && ((cf[k + 2] & 0xC7) == 0x05))
        {
            if (cf[k] == 0x48 && cf[k + 2] == 0x05 &&
                cf[k + 7] == 0x48 && cf[k + 8] == 0x33 && cf[k + 9] == 0xC4)
            {
                continue;
            }

            int32_t rel = *(int32_t *)(cf + k + 3);
            uintptr_t cand = (uintptr_t)((int64_t)(camFuncStart + k + 7) + (int64_t)rel);
            if (cand > g_textStart + g_textSize && cand < g_imgEnd && (cand & 7) == 0)
            {
                if (IsWritableDataAddress(cand))
                {
                    ADDR_XBOX_RAM_BASE_PTR = cand;
                    Mod_WriteLog("[UNIVERSAL] XboxRamBase extraido del prologo (+0x%llX): 0x%llX\n",
                                 (unsigned long long)k, (unsigned long long)ADDR_XBOX_RAM_BASE_PTR);
                    break;
                }
                else if (fallbackRamBase == 0)
                {
                    fallbackRamBase = cand;
                }
            }
        }
    }

    if (ADDR_XBOX_RAM_BASE_PTR == 0 && fallbackRamBase != 0)
    {
        ADDR_XBOX_RAM_BASE_PTR = fallbackRamBase;
        Mod_WriteLog("[UNIVERSAL] XboxRamBase extraido (fallback): 0x%llX\n", (unsigned long long)ADDR_XBOX_RAM_BASE_PTR);
    }

    if (ADDR_XBOX_RAM_BASE_PTR == 0)
    {
        Mod_WriteLog("[ERROR] No se pudo extraer XboxRamBase del prologo de 0x%llX.\n", (unsigned long long)camFuncStart);
        return false;
    }

    // 1D. Buscar CALL unico a la funcion de camara
    CALLSITE_CAM_ANGLES = FindCallToFunction(camFuncStart);
    Mod_WriteLog("[UNIVERSAL] CallSite Camara hallado: 0x%llX\n", (unsigned long long)CALLSITE_CAM_ANGLES);

    // 2. CAZAR FUNCION DE INPUT (Plan A: wasdMask, Plan B: 4 ejes 0x8001/0x7FFF + 0xFF)
    uintptr_t inputMarker = 0;
    const uint8_t wasdMask[8] = { 0x0F, 0x00, 0x00, 0x90, 0x00, 0x40, 0x04, 0x00 };
    for (size_t i = 0; i + 8 <= g_textSize; i++)
    {
        if (memcmp(text + i, wasdMask, 8) == 0)
        {
            inputMarker = g_textStart + i;
            break;
        }
    }

    if (inputMarker == 0)
    {
        Mod_WriteLog("[UNIVERSAL] Plan A (wasdMask) no hallado, intentando Plan B...\n");
        inputMarker = FindInputMarkerPlanB(fnRtlLookup);
    }

    if (inputMarker != 0)
    {
        DWORD64 inBase = 0;
        MY_RUNTIME_FUNCTION *inRt = fnRtlLookup((DWORD64)inputMarker, &inBase, NULL);
        if (inRt)
        {
            uintptr_t inputFuncStart = ResolveRootFunctionStart(inRt, inBase, "Input");
            CALLSITE_INPUT = FindCallToFunction(inputFuncStart);
            Mod_WriteLog("[UNIVERSAL] Funcion Input raiz (.pdata): 0x%llX -> CallSite: 0x%llX\n",
                         (unsigned long long)inputFuncStart, (unsigned long long)CALLSITE_INPUT);
        }
    }
    else
    {
        Mod_WriteLog("[ERROR] No se pudo localizar la funcion de Input (Plan A ni Plan B).\n");
    }

    return (CALLSITE_CAM_ANGLES != 0 && CALLSITE_INPUT != 0);
}

// ============================================================================
// CONTROL DE CURSOR, FOCO, HOTKEYS Y BINDS
// ============================================================================
static void UpdateCursorVisibility(bool forceHide)
{
    if (forceHide && !g_cursorHidden)
    {
        while (ShowCursor(FALSE) >= 0);
        g_cursorHidden = true;
    }
    else if (!forceHide && g_cursorHidden)
    {
        while (ShowCursor(TRUE) < 0);
        g_cursorHidden = false;
    }
}

static bool IsGameWindowFocused(HWND *outHwnd)
{
    if (g_guiOpen) return false;
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (g_guiHwnd && (fg == g_guiHwnd || IsChild(g_guiHwnd, fg)))
        return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId())
    {
        if (outHwnd) *outHwnd = fg;
        return true;
    }
    return false;
}

static void PollMouseDelta(int *outDx, int *outDy)
{
    *outDx = 0;
    *outDy = 0;
    HWND hwnd = NULL;
    ULONGLONG now = GetTickCount64();

    if (IsGameWindowFocused(&hwnd))
    {
        RECT rc;
        if (GetClientRect(hwnd, &rc))
        {
            POINT centerPt;
            centerPt.x = (rc.right - rc.left) / 2;
            centerPt.y = (rc.bottom - rc.top) / 2;
            ClientToScreen(hwnd, &centerPt);

            if (g_lastCamTick != 0 && (now - g_lastCamTick <= 150))
            {
                POINT curPt;
                if (GetCursorPos(&curPt))
                {
                    *outDx = curPt.x - centerPt.x;
                    *outDy = curPt.y - centerPt.y;
                }
            }
            SetCursorPos(centerPt.x, centerPt.y);
            g_lastCamTick = now;
            return;
        }
    }
    g_lastCamTick = 0;
}

static void CheckHotkeys(void)
{
    if (g_guiOpen) return;

    static ULONGLONG lastKeyTick = 0;
    ULONGLONG now = GetTickCount64();
    if (now - lastKeyTick < 220) return;

    if (IsShortcutHeldNow(g_cfg.keyToggleMod))
    {
        g_mouseEnabled = !g_mouseEnabled;
        g_lastCamTick = 0;
        if (!g_mouseEnabled) UpdateCursorVisibility(false);
        lastKeyTick = now;
        Mod_WriteLog("[HOTKEY Toggle] Mouse Mod: %s\n", g_mouseEnabled ? "ON" : "OFF");
    }
    else if (IsShortcutHeldNow(g_cfg.keySensDown))
    {
        g_cfg.sensX *= 0.90f;
        g_cfg.sensY *= 0.90f;
        if (g_cfg.sensX < 0.05f) g_cfg.sensX = 0.05f;
        if (g_cfg.sensY < 0.05f) g_cfg.sensY = 0.05f;
        Mod_SaveConfig();
        lastKeyTick = now;
        Mod_WriteLog("[HOTKEY Sens-] Sensibilidad: X=%.3f Y=%.3f\n", g_cfg.sensX, g_cfg.sensY);
    }
    else if (IsShortcutHeldNow(g_cfg.keySensUp))
    {
        g_cfg.sensX *= 1.10f;
        g_cfg.sensY *= 1.10f;
        if (g_cfg.sensX > 100.0f) g_cfg.sensX = 100.0f;
        if (g_cfg.sensY > 100.0f) g_cfg.sensY = 100.0f;
        Mod_SaveConfig();
        lastKeyTick = now;
        Mod_WriteLog("[HOTKEY Sens+] Sensibilidad: X=%.3f Y=%.3f\n", g_cfg.sensX, g_cfg.sensY);
    }
}

static bool ApplyXboxAction(uint8_t *param_1, int action)
{
    switch (action)
    {
        case XACT_RT:         param_1[9] = 0xFF;  return true;
        case XACT_LT:         param_1[8] = 0xFF;  return true;
        case XACT_A:          param_1[2] = 0xFF;  return true;
        case XACT_B:          param_1[3] = 0xFF;  return true;
        case XACT_X:          param_1[4] = 0xFF;  return true;
        case XACT_Y:          param_1[5] = 0xFF;  return true;
        case XACT_BLACK:      param_1[6] = 0xFF;  return true;
        case XACT_WHITE:      param_1[7] = 0xFF;  return true;
        case XACT_L3:         param_1[0] |= 0x40; return true;
        case XACT_R3:         param_1[0] |= 0x80; return true;
        case XACT_DPAD_UP:    param_1[0] |= 0x01; return true;
        case XACT_DPAD_DOWN:  param_1[0] |= 0x02; return true;
        case XACT_DPAD_LEFT:  param_1[0] |= 0x04; return true;
        case XACT_DPAD_RIGHT: param_1[0] |= 0x08; return true;
        case XACT_START:      param_1[0] |= 0x10; return true;
        case XACT_BACK:       param_1[0] |= 0x20; return true;
        default:              return false;
    }
}

// ---------------------------------------------------------------------------
// HOOK 1: Mando (Lectura constante de clics y caminata lenta con Alt)
// ---------------------------------------------------------------------------
static uint64_t Hooked_Input(uint8_t *param_1, uint64_t param_2, uint64_t param_3, uint8_t param_4)
{
    // Mientras la GUI este abierta, bloquear input hacia el juego (mismo PID)
    if (g_guiOpen)
    {
        UpdateCursorVisibility(false);
        g_lastCamTick = 0;
        if (param_1) memset(param_1, 0, 18);
        return 0;
    }

    uint64_t ret = Real_FUN_input(param_1, param_2, param_3, param_4);
    if (!param_1) return ret;

    HWND fgHwnd = NULL;
    bool isFocused = IsGameWindowFocused(&fgHwnd);
    if (isFocused) CheckHotkeys();

    if (!g_mouseEnabled || !isFocused)
    {
        UpdateCursorVisibility(false);
        g_lastCamTick = 0;
        return ret;
    }

    UpdateCursorVisibility(true);

    // Leer modo de camara actual si la memoria de Xbox esta lista
    int32_t camMode = -1;
    uint64_t lVar = *(uint64_t *)ADDR_XBOX_RAM_BASE_PTR;
    if (lVar != 0)
    {
        uint32_t camMgr = *(uint32_t *)(lVar + 0x323034);
        if (camMgr != 0)
        {
            uint32_t camState = *(uint32_t *)(lVar + (uint64_t)camMgr);
            if (camState != 0)
            {
                camMode = *(int32_t *)(lVar + (uint64_t)camState + 0x38);

                if (camMode != g_lastCamMode)
                {
                    Mod_WriteLog("[CAM_MODE] %d -> %d\n", g_lastCamMode, camMode);
                    if (g_lastCamMode > 0 && camMode == 0)
                    {
                        g_justReturnedToMode0 = true;
                    }
                    g_lastCamMode = camMode;
                }
            }
        }
    }

    // En Modo 0 (gameplay o arsenal), stick derecho a 0 para que no interfiera
    if (camMode == 0)
    {
        *(int16_t *)(param_1 + 0x0E) = 0;
        *(int16_t *)(param_1 + 0x10) = 0;
    }

    // CAMINATA LENTA (Walk Key):
    // Solo se aplica si la camara 3D esta activa en tiempo real (GetTickCount64() - g_lastCamFrameTick <= 120).
    // Si la camara esta pausada (por ejemplo en el arsenal o ruleta de armas), deja WASD al 100% para que nunca se trabe!
    bool in3dGameplay = (g_lastCamFrameTick != 0 && (GetTickCount64() - g_lastCamFrameTick <= 120));
    if (camMode == 0 && in3dGameplay && IsShortcutHeldNow(g_cfg.keyWalk))
    {
        int16_t *pLX = (int16_t *)(param_1 + 0x0A);
        int16_t *pLY = (int16_t *)(param_1 + 0x0C);
        *pLX = (int16_t)((float)*pLX * g_cfg.walkSpeed);
        *pLY = (int16_t)((float)*pLY * g_cfg.walkSpeed);
    }

    bool injected = false;

    // Modo 4 (Headshot Bullet Cam): quieto como el original, solo centra cursor
    if (camMode == 4)
    {
        int dx = 0, dy = 0;
        PollMouseDelta(&dx, &dy);
    }
    // Modos 5 y 3 (Ejecuciones): orbita opcional via stick virtual con signo Y corregido
    else if (g_cfg.execCamOrbit && (camMode == 5 || camMode == 3))
    {
        int dx = 0, dy = 0;
        PollMouseDelta(&dx, &dy);

        if (dx != 0 || dy != 0)
        {
            float radSensX = g_cfg.sensX * BASE_SENS_RAD_PER_PX;
            float radSensY = g_cfg.sensY * BASE_SENS_RAD_PER_PX;
            float signX = g_cfg.invertX ? -1.0f : 1.0f;
            float signY = g_cfg.invertY ? -1.0f : 1.0f;

            int stickX = (int)((float)dx * radSensX * signX * 1200000.0f);
            int stickY = (int)((float)(-dy) * radSensY * signY * 1200000.0f);

            if (stickX > 32767)  stickX = 32767;
            if (stickX < -32767) stickX = -32767;
            if (stickY > 32767)  stickY = 32767;
            if (stickY < -32767) stickY = -32767;

            *(int16_t *)(param_1 + 0x0E) = (int16_t)stickX;
            *(int16_t *)(param_1 + 0x10) = (int16_t)stickY;
            injected = true;
        }
    }

    // BINDS DE BOTONES DEL MOUSE (Left Click -> RT, Right Click -> Y, etc.)
    // SIEMPRE ACTIVOS: funciona en combate y en el arsenal para equipar dual-wield con clic derecho
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
    {
        if (ApplyXboxAction(param_1, g_cfg.leftClickAction)) injected = true;
    }
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000)
    {
        if (ApplyXboxAction(param_1, g_cfg.rightClickAction)) injected = true;
    }
    for (int i = 0; i < g_cfg.extraBindCount; i++)
    {
        int vk = g_cfg.extraBinds[i].vk;
        if (vk != 0 && (GetAsyncKeyState(vk) & 0x8000))
        {
            if (ApplyXboxAction(param_1, g_cfg.extraBinds[i].action)) injected = true;
        }
    }

    return injected ? (ret | 1ULL) : ret;
}

// ---------------------------------------------------------------------------
// HOOK 2: Camara 3D en 3ra Persona (Modo 0 - Control 1:1 Directo en RAM)
// ---------------------------------------------------------------------------
static void Hooked_Camera(void)
{
    g_lastCamFrameTick = GetTickCount64();

    uint64_t lVar    = *(uint64_t *)ADDR_XBOX_RAM_BASE_PTR;
    uint32_t thisPtr = *(uint32_t *)ADDR_REG_ECX_THIS;
    uint32_t esp     = *(uint32_t *)ADDR_REG_ESP;

    if (!g_camHookAliveLogged && lVar != 0 && thisPtr != 0 && esp != 0)
    {
        g_camHookAliveLogged = true;
        uint32_t dbgAngles = *(uint32_t *)(lVar + (uint64_t)esp + 4ULL);
        Mod_WriteLog("[CAM_HOOK_ALIVE] lVar=0x%llX | this=0x%08X | esp=0x%08X | angles=0x%08X\n",
                     (unsigned long long)lVar, thisPtr, esp, dbgAngles);
    }

    if (!g_mouseEnabled || g_guiOpen || lVar == 0 || thisPtr == 0 || esp == 0)
    {
        Real_FUN_camera();
        return;
    }

    uint32_t anglesAddr = *(uint32_t *)(lVar + (uint64_t)esp + 4ULL);
    if (anglesAddr == 0)
    {
        Real_FUN_camera();
        return;
    }

    int dx = 0, dy = 0;
    PollMouseDelta(&dx, &dy);

    float *pPitch = (float *)(lVar + (uint64_t)anglesAddr + 0x00ULL);
    float *pYaw   = (float *)(lVar + (uint64_t)anglesAddr + 0x04ULL);

    if (g_justReturnedToMode0)
    {
        *pPitch = g_savedMode0Pitch;
        g_justReturnedToMode0 = false;
    }
    else
    {
        g_savedMode0Pitch = *pPitch;
    }

    float   *pVelPitch      = (float *)(lVar + (uint64_t)thisPtr + 0x1C8ULL);
    float   *pVelYaw        = (float *)(lVar + (uint64_t)thisPtr + 0x1CCULL);
    float   *pVelAbs        = (float *)(lVar + (uint64_t)thisPtr + 0x1D0ULL);
    float   *pJumpRecenterV = (float *)(lVar + (uint64_t)thisPtr + 0x25CULL);
    uint8_t *pJumpRecenterF = (uint8_t *)(lVar + (uint64_t)thisPtr + 0x264ULL);

    float oldPitch = *pPitch;
    float oldYaw   = *pYaw;

    float radSensX = g_cfg.sensX * BASE_SENS_RAD_PER_PX;
    float radSensY = g_cfg.sensY * BASE_SENS_RAD_PER_PX;
    float signX = g_cfg.invertX ? -1.0f : 1.0f;
    float signY = g_cfg.invertY ? -1.0f : 1.0f;

    float deltaYaw   = (float)dx * radSensX * signX;
    float deltaPitch = (float)dy * radSensY * signY;

    *pVelPitch      = 0.0f;
    *pVelYaw        = 0.0f;
    *pVelAbs        = 0.0f;
    *pJumpRecenterV = 0.0f;
    *pJumpRecenterF = 0;

    float preGamePitch = oldPitch + deltaPitch;
    if (g_cfg.removeHostageLimit)
    {
        if (preGamePitch > 1.48f)  preGamePitch = 1.48f;
        if (preGamePitch < -1.48f) preGamePitch = -1.48f;
    }

    *pPitch = preGamePitch;
    float targetYaw = oldYaw + deltaYaw;

    Real_FUN_camera();

    if (g_cfg.removeHostageLimit)
    {
        *pPitch = preGamePitch;
    }

    float quickTurnOffset = *(float *)(lVar + (uint64_t)thisPtr + 0x2A8ULL);
    *pYaw = targetYaw + quickTurnOffset;

    *pVelPitch      = 0.0f;
    *pVelYaw        = 0.0f;
    *pVelAbs        = 0.0f;
    *pJumpRecenterV = 0.0f;
    *pJumpRecenterF = 0;

    if (g_camMoveLogCount < 5 && (dx != 0 || dy != 0))
    {
        g_camMoveLogCount++;
        Mod_WriteLog("[CAM_OK #%d] dx=%d dy=%d | Pitch: %.4f -> %.4f | Yaw: %.4f -> %.4f\n",
                     g_camMoveLogCount, dx, dy, oldPitch, *pPitch, oldYaw, *pYaw);
    }
}

// ============================================================================
// ENGINE DE HOOKS (CALL 5 BYTES SEGURO, COMPATIBLE CON ASLR)
// ============================================================================
static uint8_t *g_relayPage = NULL;
static size_t   g_relayUsed = 0;

static bool IsWithinRel32(uintptr_t fromNextRip, uintptr_t target)
{
    int64_t delta = (int64_t)target - (int64_t)fromNextRip;
    return (delta >= -0x7FFF0000LL && delta <= 0x7FFF0000LL);
}

static void *AllocateRelayStub(uintptr_t callSiteAddr, void *targetFunc)
{
    if (!g_relayPage || !IsWithinRel32(callSiteAddr + 5, (uintptr_t)g_relayPage))
    {
        g_relayPage = NULL;
        g_relayUsed = 0;

        uintptr_t startUp = (g_imgEnd + 0xFFFFULL) & ~0xFFFFULL;
        uintptr_t maxUp   = g_imgBase + 0x60000000ULL;
        for (uintptr_t addr = startUp; addr < maxUp; addr += 0x10000ULL)
        {
            if (!IsWithinRel32(callSiteAddr + 5, addr)) break;
            void *p = VirtualAlloc((void *)addr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p)
            {
                g_relayPage = (uint8_t *)p;
                break;
            }
        }

        if (!g_relayPage && g_imgBase > 0x10000ULL)
        {
            uintptr_t startDown = (g_imgBase & ~0xFFFFULL) - 0x10000ULL;
            uintptr_t minDown   = (g_imgBase > 0x60000000ULL) ? (g_imgBase - 0x60000000ULL) : 0x10000ULL;
            for (uintptr_t addr = startDown; addr >= minDown; addr -= 0x10000ULL)
            {
                if (!IsWithinRel32(callSiteAddr + 5, addr)) break;
                void *p = VirtualAlloc((void *)addr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (p)
                {
                    g_relayPage = (uint8_t *)p;
                    break;
                }
                if (addr < 0x10000ULL) break;
            }
        }
    }

    if (!g_relayPage || (g_relayUsed + 16 > 0x1000)) return NULL;

    uint8_t *stub = g_relayPage + g_relayUsed;
    g_relayUsed += 16;

    stub[0] = 0xFF; stub[1] = 0x25; stub[2] = 0x00;
    stub[3] = 0x00; stub[4] = 0x00; stub[5] = 0x00;
    *(uint64_t *)(stub + 6) = (uint64_t)targetFunc;

    return stub;
}

static bool InstallCallHook(uintptr_t callSiteAddr, void *hookFunc, void **realFuncPtr, const char *name)
{
    uint8_t *site = (uint8_t *)callSiteAddr;
    if (site[0] != 0xE8)
    {
        Mod_WriteLog("[ERROR] %s en 0x%llX no empieza con 0xE8.\n", name, (unsigned long long)callSiteAddr);
        return false;
    }

    *realFuncPtr = (void *)(callSiteAddr + 5 + *(int32_t *)(site + 1));

    void *stub = AllocateRelayStub(callSiteAddr, hookFunc);
    if (!stub || !IsWithinRel32(callSiteAddr + 5, (uintptr_t)stub))
    {
        Mod_WriteLog("[ERROR] No se pudo asignar relay stub cercano para %s.\n", name);
        return false;
    }

    DWORD oldProt = 0;
    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProt);
    *(int32_t *)(site + 1) = (int32_t)((int64_t)stub - ((int64_t)callSiteAddr + 5));
    VirtualProtect(site, 5, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), site, 5);

    Mod_WriteLog("[OK] Hook instalado en %s (CallSite: 0x%llX -> Destino: 0x%llX)\n",
                 name, (unsigned long long)callSiteAddr, (unsigned long long)*realFuncPtr);
    return true;
}

static DWORD WINAPI ModInitThread(LPVOID lpParam)
{
    if (InterlockedCompareExchange(&g_threadCreated, 1, 0) != 0)
        return 0;

    LoadOrCreateConfig();
    EnsureRealXInputLoaded();

    if (g_cfg.debugLog)
    {
        FILE *f = fopen(g_logPath, "w");
        if (f)
        {
            char menuTxt[48], togTxt[48];
            FormatShortcutText(g_cfg.keyOpenMenu, menuTxt, sizeof(menuTxt));
            FormatShortcutText(g_cfg.keyToggleMod, togTxt, sizeof(togTxt));
            fprintf(f, "=========================================================\n");
            fprintf(f, "50 CENT BULLETPROOF MOUSE MOD v1.0 (UNIVERSAL V1/V2+)\n");
            fprintf(f, "Menu: %s | Toggle Mod: %s\n", menuTxt, togTxt);
            fprintf(f, "=========================================================\n");
            fclose(f);
        }
    }

    if (ScanUniversalAddresses())
    {
        InstallCallHook(CALLSITE_CAM_ANGLES, (void *)&Hooked_Camera, (void **)&Real_FUN_camera, "Camera");
        InstallCallHook(CALLSITE_INPUT,      (void *)&Hooked_Input,  (void **)&Real_FUN_input,  "Input");
    }
    else
    {
        Mod_WriteLog("[FATAL] No se pudieron resolver las direcciones universales.\n");
    }

    Gui_StartThread();

    return 0;
}

// ============================================================================
// EXPORTACIONES PROXY XINPUT (Reenvio real a System32\xinput1_4.dll)
// ============================================================================
extern "C" {

DWORD WINAPI XInputGetState(DWORD dwUserIndex, void *pState)
{
    EnsureRealXInputLoaded();
    if (!g_realXInputGetState)
    {
        if (pState) memset(pState, 0, 16);
        return 1167;
    }

    DWORD res = g_realXInputGetState(dwUserIndex, pState);
    if (res == ERROR_SUCCESS && pState)
    {
        if (g_guiOpen)
        {
            memset((uint8_t *)pState + 4, 0, 12);
        }
        else if (g_mouseEnabled && g_lastCamMode == 0)
        {
            *(int16_t *)((uint8_t *)pState + 12) = 0; // sThumbRX
            *(int16_t *)((uint8_t *)pState + 14) = 0; // sThumbRY
        }
    }
    return res;
}

DWORD WINAPI XInputSetState(DWORD dwUserIndex, void *pVibration)
{
    EnsureRealXInputLoaded();
    if (!g_realXInputSetState)
        return 1167;
    return g_realXInputSetState(dwUserIndex, pVibration);
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        char exePath[MAX_PATH] = {0};
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        if (strstr(exePath, "50 Cent") != NULL || strstr(exePath, "50 cent") != NULL)
        {
            CreateThread(NULL, 0, ModInitThread, NULL, 0, NULL);
        }
    }
    return TRUE;
}
