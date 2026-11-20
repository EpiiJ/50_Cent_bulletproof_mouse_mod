#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "mouse_mod_gui.h"
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *URL_GITHUB = "https://github.com/EpiiJ";

// Colores Dark Theme
static const COLORREF COL_BG        = RGB(37, 37, 38);    // #252526
static const COLORREF COL_CTRL_BG   = RGB(30, 30, 30);    // #1E1E1E
static const COLORREF COL_BTN_BG    = RGB(51, 51, 55);    // #333337
static const COLORREF COL_BTN_PRESS = RGB(68, 68, 74);
static const COLORREF COL_BORDER    = RGB(68, 68, 74);    // #44444A
static const COLORREF COL_ACCENT    = RGB(0, 122, 204);   // #007ACC
static const COLORREF COL_TEXT      = RGB(241, 241, 241); // #F1F1F1
static const COLORREF COL_TEXT_DIM  = RGB(175, 175, 180);

// IDs de controles
enum ControlIds
{
    IDC_TAB_CONFIG = 1001,
    IDC_TAB_ABOUT  = 1002,

    IDC_SLIDER_SENS_X   = 1101,
    IDC_EDIT_SENS_X     = 1102,
    IDC_SLIDER_SENS_Y   = 1103,
    IDC_EDIT_SENS_Y     = 1104,
    IDC_SLIDER_WALK_SPD = 1105,
    IDC_EDIT_WALK_SPD   = 1106,

    IDC_CHK_INVERT_X    = 1201,
    IDC_CHK_INVERT_Y    = 1202,
    IDC_CHK_EXEC_ORBIT  = 1203,
    IDC_CHK_HOSTAGE_LIM = 1204,
    IDC_CHK_DEBUG_LOG   = 1205,

    IDC_BTN_ADD_BIND    = 1301,
    IDC_CMB_LEFT_CLICK  = 1302,
    IDC_CMB_RIGHT_CLICK = 1303,
    IDC_CMB_EXTRA_BASE  = 1310, // 1310..1312
    IDC_BTN_DEL_BASE    = 1320, // 1320..1322

    IDC_BTN_KEY_MENU    = 1401,
    IDC_BTN_KEY_TOGGLE  = 1402,
    IDC_BTN_KEY_SENS_UP = 1403,
    IDC_BTN_KEY_SENS_DN = 1404,
    IDC_BTN_KEY_WALK    = 1405,

    IDC_BTN_RESET       = 1501,

    IDC_BTN_GITHUB      = 1601
};

static const UINT_PTR TIMER_POLL = 1;

static HBRUSH g_brBg     = NULL;
static HBRUSH g_brCtrlBg = NULL;
static HFONT  g_fontNorm = NULL;
static HFONT  g_fontBold = NULL;
static HFONT  g_fontBig  = NULL;
static HFONT  g_fontSm   = NULL;

static int  g_activeTab          = 0;  // 0 = Config, 1 = About
static bool g_updatingUi         = false;
static int  g_captureShortcutIdx = -1; // 0=Menu, 1=Toggle, 2=SensUp, 3=SensDown, 4=Walk
static bool g_capturingMouseBtn  = false;
static HWND g_prevGameHwnd       = NULL;

// Handles de controles
static HWND hTabConfig, hTabAbout;
static HWND hSliderX, hEditX, hSliderY, hEditY;
static HWND hSliderWalk, hEditWalk;
static HWND hChkInvX, hChkInvY, hChkExec, hChkHostage, hChkLog;
static HWND hBtnAddBind, hCmbLeft, hCmbRight;
static HWND hCmbExtra[MAX_EXTRA_MOUSE_BINDS];
static HWND hBtnDelExtra[MAX_EXTRA_MOUSE_BINDS];
static HWND hBtnKeyMenu, hBtnKeyToggle, hBtnKeyUp, hBtnKeyDown, hBtnKeyWalk;
static HWND hBtnReset;
static HWND hBtnGitHub;

static void FormatSensText(float val, char *outBuf, size_t outSize)
{
    int scaled = (int)(val * 1000.0f + 0.5f);
    if ((scaled % 10) != 0)
        snprintf(outBuf, outSize, "%.3f", val);
    else
        snprintf(outBuf, outSize, "%.2f", val);
}

static void EnableDarkTitleBar(HWND hwnd)
{
    HMODULE hDwm = LoadLibraryA("dwmapi.dll");
    if (!hDwm) return;
    typedef HRESULT (WINAPI *DwmSetWindowAttribute_t)(HWND, DWORD, LPCVOID, DWORD);
    DwmSetWindowAttribute_t fn = (DwmSetWindowAttribute_t)GetProcAddress(hDwm, "DwmSetWindowAttribute");
    if (fn)
    {
        BOOL dark = TRUE;
        if (FAILED(fn(hwnd, 20, &dark, sizeof(dark))))
        {
            fn(hwnd, 19, &dark, sizeof(dark));
        }
    }
    FreeLibrary(hDwm);
}

static void PopulateActionCombo(HWND hCmb, int selectedAction)
{
    SendMessageA(hCmb, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < XACT_COUNT; i++)
    {
        SendMessageA(hCmb, CB_ADDSTRING, 0, (LPARAM)GetXboxActionName(i));
    }
    if (selectedAction < 0 || selectedAction >= XACT_COUNT) selectedAction = XACT_NONE;
    SendMessageA(hCmb, CB_SETCURSEL, (WPARAM)selectedAction, 0);
}

static int ClampSliderPos(float sens)
{
    int pos = (int)(sens * 100.0f + 0.5f);
    if (pos < 10)   pos = 10;
    if (pos > 1000) pos = 1000;
    return pos;
}

static void RefreshShortcutButtons()
{
    char buf[64];

    if (g_captureShortcutIdx == 0) SetWindowTextA(hBtnKeyMenu, "Press key...");
    else { FormatShortcutText(g_cfg.keyOpenMenu, buf, sizeof(buf)); SetWindowTextA(hBtnKeyMenu, buf); }

    if (g_captureShortcutIdx == 1) SetWindowTextA(hBtnKeyToggle, "Press key...");
    else { FormatShortcutText(g_cfg.keyToggleMod, buf, sizeof(buf)); SetWindowTextA(hBtnKeyToggle, buf); }

    if (g_captureShortcutIdx == 2) SetWindowTextA(hBtnKeyUp, "Press key...");
    else { FormatShortcutText(g_cfg.keySensUp, buf, sizeof(buf)); SetWindowTextA(hBtnKeyUp, buf); }

    if (g_captureShortcutIdx == 3) SetWindowTextA(hBtnKeyDown, "Press key...");
    else { FormatShortcutText(g_cfg.keySensDown, buf, sizeof(buf)); SetWindowTextA(hBtnKeyDown, buf); }

    if (g_captureShortcutIdx == 4) SetWindowTextA(hBtnKeyWalk, "Press key...");
    else { FormatShortcutText(g_cfg.keyWalk, buf, sizeof(buf)); SetWindowTextA(hBtnKeyWalk, buf); }
}

static void RefreshExtraBindControls()
{
    bool showConfig = (g_activeTab == 0);
    for (int i = 0; i < MAX_EXTRA_MOUSE_BINDS; i++)
    {
        bool active = (i < g_cfg.extraBindCount);
        ShowWindow(hCmbExtra[i], (showConfig && active) ? SW_SHOW : SW_HIDE);
        ShowWindow(hBtnDelExtra[i], (showConfig && active) ? SW_SHOW : SW_HIDE);
        if (active)
        {
            SendMessageA(hCmbExtra[i], CB_SETCURSEL, (WPARAM)g_cfg.extraBinds[i].action, 0);
        }
    }

    if (g_capturingMouseBtn)
        SetWindowTextA(hBtnAddBind, "Click M3/M4/M5...");
    else
        SetWindowTextA(hBtnAddBind, "+ Add Mouse Button");

    EnableWindow(hBtnAddBind, (g_cfg.extraBindCount < MAX_EXTRA_MOUSE_BINDS) || g_capturingMouseBtn);
}

static void SyncUiFromConfig()
{
    g_updatingUi = true;

    char buf[32];
    FormatSensText(g_cfg.sensX, buf, sizeof(buf));
    SetWindowTextA(hEditX, buf);
    SendMessageA(hSliderX, TBM_SETPOS, TRUE, ClampSliderPos(g_cfg.sensX));

    FormatSensText(g_cfg.sensY, buf, sizeof(buf));
    SetWindowTextA(hEditY, buf);
    SendMessageA(hSliderY, TBM_SETPOS, TRUE, ClampSliderPos(g_cfg.sensY));

    snprintf(buf, sizeof(buf), "%.2f", g_cfg.walkSpeed);
    SetWindowTextA(hEditWalk, buf);
    int wPos = (int)(g_cfg.walkSpeed * 100.0f + 0.5f);
    if (wPos < 34)  wPos = 34;
    if (wPos > 100) wPos = 100;
    SendMessageA(hSliderWalk, TBM_SETPOS, TRUE, wPos);

    SendMessageA(hCmbLeft,  CB_SETCURSEL, (WPARAM)g_cfg.leftClickAction, 0);
    SendMessageA(hCmbRight, CB_SETCURSEL, (WPARAM)g_cfg.rightClickAction, 0);

    RefreshExtraBindControls();
    RefreshShortcutButtons();

    InvalidateRect(g_guiHwnd, NULL, TRUE);
    g_updatingUi = false;
}

static void UpdateTabVisibility()
{
    int showCfg   = (g_activeTab == 0) ? SW_SHOW : SW_HIDE;
    int showAbout = (g_activeTab == 1) ? SW_SHOW : SW_HIDE;

    ShowWindow(hSliderX, showCfg);
    ShowWindow(hEditX, showCfg);
    ShowWindow(hSliderY, showCfg);
    ShowWindow(hEditY, showCfg);

    ShowWindow(hSliderWalk, showCfg);
    ShowWindow(hEditWalk, showCfg);

    ShowWindow(hChkInvX, showCfg);
    ShowWindow(hChkInvY, showCfg);
    ShowWindow(hChkExec, showCfg);
    ShowWindow(hChkHostage, showCfg);
    ShowWindow(hChkLog, showCfg);

    ShowWindow(hBtnAddBind, showCfg);
    ShowWindow(hCmbLeft, showCfg);
    ShowWindow(hCmbRight, showCfg);

    ShowWindow(hBtnKeyMenu, showCfg);
    ShowWindow(hBtnKeyToggle, showCfg);
    ShowWindow(hBtnKeyUp, showCfg);
    ShowWindow(hBtnKeyDown, showCfg);
    ShowWindow(hBtnKeyWalk, showCfg);
    ShowWindow(hBtnReset, showCfg);

    RefreshExtraBindControls();

    ShowWindow(hBtnGitHub, showAbout);

    InvalidateRect(g_guiHwnd, NULL, TRUE);
}

static void CancelAllCaptures()
{
    if (g_captureShortcutIdx != -1)
    {
        g_captureShortcutIdx = -1;
        RefreshShortcutButtons();
    }
    if (g_capturingMouseBtn)
    {
        g_capturingMouseBtn = false;
        RefreshExtraBindControls();
    }
    if (g_guiHwnd) InvalidateRect(g_guiHwnd, NULL, FALSE);
}

static void AddExtraMouseButton(int vk)
{
    g_capturingMouseBtn = false;

    for (int i = 0; i < g_cfg.extraBindCount; i++)
    {
        if (g_cfg.extraBinds[i].vk == vk)
        {
            RefreshExtraBindControls();
            InvalidateRect(g_guiHwnd, NULL, TRUE);
            return;
        }
    }

    if (g_cfg.extraBindCount < MAX_EXTRA_MOUSE_BINDS)
    {
        int idx = g_cfg.extraBindCount++;
        g_cfg.extraBinds[idx].vk     = vk;
        g_cfg.extraBinds[idx].action = XACT_NONE;
        Mod_SaveConfig();
    }

    RefreshExtraBindControls();
    InvalidateRect(g_guiHwnd, NULL, TRUE);
}

static void RemoveExtraMouseButton(int index)
{
    if (index < 0 || index >= g_cfg.extraBindCount) return;
    for (int i = index; i + 1 < g_cfg.extraBindCount; i++)
    {
        g_cfg.extraBinds[i] = g_cfg.extraBinds[i + 1];
    }
    g_cfg.extraBindCount--;
    Mod_SaveConfig();
    RefreshExtraBindControls();
    InvalidateRect(g_guiHwnd, NULL, TRUE);
}

static bool IsControlChecked(int ctrlId)
{
    switch (ctrlId)
    {
        case IDC_CHK_INVERT_X:    return g_cfg.invertX != 0;
        case IDC_CHK_INVERT_Y:    return g_cfg.invertY != 0;
        case IDC_CHK_EXEC_ORBIT:  return g_cfg.execCamOrbit != 0;
        case IDC_CHK_HOSTAGE_LIM: return g_cfg.removeHostageLimit != 0;
        case IDC_CHK_DEBUG_LOG:   return g_cfg.debugLog != 0;
        default: return false;
    }
}

static bool IsCheckboxControl(int ctrlId)
{
    return (ctrlId >= IDC_CHK_INVERT_X && ctrlId <= IDC_CHK_DEBUG_LOG);
}

static void DrawOwnerButton(DRAWITEMSTRUCT *dis)
{
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    int id = (int)dis->CtlID;
    bool pressed  = (dis->itemState & ODS_SELECTED) != 0;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;

    char text[128] = {0};
    GetWindowTextA(dis->hwndItem, text, sizeof(text));

    SetBkMode(hdc, TRANSPARENT);

    // 1. Pestanas superiores
    if (id == IDC_TAB_CONFIG || id == IDC_TAB_ABOUT)
    {
        bool active = ((id == IDC_TAB_CONFIG && g_activeTab == 0) ||
                       (id == IDC_TAB_ABOUT  && g_activeTab == 1));
        HBRUSH br = CreateSolidBrush(active ? COL_BG : COL_CTRL_BG);
        FillRect(hdc, &rc, br);
        DeleteObject(br);

        RECT bar = rc;
        bar.top = rc.bottom - (active ? 3 : 1);
        HBRUSH brBar = CreateSolidBrush(active ? COL_ACCENT : COL_BORDER);
        FillRect(hdc, &bar, brBar);
        DeleteObject(brBar);

        SelectObject(hdc, active ? g_fontBold : g_fontNorm);
        SetTextColor(hdc, active ? COL_TEXT : COL_TEXT_DIM);
        DrawTextA(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    // 2. Checkboxes custom (Dark Mode legible)
    if (IsCheckboxControl(id))
    {
        FillRect(hdc, &rc, g_brBg);
        bool checked = IsControlChecked(id);

        int boxSz = 15;
        int boxY = rc.top + ((rc.bottom - rc.top) - boxSz) / 2;
        RECT box = { rc.left + 1, boxY, rc.left + 1 + boxSz, boxY + boxSz };

        HBRUSH brBox = CreateSolidBrush(checked ? COL_ACCENT : COL_CTRL_BG);
        FillRect(hdc, &box, brBox);
        DeleteObject(brBox);

        HPEN penBox = CreatePen(PS_SOLID, 1, checked ? COL_ACCENT : RGB(110, 110, 118));
        HGDIOBJ oldPen = SelectObject(hdc, penBox);
        HGDIOBJ oldBr  = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, box.left, box.top, box.right, box.bottom);
        SelectObject(hdc, oldBr);
        SelectObject(hdc, oldPen);
        DeleteObject(penBox);

        if (checked)
        {
            HPEN penTick = CreatePen(PS_SOLID, 2, COL_TEXT);
            oldPen = SelectObject(hdc, penTick);
            MoveToEx(hdc, box.left + 3, box.top + 7, NULL);
            LineTo(hdc, box.left + 6, box.top + 11);
            LineTo(hdc, box.left + 12, box.top + 4);
            SelectObject(hdc, oldPen);
            DeleteObject(penTick);
        }

        RECT txtRc = rc;
        txtRc.left = box.right + 7;
        SelectObject(hdc, g_fontNorm);
        SetTextColor(hdc, COL_TEXT);
        DrawTextA(hdc, text, -1, &txtRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    // 3. Botones estandar Dark Mode
    bool isCapturingThis =
        (id == IDC_BTN_KEY_MENU    && g_captureShortcutIdx == 0) ||
        (id == IDC_BTN_KEY_TOGGLE  && g_captureShortcutIdx == 1) ||
        (id == IDC_BTN_KEY_SENS_UP && g_captureShortcutIdx == 2) ||
        (id == IDC_BTN_KEY_SENS_DN && g_captureShortcutIdx == 3) ||
        (id == IDC_BTN_KEY_WALK    && g_captureShortcutIdx == 4) ||
        (id == IDC_BTN_ADD_BIND    && g_capturingMouseBtn);

    COLORREF bgCol = COL_BTN_BG;
    COLORREF bdCol = COL_BORDER;
    if (isCapturingThis)
    {
        bgCol = COL_ACCENT;
        bdCol = COL_ACCENT;
    }
    else if (pressed)
    {
        bgCol = COL_BTN_PRESS;
    }
    else if (id == IDC_BTN_GITHUB)
    {
        bgCol = COL_ACCENT;
        bdCol = COL_ACCENT;
    }
    else if (id >= IDC_BTN_DEL_BASE && id < IDC_BTN_DEL_BASE + MAX_EXTRA_MOUSE_BINDS)
    {
        bgCol = RGB(90, 42, 42);
        bdCol = RGB(130, 58, 58);
    }

    HBRUSH br = CreateSolidBrush(bgCol);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    HPEN pen = CreatePen(PS_SOLID, 1, bdCol);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBr  = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    SelectObject(hdc, (id == IDC_BTN_GITHUB) ? g_fontBold : g_fontNorm);
    SetTextColor(hdc, disabled ? COL_TEXT_DIM : COL_TEXT);
    DrawTextA(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void DrawMainWindowPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT clientRc;
    GetClientRect(hwnd, &clientRc);
    FillRect(hdc, &clientRc, g_brBg);

    SetBkMode(hdc, TRANSPARENT);

    if (g_activeTab == 0)
    {
        // Sensibilidad
        SelectObject(hdc, g_fontBold);
        SetTextColor(hdc, COL_TEXT);
        RECT rSec1 = { 16, 44, 438, 62 };
        DrawTextA(hdc, "Sensitivity (1.00 = Default)", -1, &rSec1, DT_LEFT | DT_SINGLELINE);

        SelectObject(hdc, g_fontNorm);
        RECT rLblX = { 16, 70, 120, 92 };
        DrawTextA(hdc, "Horizontal (X):", -1, &rLblX, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT rLblY = { 16, 100, 120, 122 };
        DrawTextA(hdc, "Vertical (Y):", -1, &rLblY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        HPEN penSep = CreatePen(PS_SOLID, 1, COL_BORDER);
        HGDIOBJ oldPen = SelectObject(hdc, penSep);

        // Separador 1
        MoveToEx(hdc, 16, 130, NULL); LineTo(hdc, 438, 130);

        // Separador 2
        MoveToEx(hdc, 16, 194, NULL); LineTo(hdc, 438, 194);

        // Titulo Mouse Binds
        SelectObject(hdc, g_fontBold);
        RECT rSecBinds = { 16, 204, 250, 224 };
        DrawTextA(hdc, "Mouse Binds", -1, &rSecBinds, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, g_fontNorm);
        RECT rL = { 20, 232, 135, 254 };
        DrawTextA(hdc, "Left Click:", -1, &rL, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT rR = { 20, 260, 135, 282 };
        DrawTextA(hdc, "Right Click:", -1, &rR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        if (g_cfg.extraBindCount == 0)
        {
            SelectObject(hdc, g_fontSm);
            SetTextColor(hdc, COL_TEXT_DIM);
            RECT rHint = { 20, 296, 436, 336 };
            DrawTextA(hdc, "Click \"+ Add Mouse Button\" to bind Middle Click, Mouse 4 or Mouse 5.", -1, &rHint, DT_LEFT | DT_WORDBREAK);
            SetTextColor(hdc, COL_TEXT);
        }
        else
        {
            SelectObject(hdc, g_fontNorm);
            for (int i = 0; i < g_cfg.extraBindCount; i++)
            {
                int y = 288 + i * 28;
                char lbl[48];
                snprintf(lbl, sizeof(lbl), "%s:", GetMouseButtonName(g_cfg.extraBinds[i].vk));
                RECT rEx = { 20, y, 135, y + 22 };
                DrawTextA(hdc, lbl, -1, &rEx, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
        }

        // Separador 3
        MoveToEx(hdc, 16, 376, NULL); LineTo(hdc, 438, 376);

        // Titulo Shortcuts
        SelectObject(hdc, g_fontBold);
        RECT rSecKeys = { 16, 384, 360, 404 };
        DrawTextA(hdc, "Shortcuts (Click to rebind | Esc to cancel)", -1, &rSecKeys, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, g_fontNorm);
        RECT rK1 = { 16, 410, 102, 434 };
        DrawTextA(hdc, "Open Menu:", -1, &rK1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT rK2 = { 228, 410, 314, 434 };
        DrawTextA(hdc, "Toggle Mod:", -1, &rK2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT rK3 = { 16, 440, 102, 464 };
        DrawTextA(hdc, "Sens Up:", -1, &rK3, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT rK4 = { 228, 440, 314, 464 };
        DrawTextA(hdc, "Sens Down:", -1, &rK4, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT rK5 = { 16, 470, 102, 494 };
        DrawTextA(hdc, "Walk Key:", -1, &rK5, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT rK6 = { 228, 470, 314, 494 };
        DrawTextA(hdc, "Walk Speed:", -1, &rK6, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Separador 4
        MoveToEx(hdc, 16, 500, NULL); LineTo(hdc, 438, 500);

        // Recuadro Recommended In-Game Config
        RECT boxRec = { 16, 542, 438, 610 };
        FillRect(hdc, &boxRec, g_brCtrlBg);
        HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, boxRec.left, boxRec.top, boxRec.right, boxRec.bottom);
        SelectObject(hdc, oldBr);

        SelectObject(hdc, g_fontBold);
        SetTextColor(hdc, COL_TEXT);
        RECT rRecT = { 26, 548, 430, 564 };
        DrawTextA(hdc, "Recommended In-Game Config:", -1, &rRecT, DT_LEFT | DT_SINGLELINE);

        SelectObject(hdc, g_fontSm);
        SetTextColor(hdc, COL_TEXT_DIM);
        RECT rRec1 = { 26, 565, 430, 580 };
        DrawTextA(hdc, "Fixed Camera: OFF    |    Invert X Axis: OFF", -1, &rRec1, DT_LEFT | DT_SINGLELINE);
        RECT rRec2 = { 26, 580, 430, 595 };
        DrawTextA(hdc, "Invert Y Axis: OFF    |    Aim Assist: OFF", -1, &rRec2, DT_LEFT | DT_SINGLELINE);
        RECT rRec3 = { 26, 595, 430, 608 };
        DrawTextA(hdc, "(Do not change in-game sensitivity settings)", -1, &rRec3, DT_LEFT | DT_SINGLELINE);

        SelectObject(hdc, oldPen);
        DeleteObject(penSep);
    }
    else
    {
        // Pestana About
        SelectObject(hdc, g_fontBig);
        SetTextColor(hdc, COL_TEXT);
        RECT rTitle = { 20, 140, 434, 176 };
        DrawTextA(hdc, "50 Cent Bulletproof Mouse Mod", -1, &rTitle, DT_CENTER | DT_SINGLELINE);

        SelectObject(hdc, g_fontNorm);
        SetTextColor(hdc, COL_TEXT_DIM);
        RECT rThanks = { 20, 195, 434, 220 };
        DrawTextA(hdc, "Thanks for downloading my mod", -1, &rThanks, DT_CENTER | DT_SINGLELINE);

        SelectObject(hdc, g_fontBold);
        SetTextColor(hdc, COL_TEXT);
        RECT rAuthor = { 20, 240, 434, 264 };
        DrawTextA(hdc, "Author: Epii.J", -1, &rAuthor, DT_CENTER | DT_SINGLELINE);

        SelectObject(hdc, g_fontNorm);
        SetTextColor(hdc, COL_TEXT_DIM);
        RECT rVer = { 20, 266, 434, 290 };
        DrawTextA(hdc, "Version: 1.0", -1, &rVer, DT_CENTER | DT_SINGLELINE);
    }

    EndPaint(hwnd, &ps);
}

static void ParseSensEdit(HWND hEdit, HWND hSlider, float *targetSens)
{
    if (g_updatingUi) return;

    char buf[32] = {0};
    GetWindowTextA(hEdit, buf, sizeof(buf));
    for (int i = 0; buf[i]; i++)
    {
        if (buf[i] == ',') buf[i] = '.';
    }

    char *endPtr = NULL;
    double val = strtod(buf, &endPtr);
    if (endPtr == buf) return;

    if (val < 0.001)  val = 0.001;
    if (val > 100.0)  val = 100.0;

    *targetSens = (float)val;
    g_updatingUi = true;
    SendMessageA(hSlider, TBM_SETPOS, TRUE, ClampSliderPos(*targetSens));
    g_updatingUi = false;
}

static void ShowGuiWindow()
{
    if (!g_guiHwnd) return;

    HWND fg = GetForegroundWindow();
    if (fg && fg != g_guiHwnd)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId())
        {
            g_prevGameHwnd = fg;
        }
    }

    RECT targetRc;
    if (g_prevGameHwnd && IsWindow(g_prevGameHwnd) && GetWindowRect(g_prevGameHwnd, &targetRc))
    {
        RECT winRc;
        GetWindowRect(g_guiHwnd, &winRc);
        int w = winRc.right - winRc.left;
        int h = winRc.bottom - winRc.top;
        int x = targetRc.left + ((targetRc.right - targetRc.left) - w) / 2;
        int y = targetRc.top + ((targetRc.bottom - targetRc.top) - h) / 2;
        SetWindowPos(g_guiHwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }

    CancelAllCaptures();
    SyncUiFromConfig();
    g_guiOpen = true;

    ShowWindow(g_guiHwnd, SW_SHOW);
    SetForegroundWindow(g_guiHwnd);
    BringWindowToTop(g_guiHwnd);
}

static void HideGuiWindow()
{
    if (!g_guiHwnd) return;
    CancelAllCaptures();
    Mod_SaveConfig();

    ShowWindow(g_guiHwnd, SW_HIDE);
    g_guiOpen = false;
    Mod_NotifyGuiClosed();

    if (g_prevGameHwnd && IsWindow(g_prevGameHwnd))
    {
        SetForegroundWindow(g_prevGameHwnd);
    }
}

static void ToggleGuiWindow()
{
    if (g_guiOpen) HideGuiWindow();
    else ShowGuiWindow();
}

static void CreateGuiControls(HWND hwnd)
{
    HINSTANCE hInst = GetModuleHandleA(NULL);

    // Pestanas superiores
    hTabConfig = CreateWindowA("BUTTON", "Config", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                               0, 0, 227, 36, hwnd, (HMENU)(INT_PTR)IDC_TAB_CONFIG, hInst, NULL);
    hTabAbout  = CreateWindowA("BUTTON", "About", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                               227, 0, 227, 36, hwnd, (HMENU)(INT_PTR)IDC_TAB_ABOUT, hInst, NULL);

    // Sliders y cajas de sensibilidad (rango slider 10..1000 = 0.10..10.00)
    hSliderX = CreateWindowA(TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                             122, 68, 240, 26, hwnd, (HMENU)(INT_PTR)IDC_SLIDER_SENS_X, hInst, NULL);
    SendMessageA(hSliderX, TBM_SETRANGE, TRUE, MAKELONG(10, 1000));

    hEditX = CreateWindowA("EDIT", "1.00", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_CENTER | ES_AUTOHSCROLL,
                           368, 70, 70, 22, hwnd, (HMENU)(INT_PTR)IDC_EDIT_SENS_X, hInst, NULL);

    hSliderY = CreateWindowA(TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                             122, 98, 240, 26, hwnd, (HMENU)(INT_PTR)IDC_SLIDER_SENS_Y, hInst, NULL);
    SendMessageA(hSliderY, TBM_SETRANGE, TRUE, MAKELONG(10, 1000));

    hEditY = CreateWindowA("EDIT", "1.00", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_CENTER | ES_AUTOHSCROLL,
                           368, 100, 70, 22, hwnd, (HMENU)(INT_PTR)IDC_EDIT_SENS_Y, hInst, NULL);

    // Checkboxes principales (2 filas x 2 columnas)
    hChkInvX    = CreateWindowA("BUTTON", "Invert X", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                16, 138, 195, 22, hwnd, (HMENU)(INT_PTR)IDC_CHK_INVERT_X, hInst, NULL);
    hChkInvY    = CreateWindowA("BUTTON", "Invert Y", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                225, 138, 210, 22, hwnd, (HMENU)(INT_PTR)IDC_CHK_INVERT_Y, hInst, NULL);
    hChkExec    = CreateWindowA("BUTTON", "Execution Camera Orbit", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                16, 164, 200, 22, hwnd, (HMENU)(INT_PTR)IDC_CHK_EXEC_ORBIT, hInst, NULL);
    hChkHostage = CreateWindowA("BUTTON", "Hostage Vertical Limit Bypass", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                225, 164, 218, 22, hwnd, (HMENU)(INT_PTR)IDC_CHK_HOSTAGE_LIM, hInst, NULL);

    // Mouse Binds
    hBtnAddBind = CreateWindowA("BUTTON", "+ Add Mouse Button", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                290, 202, 148, 24, hwnd, (HMENU)(INT_PTR)IDC_BTN_ADD_BIND, hInst, NULL);

    hCmbLeft  = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                              140, 230, 298, 240, hwnd, (HMENU)(INT_PTR)IDC_CMB_LEFT_CLICK, hInst, NULL);
    hCmbRight = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                              140, 258, 298, 240, hwnd, (HMENU)(INT_PTR)IDC_CMB_RIGHT_CLICK, hInst, NULL);

    PopulateActionCombo(hCmbLeft, g_cfg.leftClickAction);
    PopulateActionCombo(hCmbRight, g_cfg.rightClickAction);

    for (int i = 0; i < MAX_EXTRA_MOUSE_BINDS; i++)
    {
        int y = 286 + i * 28;
        hCmbExtra[i] = CreateWindowA("COMBOBOX", "", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
                                     140, y, 264, 240, hwnd, (HMENU)(INT_PTR)(IDC_CMB_EXTRA_BASE + i), hInst, NULL);
        PopulateActionCombo(hCmbExtra[i], XACT_NONE);

        hBtnDelExtra[i] = CreateWindowA("BUTTON", "X", WS_CHILD | BS_OWNERDRAW,
                                        410, y, 28, 23, hwnd, (HMENU)(INT_PTR)(IDC_BTN_DEL_BASE + i), hInst, NULL);
    }

    // Shortcuts
    hBtnKeyMenu   = CreateWindowA("BUTTON", "", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                  104, 410, 114, 24, hwnd, (HMENU)(INT_PTR)IDC_BTN_KEY_MENU, hInst, NULL);
    hBtnKeyToggle = CreateWindowA("BUTTON", "", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                  316, 410, 122, 24, hwnd, (HMENU)(INT_PTR)IDC_BTN_KEY_TOGGLE, hInst, NULL);
    hBtnKeyUp     = CreateWindowA("BUTTON", "", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                  104, 440, 114, 24, hwnd, (HMENU)(INT_PTR)IDC_BTN_KEY_SENS_UP, hInst, NULL);
    hBtnKeyDown   = CreateWindowA("BUTTON", "", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                  316, 440, 122, 24, hwnd, (HMENU)(INT_PTR)IDC_BTN_KEY_SENS_DN, hInst, NULL);

    // Fila Walk: Boton de tecla a la izquierda, Slider y Edit a la derecha
    hBtnKeyWalk   = CreateWindowA("BUTTON", "", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                  104, 470, 114, 24, hwnd, (HMENU)(INT_PTR)IDC_BTN_KEY_WALK, hInst, NULL);
    hSliderWalk   = CreateWindowA(TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                  306, 468, 80, 26, hwnd, (HMENU)(INT_PTR)IDC_SLIDER_WALK_SPD, hInst, NULL);
    SendMessageA(hSliderWalk, TBM_SETRANGE, TRUE, MAKELONG(34, 100));
    hEditWalk     = CreateWindowA("EDIT", "0.56", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_CENTER | ES_AUTOHSCROLL,
                                  392, 470, 46, 22, hwnd, (HMENU)(INT_PTR)IDC_EDIT_WALK_SPD, hInst, NULL);

    // Fila inferior: DebugLog a la izquierda, Reset to Default a la derecha
    hChkLog   = CreateWindowA("BUTTON", "Debug Log (mod_log.txt)", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                              16, 510, 210, 22, hwnd, (HMENU)(INT_PTR)IDC_CHK_DEBUG_LOG, hInst, NULL);
    hBtnReset = CreateWindowA("BUTTON", "Reset to Default", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                              306, 508, 132, 26, hwnd, (HMENU)(INT_PTR)IDC_BTN_RESET, hInst, NULL);

    // Boton GitHub en pestana About
    hBtnGitHub = CreateWindowA("BUTTON", "GitHub", WS_CHILD | BS_OWNERDRAW,
                               157, 320, 140, 36, hwnd, (HMENU)(INT_PTR)IDC_BTN_GITHUB, hInst, NULL);

    // Fuentes en controles nativos
    SendMessageA(hEditX,    WM_SETFONT, (WPARAM)g_fontNorm, TRUE);
    SendMessageA(hEditY,    WM_SETFONT, (WPARAM)g_fontNorm, TRUE);
    SendMessageA(hEditWalk, WM_SETFONT, (WPARAM)g_fontNorm, TRUE);
    SendMessageA(hCmbLeft,  WM_SETFONT, (WPARAM)g_fontNorm, TRUE);
    SendMessageA(hCmbRight, WM_SETFONT, (WPARAM)g_fontNorm, TRUE);
    for (int i = 0; i < MAX_EXTRA_MOUSE_BINDS; i++)
    {
        SendMessageA(hCmbExtra[i], WM_SETFONT, (WPARAM)g_fontNorm, TRUE);
    }
}

static LRESULT CALLBACK GuiWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CREATE:
            EnableDarkTitleBar(hwnd);
            CreateGuiControls(hwnd);
            SyncUiFromConfig();
            UpdateTabVisibility();
            SetTimer(hwnd, TIMER_POLL, 30, NULL);
            return 0;

        case WM_PAINT:
            DrawMainWindowPaint(hwnd);
            return 0;

        case WM_DRAWITEM:
            DrawOwnerButton((DRAWITEMSTRUCT *)lParam);
            return TRUE;

        case WM_CTLCOLORSTATIC:
        {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, COL_TEXT);
            SetBkColor(hdc, COL_BG);
            return (INT_PTR)g_brBg;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, COL_TEXT);
            SetBkColor(hdc, COL_CTRL_BG);
            return (INT_PTR)g_brCtrlBg;
        }

        case WM_HSCROLL:
        {
            HWND hCtrl = (HWND)lParam;
            if (!g_updatingUi && (hCtrl == hSliderX || hCtrl == hSliderY || hCtrl == hSliderWalk))
            {
                int pos = (int)SendMessageA(hCtrl, TBM_GETPOS, 0, 0);
                float val = (float)pos / 100.0f;
                char buf[32];

                g_updatingUi = true;
                if (hCtrl == hSliderX)
                {
                    FormatSensText(val, buf, sizeof(buf));
                    g_cfg.sensX = val;
                    SetWindowTextA(hEditX, buf);
                }
                else if (hCtrl == hSliderY)
                {
                    FormatSensText(val, buf, sizeof(buf));
                    g_cfg.sensY = val;
                    SetWindowTextA(hEditY, buf);
                }
                else if (hCtrl == hSliderWalk)
                {
                    g_cfg.walkSpeed = val;
                    snprintf(buf, sizeof(buf), "%.2f", val);
                    SetWindowTextA(hEditWalk, buf);
                }
                g_updatingUi = false;
            }
            return 0;
        }

        case WM_COMMAND:
        {
            int id   = LOWORD(wParam);
            int code = HIWORD(wParam);

            // Edicion de sensibilidad manual
            if (id == IDC_EDIT_SENS_X || id == IDC_EDIT_SENS_Y)
            {
                if (code == EN_CHANGE)
                {
                    if (id == IDC_EDIT_SENS_X) ParseSensEdit(hEditX, hSliderX, &g_cfg.sensX);
                    else                       ParseSensEdit(hEditY, hSliderY, &g_cfg.sensY);
                }
                else if (code == EN_KILLFOCUS)
                {
                    g_updatingUi = true;
                    char buf[32];
                    if (id == IDC_EDIT_SENS_X)
                    {
                        FormatSensText(g_cfg.sensX, buf, sizeof(buf));
                        SetWindowTextA(hEditX, buf);
                    }
                    else
                    {
                        FormatSensText(g_cfg.sensY, buf, sizeof(buf));
                        SetWindowTextA(hEditY, buf);
                    }
                    g_updatingUi = false;
                }
                return 0;
            }

            // Edicion manual de Walk Speed
            if (id == IDC_EDIT_WALK_SPD)
            {
                if (code == EN_CHANGE && !g_updatingUi)
                {
                    char buf[32] = {0};
                    GetWindowTextA(hEditWalk, buf, sizeof(buf));
                    for (int i = 0; buf[i]; i++) if (buf[i] == ',') buf[i] = '.';
                    char *endPtr = NULL;
                    double val = strtod(buf, &endPtr);
                     if (endPtr != buf)
                    {
                        if (val < 0.34) val = 0.34;
                        if (val > 1.0)  val = 1.0;
                        g_cfg.walkSpeed = (float)val;
                        g_updatingUi = true;
                        SendMessageA(hSliderWalk, TBM_SETPOS, TRUE, (int)(g_cfg.walkSpeed * 100.0f + 0.5f));
                        g_updatingUi = false;
                    }
                }
                else if (code == EN_KILLFOCUS)
                {
                    g_updatingUi = true;
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%.2f", g_cfg.walkSpeed);
                    SetWindowTextA(hEditWalk, buf);
                    g_updatingUi = false;
                }
                return 0;
            }

            // Combos de binds
            if (code == CBN_SELCHANGE)
            {
                int sel = (int)SendMessageA((HWND)lParam, CB_GETCURSEL, 0, 0);
                if (sel < 0 || sel >= XACT_COUNT) sel = XACT_NONE;

                if (id == IDC_CMB_LEFT_CLICK)
                {
                    g_cfg.leftClickAction = sel;
                    Mod_SaveConfig();
                }
                else if (id == IDC_CMB_RIGHT_CLICK)
                {
                    g_cfg.rightClickAction = sel;
                    Mod_SaveConfig();
                }
                else if (id >= IDC_CMB_EXTRA_BASE && id < IDC_CMB_EXTRA_BASE + MAX_EXTRA_MOUSE_BINDS)
                {
                    int idx = id - IDC_CMB_EXTRA_BASE;
                    if (idx < g_cfg.extraBindCount)
                    {
                        g_cfg.extraBinds[idx].action = sel;
                        Mod_SaveConfig();
                    }
                }
                return 0;
            }

            if (code != BN_CLICKED) return 0;

            switch (id)
            {
                case IDC_TAB_CONFIG:
                    CancelAllCaptures();
                    g_activeTab = 0;
                    UpdateTabVisibility();
                    break;

                case IDC_TAB_ABOUT:
                    CancelAllCaptures();
                    g_activeTab = 1;
                    UpdateTabVisibility();
                    break;

                case IDC_CHK_INVERT_X:
                    g_cfg.invertX = !g_cfg.invertX;
                    Mod_SaveConfig();
                    InvalidateRect(hChkInvX, NULL, FALSE);
                    break;

                case IDC_CHK_INVERT_Y:
                    g_cfg.invertY = !g_cfg.invertY;
                    Mod_SaveConfig();
                    InvalidateRect(hChkInvY, NULL, FALSE);
                    break;

                case IDC_CHK_EXEC_ORBIT:
                    g_cfg.execCamOrbit = !g_cfg.execCamOrbit;
                    Mod_SaveConfig();
                    InvalidateRect(hChkExec, NULL, FALSE);
                    break;

                case IDC_CHK_HOSTAGE_LIM:
                    g_cfg.removeHostageLimit = !g_cfg.removeHostageLimit;
                    Mod_SaveConfig();
                    InvalidateRect(hChkHostage, NULL, FALSE);
                    break;

                case IDC_CHK_DEBUG_LOG:
                    g_cfg.debugLog = !g_cfg.debugLog;
                    Mod_SaveConfig();
                    InvalidateRect(hChkLog, NULL, FALSE);
                    break;

                case IDC_BTN_ADD_BIND:
                    if (g_capturingMouseBtn)
                    {
                        CancelAllCaptures();
                    }
                    else if (g_cfg.extraBindCount < MAX_EXTRA_MOUSE_BINDS)
                    {
                        CancelAllCaptures();
                        g_capturingMouseBtn = true;
                        RefreshExtraBindControls();
                        InvalidateRect(hBtnAddBind, NULL, FALSE);
                    }
                    break;

                case IDC_BTN_KEY_MENU:
                    CancelAllCaptures();
                    g_captureShortcutIdx = 0;
                    RefreshShortcutButtons();
                    InvalidateRect(hBtnKeyMenu, NULL, FALSE);
                    break;

                case IDC_BTN_KEY_TOGGLE:
                    CancelAllCaptures();
                    g_captureShortcutIdx = 1;
                    RefreshShortcutButtons();
                    InvalidateRect(hBtnKeyToggle, NULL, FALSE);
                    break;

                case IDC_BTN_KEY_SENS_UP:
                    CancelAllCaptures();
                    g_captureShortcutIdx = 2;
                    RefreshShortcutButtons();
                    InvalidateRect(hBtnKeyUp, NULL, FALSE);
                    break;

                case IDC_BTN_KEY_SENS_DN:
                    CancelAllCaptures();
                    g_captureShortcutIdx = 3;
                    RefreshShortcutButtons();
                    InvalidateRect(hBtnKeyDown, NULL, FALSE);
                    break;

                case IDC_BTN_KEY_WALK:
                    CancelAllCaptures();
                    g_captureShortcutIdx = 4;
                    RefreshShortcutButtons();
                    InvalidateRect(hBtnKeyWalk, NULL, FALSE);
                    break;

                case IDC_BTN_RESET:
                {
                    CancelAllCaptures();
                    int res = MessageBoxA(hwnd,
                        "Reset all settings, mouse binds and shortcuts to default?",
                        "50 Cent Bulletproof Mouse Mod",
                        MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);
                    if (res == IDYES)
                    {
                        Mod_ResetDefaults();
                        Mod_SaveConfig();
                        SyncUiFromConfig();
                    }
                    break;
                }

                case IDC_BTN_GITHUB:
                    ShellExecuteA(NULL, "open", URL_GITHUB, NULL, NULL, SW_SHOWNORMAL);
                    break;

                default:
                    if (id >= IDC_BTN_DEL_BASE && id < IDC_BTN_DEL_BASE + MAX_EXTRA_MOUSE_BINDS)
                    {
                        CancelAllCaptures();
                        RemoveExtraMouseButton(id - IDC_BTN_DEL_BASE);
                    }
                    break;
            }
            return 0;
        }

        case WM_TIMER:
        {
            if (wParam != TIMER_POLL) break;

            if (g_guiOpen && g_capturingMouseBtn)
            {
                if (GetAsyncKeyState(VK_MBUTTON) & 0x8000)
                {
                    AddExtraMouseButton(VK_MBUTTON);
                }
                else if (GetAsyncKeyState(VK_XBUTTON1) & 0x8000)
                {
                    AddExtraMouseButton(VK_XBUTTON1);
                }
                else if (GetAsyncKeyState(VK_XBUTTON2) & 0x8000)
                {
                    AddExtraMouseButton(VK_XBUTTON2);
                }
                return 0;
            }

            static ULONGLONG lastMenuToggleTick = 0;
            ULONGLONG now = GetTickCount64();

            if (g_captureShortcutIdx == -1 && !g_capturingMouseBtn && (now - lastMenuToggleTick >= 300))
            {
                HWND fg = GetForegroundWindow();
                DWORD pid = 0;
                if (fg) GetWindowThreadProcessId(fg, &pid);

                HWND focusCtrl = g_guiOpen ? GetFocus() : NULL;
                bool editingText = (focusCtrl == hEditX || focusCtrl == hEditY || focusCtrl == hEditWalk);

                if (pid == GetCurrentProcessId() && !editingText && IsShortcutHeldNow(g_cfg.keyOpenMenu))
                {
                    lastMenuToggleTick = now;
                    ToggleGuiWindow();
                }
            }
            return 0;
        }

        case WM_CLOSE:
            HideGuiWindow();
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, TIMER_POLL);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static bool IsModifierKey(int vk)
{
    return (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
            vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
            vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU);
}

static void CommitCapturedShortcut(int vk)
{
    ShortcutDef sc;
    sc.vk    = vk;
    sc.shift = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
    sc.ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    sc.alt   = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;

    switch (g_captureShortcutIdx)
    {
        case 0: g_cfg.keyOpenMenu  = sc; break;
        case 1: g_cfg.keyToggleMod = sc; break;
        case 2: g_cfg.keySensUp    = sc; break;
        case 3: g_cfg.keySensDown  = sc; break;
        case 4: g_cfg.keyWalk      = sc; break;
    }

    g_captureShortcutIdx = -1;
    RefreshShortcutButtons();
    Mod_SaveConfig();
    InvalidateRect(g_guiHwnd, NULL, FALSE);
}

static DWORD WINAPI GuiThreadProc(LPVOID lpParam)
{
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC  = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    g_brBg     = CreateSolidBrush(COL_BG);
    g_brCtrlBg = CreateSolidBrush(COL_CTRL_BG);
    g_fontNorm = CreateFontA(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g_fontBold = CreateFontA(15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g_fontBig  = CreateFontA(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g_fontSm   = CreateFontA(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = GuiWndProc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = g_brBg;
    wc.lpszClassName = "BulletproofMouseModGuiClass";
    RegisterClassA(&wc);

    DWORD style   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    DWORD exStyle = WS_EX_TOPMOST;
    RECT rc = { 0, 0, 454, 624 };
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);

    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int posX = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int posY = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

    g_guiHwnd = CreateWindowExA(
        exStyle,
        wc.lpszClassName,
        "50 Cent Bulletproof Mouse Mod",
        style,
        posX, posY, winW, winH,
        NULL, NULL, wc.hInstance, NULL);

    if (!g_guiHwnd) return 0;

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0)
    {
        if (g_guiOpen && g_captureShortcutIdx >= 0 &&
            (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN))
        {
            int vk = (int)msg.wParam;
            if (vk == VK_ESCAPE)
            {
                CancelAllCaptures();
            }
            else if (!IsModifierKey(vk))
            {
                CommitCapturedShortcut(vk);
            }
            continue;
        }

        if (g_guiOpen && g_capturingMouseBtn)
        {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
            {
                CancelAllCaptures();
                continue;
            }
            if (msg.message == WM_MBUTTONDOWN)
            {
                AddExtraMouseButton(VK_MBUTTON);
                continue;
            }
            if (msg.message == WM_XBUTTONDOWN)
            {
                WORD xb = HIWORD(msg.wParam);
                AddExtraMouseButton((xb == 1) ? VK_XBUTTON1 : VK_XBUTTON2);
                continue;
            }
        }

        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return 0;
}

void Gui_StartThread(void)
{
    CreateThread(NULL, 0, GuiThreadProc, NULL, 0, NULL);
}
