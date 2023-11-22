#include <windows.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <imm.h>
#include <assert.h>
#include "immdev.h"
#include "resource.h"

#define STANDALONE

static HINSTANCE ghImm32Inst;

static UINT guScanCode[256]; /* Mapping: virtual key --> scan code */
static POINT gptRaiseEdge; /* Border + Edge metrics */
static BOOL g_bWantSoftKBDMetrics = TRUE;

#ifndef min
    #define min(x1, x2) (((x1) < (x2)) ? (x1) : (x2))
#endif

#ifdef STANDALONE
    #define ImmCreateSoftKeyboard C1_CreateSoftKeyboard
    #define ImmShowSoftKeyboard C1_ShowSoftKeyboard
    #define ImmDestroySoftKeyboard C1_DestroySoftKeyboard
#endif

// Win: ImmPtInRect
static inline BOOL
Imm32PtInRect(
    _In_ const POINT *ppt,
    _In_ LONG x,
    _In_ LONG y,
    _In_ LONG cx,
    _In_ LONG cy)
{
    return (x <= ppt->x) && (ppt->x < x + cx) && (y <= ppt->y) && (ppt->y < y + cy);
}

static inline INT
Imm32Clamp(
    _In_ INT x,
    _In_ INT xMin,
    _In_ INT xMax)
{
    if (x < xMin)
        return xMin;
    if (x > xMax)
        return xMax;
    return x;
}

// Win: GetAllMonitorSize
static VOID
Imm32GetAllMonitorSize(
    _Out_ LPRECT prcWork)
{
    if (GetSystemMetrics(SM_CMONITORS) == 1)
    {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, prcWork, 0);
        return;
    }

    prcWork->left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    prcWork->top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    prcWork->right  = prcWork->left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    prcWork->bottom = prcWork->top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

// Win: GetNearestMonitorSize
static BOOL
Imm32GetNearestWorkArea(
    _In_opt_ HWND hwnd,
    _Out_ LPRECT prcWork)
{
    HMONITOR hMonitor;
    MONITORINFO mi;

    if (GetSystemMetrics(SM_CMONITORS) == 1)
    {
        Imm32GetAllMonitorSize(prcWork);
        return TRUE;
    }

    hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hMonitor)
        return FALSE;

    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMonitor, &mi);
    *prcWork = mi.rcWork;
    return TRUE;
}

/*****************************************************************************
 * IME Software Keyboard Type T1
 */

#define T1_CLASSNAMEW L"SoftKBDClsT1"

typedef struct T1WINDOW
{
    char unused;
} T1WINDOW, *PT1WINDOW;

// Win: SKWndProcT1
LRESULT CALLBACK
T1_WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_CREATE:
            return -1;
    }
    return 0;
}

/*****************************************************************************
 * IME Software Keyboard Type C1
 */

#define C1_CLASSNAMEW L"SoftKBDClsC1"

#define C1K_MAX 56

#undef DEFINE_C1K
#define DEFINE_C1K(internal_code, virtual_key_code, internal_code_name, virtual_key_name, is_special) \
    internal_code_name = internal_code,

/* Define internal codes (C1K_...) */
typedef enum C1KEY
{
#include "c1keys.h"
} C1KEY;

#undef DEFINE_C1K
#define DEFINE_C1K(internal_code, virtual_key_code, internal_code_name, virtual_key_name, is_special) \
    virtual_key_code,

/* Mapping: C1K --> Virtual Key */
const BYTE gC1K2VK[C1K_MAX] =
{
#include "c1keys.h"
};

#define FLAG_SHIFT_PRESSED 1
#define FLAG_DRAGGING 2
#define FLAG_PRESSED 4

typedef struct C1WINDOW
{
    WCHAR Data[2][47];
    DWORD dwFlags;
    HBITMAP hbmKeyboard;
    LPARAM SubType;
    INT iPressedKey;
    POINT pt1, pt2;
    DWORD CharSet;
} C1WINDOW, *PC1WINDOW;

static POINT gptButtonPos[C1K_MAX];
static BOOL gfSoftKbdC1Init = FALSE;

// Win: InitSKC1ButtonPos
void C1_InitButtonPos(void)
{
    LONG x = 0, y = 0;
    INT iKey;

    // 1st row
    for (iKey = C1K_OEM_3; iKey < C1K_Q; ++iKey)
    {
        gptButtonPos[iKey].x = x;
        gptButtonPos[iKey].y = y;
        x += 24;
    }
    gptButtonPos[C1K_BACKSPACE].x = x;
    gptButtonPos[C1K_BACKSPACE].y = y;

    // 2nd row
    y = 28;
    gptButtonPos[C1K_TAB].x = 0;
    gptButtonPos[C1K_TAB].y = y;
    x = 36;
    for (; iKey < C1K_A; ++iKey)
    {
        gptButtonPos[iKey].x = x;
        gptButtonPos[iKey].y = y;
        x += 24;
    }

    // 3rd row
    y = 56;
    gptButtonPos[C1K_CAPS].x = 0;
    gptButtonPos[C1K_CAPS].y = y;
    x = 42;
    for (; iKey < C1K_Z; ++iKey)
    {
        gptButtonPos[iKey].x = x;
        gptButtonPos[iKey].y = y;
        x += 24;
    }
    gptButtonPos[C1K_ENTER].x = x;
    gptButtonPos[C1K_ENTER].y = y;

    // 4th row
    y = 84;
    gptButtonPos[C1K_SHIFT].x = 0;
    gptButtonPos[C1K_SHIFT].y = y;
    x = 60;
    for (; iKey < C1K_BACKSPACE; ++iKey)
    {
        gptButtonPos[iKey].x = x;
        gptButtonPos[iKey].y = y;
        x += 24;
    }

    // 5th row
    y = 112;
    gptButtonPos[C1K_INSERT].x = 0;
    gptButtonPos[C1K_INSERT].y = y;
    gptButtonPos[C1K_DELETE].x = 58;
    gptButtonPos[C1K_DELETE].y = y;
    gptButtonPos[C1K_SPACE].x = 96;
    gptButtonPos[C1K_SPACE].y = y;
    gptButtonPos[C1K_ESCAPE].x = 310;
    gptButtonPos[C1K_ESCAPE].y = y;
}

// Win: SKC1DrawConvexRect
void C1_DrawConvexRect(HDC hDC, INT x, INT y, INT width, INT height)
{
    HGDIOBJ hLtGrayBrush = GetStockObject(LTGRAY_BRUSH);
    HGDIOBJ hBlackPen = GetStockObject(BLACK_PEN);
    HGDIOBJ hWhiteBrush = GetStockObject(WHITE_BRUSH);
    HGDIOBJ hGrayBrush = GetStockObject(GRAY_BRUSH);
    INT y2 = y + height - 1;

    /* Draw face */
    SelectObject(hDC, hLtGrayBrush);
    SelectObject(hDC, hBlackPen);
    Rectangle(hDC, x, y, x + width, y + height);

    /* Draw light edge */
    SelectObject(hDC, hWhiteBrush);
    PatBlt(hDC, x, y2, 2, 1 - height, PATCOPY);
    PatBlt(hDC, x, y, width - 1, 2, PATCOPY);

    /* Draw dark edge */
    SelectObject(hDC, hGrayBrush);
    PatBlt(hDC, x + 1, y2, width - 2, -1, PATCOPY);
    PatBlt(hDC, x + width - 1, y2, -1, 2 - height, PATCOPY);
}

// Win: SKC1InvertButton
void C1_InvertButton(HDC hDC, INT iKey)
{
    INT width = 24, height = 28;

    if (iKey < 0)
        return;

    switch (iKey)
    {
        case C1K_BACKSPACE: case C1K_TAB:
            width = 36;
            break;
        case C1K_CAPS: case C1K_ENTER:
            width = 42;
            break;
        case C1K_SHIFT:
            width = 60;
            break;
        case C1K_INSERT: case C1K_DELETE: case C1K_ESCAPE:
            width = 38;
            height = 24;
            break;
        case C1K_SPACE:
            width = 172;
            height = 24;
            break;
        default:
            break;
    }

    BitBlt(hDC, gptButtonPos[iKey].x, gptButtonPos[iKey].y, width, height,
           hDC, gptButtonPos[iKey].x, gptButtonPos[iKey].y, DSTINVERT);
}

// Win: SKC1DrawBitmap
void C1_DrawBitmap(HDC hdc, INT x, INT y, INT width, INT height, INT nBitmapID)
{
    HBITMAP hBitmap = LoadBitmapW(ghImm32Inst, MAKEINTRESOURCEW(nBitmapID));
    HDC hMemDC = CreateCompatibleDC(hdc);
    HGDIOBJ hbmOld = SelectObject(hMemDC, hBitmap);
    BitBlt(hdc, x, y, width, height, hMemDC, 0, 0, SRCCOPY);
    DeleteObject(SelectObject(hMemDC, hbmOld));
    DeleteDC(hMemDC);
}

// Win: SKC1DrawLabel
void C1_DrawLabel(HDC hDC, INT nBitmapID)
{
    HBITMAP hBitmap;
    HGDIOBJ hbmOld;
    HDC hMemDC;
    INT iKey;

    hBitmap = LoadBitmapW(ghImm32Inst, MAKEINTRESOURCEW(nBitmapID));
    hMemDC = CreateCompatibleDC(hDC);
    hbmOld = SelectObject(hMemDC, hBitmap);
    for (iKey = C1K_OEM_3; iKey < C1K_BACKSPACE; ++iKey)
    {
        BitBlt(hDC, gptButtonPos[iKey].x + 2, gptButtonPos[iKey].y + 2, 8, 8,
               hMemDC, iKey * 8, 0, SRCCOPY);
    }
    DeleteObject(SelectObject(hMemDC, hbmOld));
    DeleteDC(hMemDC);
}

// Win: InitSKC1Bitmap
void C1_InitBitmap(HDC hDC, INT x, INT y, INT width, INT height)
{
    HGDIOBJ hLtGrayBrush = GetStockObject(LTGRAY_BRUSH);
    HGDIOBJ hNullPen = GetStockObject(NULL_PEN);
    INT iKey;

    /* Draw keyboard frame */
    SelectObject(hDC, hLtGrayBrush);
    SelectObject(hDC, hNullPen);
    Rectangle(hDC, x, y, width + 1, height + 1);

    for (iKey = C1K_OEM_3; iKey < C1K_BACKSPACE; ++iKey)
    {
        C1_DrawConvexRect(hDC, gptButtonPos[iKey].x, gptButtonPos[iKey].y, 24, 28);
    }

    C1_DrawLabel(hDC, IDB_C1_CHARS);

    C1_DrawConvexRect(hDC, gptButtonPos[C1K_BACKSPACE].x, gptButtonPos[C1K_BACKSPACE].y, 36, 28);
    C1_DrawBitmap(hDC, gptButtonPos[C1K_BACKSPACE].x + 2, gptButtonPos[C1K_BACKSPACE].y + 2, 32, 24, IDB_C1_BACKSPACE);

    C1_DrawConvexRect(hDC, gptButtonPos[C1K_TAB].x, gptButtonPos[C1K_TAB].y, 36, 28);
    C1_DrawBitmap(hDC, gptButtonPos[C1K_TAB].x + 2, gptButtonPos[C1K_TAB].y + 2, 32, 24, IDB_C1_TAB);

    C1_DrawConvexRect(hDC, gptButtonPos[C1K_CAPS].x, gptButtonPos[C1K_CAPS].y, 42, 28);
    C1_DrawBitmap(hDC, gptButtonPos[C1K_CAPS].x + 2, gptButtonPos[C1K_CAPS].y + 2, 38, 24, IDB_C1_CAPS);

    C1_DrawConvexRect(hDC, gptButtonPos[C1K_ENTER].x, gptButtonPos[C1K_ENTER].y, 42, 28);
    C1_DrawBitmap(hDC, gptButtonPos[C1K_ENTER].x + 2, gptButtonPos[C1K_ENTER].y + 2, 38, 24, IDB_C1_ENTER);

    C1_DrawConvexRect(hDC, gptButtonPos[C1K_SHIFT].x, gptButtonPos[C1K_SHIFT].y, 60, 28);
    C1_DrawBitmap(hDC, gptButtonPos[C1K_SHIFT].x + 2, gptButtonPos[C1K_SHIFT].y + 2, 56, 24, IDB_C1_SHIFT);

    C1_DrawConvexRect(hDC, gptButtonPos[C1K_INSERT].x, gptButtonPos[C1K_INSERT].y, 38, 24);
    C1_DrawBitmap(hDC, gptButtonPos[C1K_INSERT].x + 2, gptButtonPos[C1K_INSERT].y + 2, 34, 20, IDB_C1_INS);

    C1_DrawConvexRect(hDC, gptButtonPos[C1K_DELETE].x, gptButtonPos[C1K_DELETE].y, 38, 24);
    C1_DrawBitmap(hDC, gptButtonPos[C1K_DELETE].x + 2, gptButtonPos[C1K_DELETE].y + 2, 34, 20, IDB_C1_DEL);

    C1_DrawConvexRect(hDC, gptButtonPos[C1K_SPACE].x, gptButtonPos[C1K_SPACE].y, 172, 24);

    C1_DrawConvexRect(hDC, gptButtonPos[C1K_ESCAPE].x, gptButtonPos[C1K_ESCAPE].y , 38, 24);
    C1_DrawBitmap(hDC, gptButtonPos[C1K_ESCAPE].x + 2, gptButtonPos[C1K_ESCAPE].y + 2, 34, 20, IDB_C1_ESCAPE);
}

// Win: CreateC1Window
INT C1_OnCreate(HWND hWnd)
{
    HGLOBAL hGlobal;
    PC1WINDOW pC1;
    HDC hDC, hMemDC;
    RECT rc;
    HGDIOBJ hbmOld;
    HBITMAP hbmKeyboard;

    hGlobal = GlobalAlloc(GHND, sizeof(C1WINDOW));
    if (!hGlobal)
        return -1;

    pC1 = (PC1WINDOW)GlobalLock(hGlobal);
    if (!pC1)
    {
        GlobalFree(hGlobal);
        return -1;
    }
    SetWindowLongPtrW(hWnd, 0, (LONG_PTR)hGlobal);

    if (!gfSoftKbdC1Init)
    {
        C1_InitButtonPos();
        gfSoftKbdC1Init = TRUE;
    }

    pC1->iPressedKey = -1;
    pC1->CharSet = GB2312_CHARSET;

    GetClientRect(hWnd, &rc);

    hDC = GetDC(hWnd);
    hMemDC = CreateCompatibleDC(hDC);
    hbmKeyboard = CreateCompatibleBitmap(hDC, rc.right - rc.left, rc.bottom - rc.top);
    ReleaseDC(hWnd, hDC);

    hbmOld = SelectObject(hMemDC, hbmKeyboard);
    C1_InitBitmap(hMemDC, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hMemDC, hbmOld);
    pC1->hbmKeyboard = hbmKeyboard;
    DeleteDC(hMemDC);

    GlobalUnlock(hGlobal);
    return 0;
}

// Win: ShowSKC1Window
void C1_OnDraw(HDC hDC, HWND hWnd)
{
    HGLOBAL hGlobal;
    PC1WINDOW pC1;
    HDC hMemDC;
    RECT rc;
    HGDIOBJ hbmOld;

    hGlobal = (HGLOBAL)GetWindowLongPtrW(hWnd, 0);
    pC1 = (PC1WINDOW)GlobalLock(hGlobal);
    if (!hGlobal || !pC1)
        return;

    GetClientRect(hWnd, &rc);

    hMemDC = CreateCompatibleDC(hDC);
    hbmOld = SelectObject(hMemDC, pC1->hbmKeyboard);
    BitBlt(hDC, 0, 0, rc.right - rc.left, rc.bottom - rc.top, hMemDC, 0, 0, SRCCOPY);
    SelectObject(hMemDC, hbmOld);
    DeleteDC(hMemDC);

    GlobalUnlock(hGlobal);
}

// Win: UpdateSKC1Window
BOOL C1_SetData(HWND hWnd, SOFTKBDDATA *pData)
{
    HGLOBAL hGlobal;
    PC1WINDOW pC1;
    HDC hDC, hMemDC;
    INT iKey;
    BOOL bDisabled;
    HBITMAP hbmKeyboard;
    HGDIOBJ hbmOld, hFontOld;
    HFONT hFont;
    RECT rc;
    LOGFONTW lf;

    if (pData->uCount != 2)
        return 0;

    hGlobal = (HGLOBAL)GetWindowLongPtrW(hWnd, 0);
    pC1 = (PC1WINDOW)GlobalLock(hGlobal);
    if (!hGlobal || !pC1)
        return FALSE;

    hDC = GetDC(hWnd);
    hMemDC = CreateCompatibleDC(hDC);

    hbmKeyboard = pC1->hbmKeyboard;
    hbmOld = SelectObject(hMemDC, hbmKeyboard);

    GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(LOGFONTW), &lf);
    lf.lfHeight = -12;
    if (pC1->CharSet != DEFAULT_CHARSET)
        lf.lfCharSet = (BYTE)pC1->CharSet;

    hFont = CreateFontIndirectW(&lf);
    hFontOld = SelectObject(hMemDC, hFont);
    for (iKey = C1K_OEM_3; iKey < C1K_BACKSPACE; ++iKey)
    {
        pC1->Data[1][iKey] = pData->wCode[0][(BYTE)gC1K2VK[iKey]];
        pC1->Data[0][iKey] = pData->wCode[1][(BYTE)gC1K2VK[iKey]];
    }
    SetBkColor(hMemDC, RGB(191, 191, 191));

    for (iKey = C1K_OEM_3; iKey < C1K_BACKSPACE; ++iKey)
    {
        rc.left = gptButtonPos[iKey].x + 10;
        rc.right = rc.left + 12;
        rc.top = gptButtonPos[iKey].y + 2;
        rc.bottom = gptButtonPos[iKey].y + 14;
        bDisabled = pC1->Data[0][iKey] == 0;
        DrawTextExW(hMemDC, &pC1->Data[0][iKey], !bDisabled, &rc, DT_CENTER, NULL);
        rc.left = gptButtonPos[iKey].x + 1 + 1;
        rc.right = gptButtonPos[iKey].x + 1 + 13;
        rc.top = gptButtonPos[iKey].y + 14;
        rc.bottom = rc.top + 12;
        bDisabled = pC1->Data[1][iKey] == 0;
        DrawTextExW(hMemDC, &pC1->Data[1][iKey], !bDisabled, &rc, DT_CENTER, NULL);
    }

    if (pC1->dwFlags & FLAG_SHIFT_PRESSED)
        C1_InvertButton(hMemDC, C1K_SHIFT);

    pC1->dwFlags = 0;

    SelectObject(hMemDC, hbmOld);
    DeleteObject(SelectObject(hMemDC, hFontOld));

    DeleteDC(hMemDC);
    ReleaseDC(hWnd, hDC);

    GlobalUnlock(hGlobal);
    return TRUE;
}

// Win: SKC1DrawDragBorder
void C1_DrawDragBorder(HWND hWnd, LPPOINT ppt1, LPPOINT ppt2)
{
    HGDIOBJ hGrayBrush = GetStockObject(GRAY_BRUSH);
    INT x, y;
    RECT rc, rcWork;
    INT cxBorder = GetSystemMetrics(SM_CXBORDER), cyBorder = GetSystemMetrics(SM_CYBORDER);
    HDC hDisplayDC;

    Imm32GetAllMonitorSize(&rcWork);
    hDisplayDC = CreateDCW(L"DISPLAY", NULL, NULL, NULL);

    SelectObject(hDisplayDC, hGrayBrush);
    x = ppt1->x - ppt2->x;
    y = ppt1->y - ppt2->y;
    if (x < rcWork.left)
        x = rcWork.left;
    if (y < rcWork.top)
        y = rcWork.top;

    GetWindowRect(hWnd, &rc);

    if (rc.right - rc.left + x > rcWork.right)
        x = rc.left + rcWork.right - rc.right;
    if (y + rc.bottom - rc.top > rcWork.bottom)
        y = rc.top + rcWork.bottom - rc.bottom;

    ppt2->x = ppt1->x - x;
    ppt2->y = ppt1->y - y;

    PatBlt(hDisplayDC, x, y, rc.right - rc.left - cxBorder, cyBorder, PATINVERT);
    PatBlt(hDisplayDC, x, y + cyBorder, cxBorder, rc.bottom - rc.top - cyBorder, PATINVERT);
    PatBlt(hDisplayDC, x + cxBorder, y + rc.bottom - rc.top, rc.right - rc.left - cxBorder, -cyBorder, PATINVERT);
    PatBlt(hDisplayDC, x + rc.right - rc.left, y, -cxBorder, rc.bottom - rc.top - cyBorder, PATINVERT);

    DeleteDC(hDisplayDC);
}

// Win: SKC1MousePosition
INT C1_HitTest(const POINT *ppt)
{
    INT iKey;

    for (iKey = C1K_OEM_3; iKey < C1K_BACKSPACE; ++iKey)
    {
        if (Imm32PtInRect(ppt, gptButtonPos[iKey].x, gptButtonPos[iKey].y, 24, 28))
            return iKey;
    }

    if (Imm32PtInRect(ppt, gptButtonPos[C1K_BACKSPACE].x, gptButtonPos[C1K_BACKSPACE].y, 36, 28))
        return C1K_BACKSPACE;
    if (Imm32PtInRect(ppt, gptButtonPos[C1K_TAB].x, gptButtonPos[C1K_TAB].y, 36, 28))
        return C1K_TAB;
    if (Imm32PtInRect(ppt, gptButtonPos[C1K_CAPS].x, gptButtonPos[C1K_CAPS].y, 42, 28))
        return C1K_CAPS;
    if (Imm32PtInRect(ppt, gptButtonPos[C1K_ENTER].x, gptButtonPos[C1K_ENTER].y, 42, 28))
        return C1K_ENTER;
    if (Imm32PtInRect(ppt, gptButtonPos[C1K_SHIFT].x, gptButtonPos[C1K_SHIFT].y, 60, 28))
        return C1K_SHIFT;
    if (Imm32PtInRect(ppt, gptButtonPos[C1K_ESCAPE].x, gptButtonPos[C1K_ESCAPE].y, 38, 24))
        return C1K_ESCAPE;
    if (Imm32PtInRect(ppt, gptButtonPos[C1K_SPACE].x, gptButtonPos[C1K_SPACE].y, 172, 24))
        return C1K_SPACE;
    if (Imm32PtInRect(ppt, gptButtonPos[C1K_INSERT].x, gptButtonPos[C1K_INSERT].y, 38, 24))
        return C1K_INSERT;
    if (Imm32PtInRect(ppt, gptButtonPos[C1K_DELETE].x, gptButtonPos[C1K_DELETE].y, 38, 24))
        return C1K_DELETE;

    return -1;
}

// Win: SKC1ButtonDown
void C1_OnButtonDown(HWND hWnd, PC1WINDOW pC1)
{
    INT iPressedKey;
    HDC hMemDC;
    WCHAR wch = 0xFF;
    HGDIOBJ hbmOld;
    HDC hDC;

    SetCapture(hWnd);

    iPressedKey = pC1->iPressedKey;
    if (iPressedKey == -1)
    {
        pC1->dwFlags |= FLAG_DRAGGING;
        C1_DrawDragBorder(hWnd, &pC1->pt1, &pC1->pt2);
        return;
    }

    if (iPressedKey < C1K_BACKSPACE)
    {
        wch = pC1->Data[!(pC1->dwFlags & 1)][iPressedKey];
        if (!wch)
        {
            MessageBeep(0xFFFFFFFF);
            pC1->iPressedKey = -1;
            return;
        }
    }

    if ((iPressedKey != C1K_SHIFT) || !(pC1->dwFlags & FLAG_SHIFT_PRESSED))
    {
        hDC = GetDC(hWnd);
        hMemDC = CreateCompatibleDC(hDC);
        hbmOld = SelectObject(hMemDC, pC1->hbmKeyboard);
        C1_InvertButton(hDC, pC1->iPressedKey);
        C1_InvertButton(hMemDC, pC1->iPressedKey);
        SelectObject(hMemDC, hbmOld);
        DeleteDC(hMemDC);
        ReleaseDC(hWnd, hDC);
    }

    pC1->dwFlags |= FLAG_PRESSED;
}

// Win: SKC1SetCursor
BOOL C1_OnSetCursor(HWND hWnd, LPARAM lParam)
{
    HGLOBAL hGlobal;
    PC1WINDOW pC1;
    HCURSOR hCursor;
    INT iKey;
    POINT pt1, pt2;

    hGlobal = (HGLOBAL)GetWindowLongW(hWnd, 0);
    pC1 = (PC1WINDOW)GlobalLock(hGlobal);
    if (!hGlobal || !pC1)
        return FALSE;

    if (pC1->dwFlags & FLAG_DRAGGING)
    {
        hCursor = LoadCursorW(0, (LPCWSTR)IDC_SIZEALL);
        SetCursor(hCursor);
        GlobalUnlock(hGlobal);
        return TRUE;
    }

    GetCursorPos(&pt1);
    pt2 = pt1;
    ScreenToClient(hWnd, &pt2);

    iKey = C1_HitTest(&pt2);
    if (iKey == -1)
        hCursor = LoadCursorW(0, (LPCWSTR)IDC_SIZEALL);
    else
        hCursor = LoadCursorW(0, (LPCWSTR)IDC_HAND);
    SetCursor(hCursor);

    if (HIWORD(lParam) == WM_LBUTTONDOWN)
    {
        pC1->pt1 = pt1;
        pC1->pt2 = pt2;
        pC1->iPressedKey = iKey;
        C1_OnButtonDown(hWnd, pC1);
    }

    GlobalUnlock(hGlobal);
    return TRUE;
}

// Win: SKC1MouseMove
BOOL C1_OnMouseMove(HWND hWnd, WPARAM wParam, LPARAM lParam)
{
    HGLOBAL hGlobal;
    PC1WINDOW pC1;
    HDC hMemDC;
    DWORD dwFlags;
    INT iPressedKey;
    POINT pt;
    HGDIOBJ hbmOld;
    HDC hDC;
    INT iKey;

    hGlobal = (HGLOBAL)GetWindowLongW(hWnd, 0);
    pC1 = (PC1WINDOW)GlobalLock(hGlobal);
    if (!hGlobal || !pC1)
        return FALSE;

    if (pC1->dwFlags & FLAG_DRAGGING)
    {
        C1_DrawDragBorder(hWnd, &pC1->pt1, &pC1->pt2);
        GetCursorPos(&pC1->pt1);
        C1_DrawDragBorder(hWnd, &pC1->pt1, &pC1->pt2);
        GlobalUnlock(hGlobal);
        return TRUE;
    }

    if (pC1->iPressedKey != -1)
    {
        GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);
        iKey = C1_HitTest(&pt);

        hDC = GetDC(hWnd);
        hMemDC = CreateCompatibleDC(hDC);
        hbmOld = SelectObject(hMemDC, pC1->hbmKeyboard);
        dwFlags = pC1->dwFlags;

        iPressedKey = pC1->iPressedKey;
        if (!!(dwFlags & FLAG_PRESSED) == (iKey != iPressedKey))
        {
            if (iPressedKey != C1K_SHIFT || !(dwFlags & FLAG_SHIFT_PRESSED))
            {
                C1_InvertButton(hDC, iPressedKey);
                C1_InvertButton(hMemDC, pC1->iPressedKey);
            }

            pC1->dwFlags ^= FLAG_PRESSED;
        }

        SelectObject(hMemDC, hbmOld);
        DeleteDC(hMemDC);
        ReleaseDC(hWnd, hDC);
    }

    GlobalUnlock(hGlobal);
    return TRUE;
}

// Win: SKC1ButtonUp
BOOL C1_OnButtonUp(HWND hWnd, WPARAM wParam, LPARAM lParam)
{
    HGLOBAL hGlobal;
    PC1WINDOW pC1;
    BOOL ret = FALSE;
    INT x, y, iKey;
    HDC hDC, hMemDC;
    INT iPressedKey;
    HGDIOBJ hbmOld;
#ifndef STANDALONE
    HIMC hIMC;
    HWND hwndOwner;
    LPINPUTCONTEXT pIC;
#endif

    hGlobal = (HGLOBAL)GetWindowLongPtrW(hWnd, 0);
    pC1 = (PC1WINDOW)GlobalLock(hGlobal);
    if (!hGlobal || !pC1)
        return FALSE;

    ReleaseCapture();

    if (pC1->dwFlags & FLAG_DRAGGING)
    {
        pC1->dwFlags &= ~FLAG_DRAGGING;
        C1_DrawDragBorder(hWnd, &pC1->pt1, &pC1->pt2);
        x = pC1->pt1.x - pC1->pt2.x;
        y = pC1->pt1.y - pC1->pt2.y;
        SetWindowPos(hWnd, 0, x, y, 0, 0, 0x15u);
        ret = TRUE;
#ifndef STANDALONE
        hwndOwner = GetWindow(hWnd, GW_OWNER);
        hIMC = (HIMC)GetWindowLongPtrW(hwndOwner, 0);
        if (hIMC)
        {
            pIC = ImmLockIMC(hIMC);
            if (pIC)
            {
                pIC->fdwInit |= INIT_SOFTKBDPOS;
                pIC->ptSoftKbdPos.x = x;
                pIC->ptSoftKbdPos.y = y;
                ImmUnlockIMC(hIMC);
            }
        }
#endif
        GlobalUnlock(hGlobal);
        return ret;
    }

    iKey = pC1->iPressedKey;
    if (iKey == -1)
        return FALSE;

    if (!(pC1->dwFlags & FLAG_PRESSED))
    {
        pC1->iPressedKey = -1;
        GlobalUnlock(hGlobal);
        return ret;
    }

    if (iKey == C1K_SHIFT)
    {
        if (!(pC1->dwFlags & FLAG_SHIFT_PRESSED))
        {
            pC1->dwFlags |= FLAG_SHIFT_PRESSED;
            pC1->dwFlags &= ~FLAG_PRESSED;
            pC1->iPressedKey = -1;
            GlobalUnlock(hGlobal);
            return ret;
        }
    }
    else if (iKey < C1K_BACKSPACE && (pC1->dwFlags & FLAG_SHIFT_PRESSED))
    {
        INT iVK = gC1K2VK[pC1->iPressedKey];
        keybd_event(VK_SHIFT, guScanCode[C1K_SHIFT], 0, 0);
        keybd_event(iVK, guScanCode[(BYTE)iVK], 0, 0);
        keybd_event(iVK, guScanCode[(BYTE)iVK], KEYEVENTF_KEYUP, 0);
        keybd_event(VK_SHIFT, guScanCode[C1K_SHIFT], KEYEVENTF_KEYUP, 0);
    }
    else
    {
        INT iVK = gC1K2VK[iKey];
        keybd_event(iVK, guScanCode[iVK], 0, 0);
        keybd_event(iVK, guScanCode[iVK], KEYEVENTF_KEYUP, 0);
    }

    ret = TRUE;

    hDC = GetDC(hWnd);
    hMemDC = CreateCompatibleDC(hDC);
    hbmOld = SelectObject(hMemDC, pC1->hbmKeyboard);

    C1_InvertButton(hDC, pC1->iPressedKey);
    C1_InvertButton(hMemDC, pC1->iPressedKey);

    if (pC1->iPressedKey < C1K_BACKSPACE && (pC1->dwFlags & FLAG_SHIFT_PRESSED))
    {
        C1_InvertButton(hDC, C1K_SHIFT);
        C1_InvertButton(hMemDC, C1K_SHIFT);
    }

    if (pC1->iPressedKey < C1K_BACKSPACE || pC1->iPressedKey == C1K_SHIFT)
        pC1->dwFlags &= ~FLAG_SHIFT_PRESSED;

    SelectObject(hMemDC, hbmOld);
    DeleteDC(hMemDC);
    ReleaseDC(hWnd, hDC);

    pC1->dwFlags &= ~FLAG_PRESSED;
    pC1->iPressedKey = -1;
    GlobalUnlock(hGlobal);
    return ret;
}

// Win: DestroyC1Window
void C1_OnDestroy(HWND hWnd)
{
    HGLOBAL hGlobal;
    PC1WINDOW pC1;
    HWND hwndOwner;

    hGlobal = (HGLOBAL)GetWindowLongPtrW(hWnd, 0);
    pC1 = (PC1WINDOW)GlobalLock(hGlobal);
    if (!hGlobal || !pC1)
        return;

    if (pC1->dwFlags & FLAG_DRAGGING)
        C1_DrawDragBorder(hWnd, &pC1->pt1, &pC1->pt2);

    DeleteObject(pC1->hbmKeyboard);
    GlobalUnlock(hGlobal);
    GlobalFree(hGlobal);

#ifdef STANDALONE
    PostQuitMessage(0);
#else
    hwndOwner = GetWindow(hWnd, GW_OWNER);
    if (hwndOwner)
        SendMessageW(hwndOwner, WM_IME_NOTIFY, IMN_SOFTKBDDESTROYED, 0);
#endif
}

// Win: (None)
LRESULT C1_OnImeControl(HWND hWnd, WPARAM wParam, LPARAM lParam)
{
    HGLOBAL hGlobal;
    PC1WINDOW pC1;
    LOGFONTW lf;
    RECT rc;
    LRESULT ret = 0;

    switch (wParam)
    {
        case IMC_GETSOFTKBDFONT:
        {
            HDC hDC = GetDC(hWnd);
            GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(LOGFONTW), &lf);
            ReleaseDC(hWnd, hDC);
            *(LPLOGFONTW)lParam = lf;
            break;
        }
        case IMC_SETSOFTKBDFONT:
        {
            LPLOGFONTW plf = (LPLOGFONTW)lParam;
            LOGFONTW lf;
            GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(LOGFONTW), &lf);
            if (lf.lfCharSet == plf->lfCharSet)
                return 0;

            hGlobal = (HGLOBAL)GetWindowLongPtrW(hWnd, 0);
            pC1 = (PC1WINDOW)GlobalLock(hGlobal);
            if (!hGlobal || !pC1)
                return 1;

            pC1->CharSet = plf->lfCharSet;
            GlobalUnlock(hGlobal);
            break;
        }
        case IMC_GETSOFTKBDPOS:
        {
            GetWindowRect(hWnd, &rc);
            return MAKELRESULT(rc.left, rc.top);
        }
        case IMC_SETSOFTKBDPOS:
        {
            POINT pt;
            POINTSTOPOINT(pt, lParam);
            SetWindowPos(hWnd, NULL, pt.x, pt.y, 0, 0,
                         (SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE));
            break;
        }
        case IMC_GETSOFTKBDSUBTYPE:
        case IMC_SETSOFTKBDSUBTYPE:
        {
            hGlobal = (HGLOBAL)GetWindowLongPtrW(hWnd, 0);
            pC1 = (PC1WINDOW)GlobalLock(hGlobal);
            if (!hGlobal || !pC1)
                return -1;
            ret = pC1->SubType;
            if (wParam == IMC_SETSOFTKBDSUBTYPE)
                pC1->SubType = lParam;
            break;
        }
        case IMC_SETSOFTKBDDATA:
        {
            if (C1_SetData(hWnd, (SOFTKBDDATA*)lParam))
                return -1;

            InvalidateRect(hWnd, 0, 0);
        }
        default:
            break;
    }

    return ret;
}

// Win: SKWndProcC1
LRESULT CALLBACK
C1_WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    LRESULT ret = 0;

    switch (uMsg)
    {
        case WM_CREATE:
        {
            return C1_OnCreate(hWnd);
        }
        case WM_DESTROY:
        {
            C1_OnDestroy(hWnd);
            break;
        }
        case WM_SETCURSOR:
        {
            if (C1_OnSetCursor(hWnd, lParam))
                break;
            return DefWindowProcW(hWnd, uMsg, wParam, lParam);
        }
        case WM_MOUSEMOVE:
        {
            if (C1_OnMouseMove(hWnd, wParam, lParam))
                break;
            return DefWindowProcW(hWnd, uMsg, wParam, lParam);
        }
        case WM_LBUTTONUP:
        {
            if (C1_OnButtonUp(hWnd, wParam, lParam))
                break;
            return DefWindowProcW(hWnd, uMsg, wParam, lParam);
        }
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hDC = BeginPaint(hWnd, &ps);
            C1_OnDraw(hDC, hWnd);
            EndPaint(hWnd, &ps);
            break;
        }
        case WM_IME_CONTROL:
        {
            return C1_OnImeControl(hWnd, wParam, lParam);
        }
        case WM_MOUSEACTIVATE:
        {
            return MA_NOACTIVATE;
        }
        default:
        {
            return DefWindowProcW(hWnd, uMsg, wParam, lParam);
        }
    }
    return 0;
}

/***********************************************************************/

static BOOL
Imm32RegisterSoftKeyboard(
    _In_ UINT uType)
{
    WNDCLASSEXW wcx;
    LPCWSTR pszClass = ((uType == 1) ? T1_CLASSNAMEW : C1_CLASSNAMEW);
    if (GetClassInfoExW(ghImm32Inst, pszClass, &wcx))
        return TRUE;

    ZeroMemory(&wcx, sizeof(wcx));
    wcx.cbSize        = sizeof(wcx);
    wcx.style         = CS_IME;
    wcx.cbWndExtra    = sizeof(PT1WINDOW);
    wcx.hIcon         = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wcx.hInstance     = ghImm32Inst;
    wcx.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL);
    wcx.lpszClassName = pszClass;

    if (uType == 1)
    {
        wcx.lpfnWndProc = T1_WindowProc;
        wcx.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    }
    else
    {
        wcx.lpfnWndProc = C1_WindowProc;
        wcx.hbrBackground = (HBRUSH)GetStockObject(LTGRAY_BRUSH);
    }

    return !!RegisterClassExW(&wcx);
}

static void
Imm32GetSoftKeyboardDimension(
    _In_ UINT uType,
    _Out_ LPINT pcx,
    _Out_ LPINT pcy)
{
    if (uType == 1)
    {
#if 0
        TEXTMETRICW tm;
        T1_GetTextMetric(&tm);
        *pcx = 15 * tm.tmMaxCharWidth + 2 * gptRaiseEdge.x + 139;
        *pcy = 5 * tm.tmHeight + 2 * gptRaiseEdge.y + 58;
#endif
    }
    else
    {
        INT cxEdge = GetSystemMetrics(SM_CXEDGE), cyEdge = GetSystemMetrics(SM_CXEDGE);
        *pcx = 2 * (GetSystemMetrics(SM_CXBORDER) + cxEdge) + 348;
        *pcy = 2 * (GetSystemMetrics(SM_CYBORDER) + cyEdge) + 136;
    }
}

/***********************************************************************
 *		ImmCreateSoftKeyboard (IMM32.@)
 *
 * @see https://katahiromz.web.fc2.com/colony3rd/imehackerz/en/ImmCreateSoftKeyboard.html
 */
HWND WINAPI
ImmCreateSoftKeyboard(
    _In_ UINT uType,
    _In_ HWND hwndParent,
    _In_ INT x,
    _In_ INT y)
{
    HKL hKL;
    UINT iVK;
    INT xSoftKBD, ySoftKBD, cxSoftKBD, cySoftKBD, cxEdge, cyEdge;
    HWND hwndSoftKBD;
    DWORD Style, ExStyle, UICaps;
    LPCWSTR pszClass;
    RECT rcWorkArea;
#ifndef STANDALONE
    PIMEDPI pImeDpi;
#endif

    if (uType != 1 && uType != 2)
        return NULL; /* Invalid keyboard type */

#ifndef STANDALONE
    /* Check IME */
    hKL = GetKeyboardLayout(0);
    pImeDpi = ImmLockImeDpi(hKL);
    if (!pImeDpi)
        return NULL; /* No IME */

    UICaps = pImeDpi->ImeInfo.fdwUICaps;
    ImmUnlockImeDpi(pImeDpi);

    /* Check IME capability */
    if (!(UICaps & UI_CAP_SOFTKBD))
        return NULL; /* No capability for soft keyboard */
#endif

    /* Want metrics? */
    if (g_bWantSoftKBDMetrics)
    {
        for (iVK = 0; iVK < 0xFF; ++iVK)
        {
            guScanCode[iVK] = MapVirtualKeyW(iVK, 0);
        }

        cxEdge = GetSystemMetrics(SM_CXEDGE);
        cyEdge = GetSystemMetrics(SM_CYEDGE);
        gptRaiseEdge.x = GetSystemMetrics(SM_CXBORDER) + cxEdge;
        gptRaiseEdge.y = GetSystemMetrics(SM_CYBORDER) + cyEdge;

        g_bWantSoftKBDMetrics = FALSE;
    }

    if (!Imm32GetNearestWorkArea(hwndParent, &rcWorkArea))
        return NULL;

    /* Register the window class */
    if (!Imm32RegisterSoftKeyboard(uType))
        return NULL;

    /* Calculate keyboard size */
    Imm32GetSoftKeyboardDimension(uType, &cxSoftKBD, &cySoftKBD);

    /* Adjust keyboard position */
    xSoftKBD = Imm32Clamp(x, rcWorkArea.left, rcWorkArea.right  - cxSoftKBD);
    ySoftKBD = Imm32Clamp(y, rcWorkArea.top , rcWorkArea.bottom - cySoftKBD);

    /* Create soft keyboard window */
    if (uType == 1)
    {
        Style = (WS_POPUP | WS_DISABLED);
        ExStyle = 0;
        pszClass = T1_CLASSNAMEW;
    }
    else
    {
        Style = (WS_POPUP | WS_DISABLED | WS_BORDER);
        ExStyle = (WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME);
        pszClass = C1_CLASSNAMEW;
    }
    hwndSoftKBD = CreateWindowExW(ExStyle, pszClass, NULL, Style,
                                  xSoftKBD, ySoftKBD, cxSoftKBD, cySoftKBD,
                                  hwndParent, NULL, ghImm32Inst, NULL);
    /* Initial is hidden */
    ShowWindow(hwndSoftKBD, SW_HIDE);
    UpdateWindow(hwndSoftKBD);

    return hwndSoftKBD;
}

/***********************************************************************
 *		ImmShowSoftKeyboard (IMM32.@)
 *
 * @see https://katahiromz.web.fc2.com/colony3rd/imehackerz/en/ImmShowSoftKeyboard.html
 */
BOOL WINAPI
ImmShowSoftKeyboard(
    _In_ HWND hwndSoftKBD,
    _In_ INT nCmdShow)
{
    return hwndSoftKBD && ShowWindow(hwndSoftKBD, nCmdShow);
}

/***********************************************************************
 *		ImmDestroySoftKeyboard (IMM32.@)
 *
 * @see https://katahiromz.web.fc2.com/colony3rd/imehackerz/en/ImmDestroySoftKeyboard.html
 */
BOOL WINAPI
ImmDestroySoftKeyboard(
    _In_ HWND hwndSoftKBD)
{
    return DestroyWindow(hwndSoftKBD);
}

INT WINAPI
WinMain(HINSTANCE   hInstance,
        HINSTANCE   hPrevInstance,
        LPSTR       lpCmdLine,
        INT         nCmdShow)
{
    ghImm32Inst = hInstance;
    HWND hwndSoftKBD = ImmCreateSoftKeyboard(2, NULL, 0, 0);

    ImmShowSoftKeyboard(hwndSoftKBD, SW_SHOWNOACTIVATE);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (INT)msg.wParam;
}
