// GifFace.cpp
// Win32 + GDI+ animated GIF overlay with:
// - True per-pixel transparency (transparent GIFs work) via UpdateLayeredWindow (ULW_ALPHA)
// - Click-through
// - No taskbar / Alt-Tab
// - Always on top
// - Bounces on screen edges
// - Loads GIF from HTTP URL (downloads to %TEMP% first)
// Exit hotkey: Ctrl + Alt + Q

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <gdiplus.h>
#include <wininet.h>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "wininet.lib")

using namespace Gdiplus;

// ----------------------------- Globals --------------------------------

static ULONG_PTR g_gdiplusToken = 0;

static Image* g_gif = nullptr;
static GUID   g_frameDimGuid = {};
static UINT   g_frameCount = 0;
static UINT   g_frameIndex = 0;
static std::vector<UINT> g_delaysMs;

// Timers
static const UINT_PTR g_gifTimerId  = 1;
static const UINT_PTR g_moveTimerId = 2;

// Hotkey (Ctrl+Alt+Q)
static const int g_hotkeyId = 1001;

// Movement state
static int g_x = 500, g_y = 300;
static int g_vx = 6, g_vy = 5;
static int g_w = 320, g_h = 240;

// Layered-window backing store (32-bit ARGB DIB)
static HBITMAP g_hDib = nullptr;
static void*   g_dibBits = nullptr;
static HDC     g_memDC = nullptr;
static int     g_stride = 0;

// Temp download path (optional cleanup)
static wchar_t g_tempGifPath[MAX_PATH]{};

// ----------------------------- Utilities --------------------------------

static void SafeDeleteGif()
{
    delete g_gif;
    g_gif = nullptr;
    g_frameCount = 0;
    g_frameIndex = 0;
    g_delaysMs.clear();
}

static void FreeBackbuffer()
{
    if (g_memDC) { DeleteDC(g_memDC); g_memDC = nullptr; }
    if (g_hDib)  { DeleteObject(g_hDib); g_hDib = nullptr; }
    g_dibBits = nullptr;
    g_stride = 0;
}

static void LoadGifDelays(Image* img)
{
    g_delaysMs.clear();
    const PROPID FrameDelay = 0x5100; // array of UINTs, unit = 1/100 sec

    UINT size = img->GetPropertyItemSize(FrameDelay);
    if (size == 0)
    {
        g_delaysMs.assign(g_frameCount, 100);
        return;
    }

    std::vector<BYTE> buf(size);
    PropertyItem* pi = reinterpret_cast<PropertyItem*>(buf.data());
    if (img->GetPropertyItem(FrameDelay, size, pi) != Ok || pi->length < 4)
    {
        g_delaysMs.assign(g_frameCount, 100);
        return;
    }

    const UINT count = pi->length / 4;
    g_delaysMs.resize(g_frameCount, 100);

    UINT* delays = reinterpret_cast<UINT*>(pi->value);
    for (UINT i = 0; i < g_frameCount; i++)
    {
        UINT d = (i < count) ? delays[i] : delays[count - 1];
        UINT ms = d * 10; // 1/100 sec -> ms
        if (ms < 10) ms = 10; // guard against 0
        g_delaysMs[i] = ms;
    }
}

static bool InitGifFromFile(const wchar_t* path)
{
    SafeDeleteGif();

    g_gif = new Image(path);
    if (!g_gif || g_gif->GetLastStatus() != Ok)
        return false;

    UINT dimCount = g_gif->GetFrameDimensionsCount();
    if (dimCount == 0)
        return false;

    std::vector<GUID> dims(dimCount);
    g_gif->GetFrameDimensionsList(dims.data(), dimCount);

    g_frameDimGuid = dims[0];
    g_frameCount = g_gif->GetFrameCount(&g_frameDimGuid);
    if (g_frameCount == 0)
        return false;

    g_frameIndex = 0;

    g_w = (int)g_gif->GetWidth();
    g_h = (int)g_gif->GetHeight();

    LoadGifDelays(g_gif);
    return true;
}

static bool CreateBackbuffer(int w, int h)
{
    FreeBackbuffer();

    HDC screenDC = GetDC(nullptr);
    if (!screenDC) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down DIB
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    g_memDC = CreateCompatibleDC(screenDC);
    if (!g_memDC)
    {
        ReleaseDC(nullptr, screenDC);
        return false;
    }

    g_hDib = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &g_dibBits, nullptr, 0);
    ReleaseDC(nullptr, screenDC);

    if (!g_hDib || !g_dibBits)
    {
        FreeBackbuffer();
        return false;
    }

    SelectObject(g_memDC, g_hDib);
    g_stride = w * 4;
    return true;
}

static void ApplyOverlayStyles(HWND hwnd)
{
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    ex |= (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
    ex &= ~WS_EX_APPWINDOW;
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
}

static void BounceStep()
{
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);

    g_x += g_vx;
    g_y += g_vy;

    if (g_x < 0) { g_x = 0; g_vx = -g_vx; }
    if (g_y < 0) { g_y = 0; g_vy = -g_vy; }

    if (g_x + g_w > screenW) { g_x = screenW - g_w; g_vx = -g_vx; }
    if (g_y + g_h > screenH) { g_y = screenH - g_h; g_vy = -g_vy; }
}

// WinINet download to a temp file
static bool DownloadToTempFile(const wchar_t* url, wchar_t* outPath)
{
    wchar_t tempDir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tempDir))
        return false;

    if (!GetTempFileNameW(tempDir, L"gfc", 0, outPath))
        return false;

    HINTERNET hInet = InternetOpenW(L"GifFace", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) return false;

    HINTERNET hUrl = InternetOpenUrlW(
        hInet,
        url,
        nullptr,
        0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE,
        0
    );

    if (!hUrl)
    {
        InternetCloseHandle(hInet);
        return false;
    }

    HANDLE hFile = CreateFileW(outPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInet);
        return false;
    }

    BYTE  buffer[8192];
    DWORD bytesRead = 0;
    DWORD bytesWritten = 0;

    bool ok = true;
    while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0)
    {
        if (!WriteFile(hFile, buffer, bytesRead, &bytesWritten, nullptr) || bytesWritten != bytesRead)
        {
            ok = false;
            break;
        }
    }

    CloseHandle(hFile);
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInet);

    if (!ok)
    {
        DeleteFileW(outPath);
        outPath[0] = L'\0';
        return false;
    }

    return true;
}

// Render current frame into ARGB DIB and push it with per-pixel alpha
static void RenderLayered(HWND hwnd)
{
    if (!g_gif || !g_memDC || !g_dibBits) return;

    // Clear backbuffer to fully transparent
    ZeroMemory(g_dibBits, (size_t)g_stride * (size_t)g_h);

    // Select frame and draw to backbuffer
    g_gif->SelectActiveFrame(&g_frameDimGuid, g_frameIndex);

    Graphics gfx(g_memDC);
    gfx.SetCompositingMode(CompositingModeSourceOver);
    gfx.SetCompositingQuality(CompositingQualityHighQuality);
    gfx.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    gfx.SetSmoothingMode(SmoothingModeHighQuality);
    gfx.DrawImage(g_gif, 0, 0, g_w, g_h);

    HDC screenDC = GetDC(nullptr);

    SIZE  size{ g_w, g_h };
    POINT ptSrc{ 0, 0 };
    POINT ptDst{ g_x, g_y };

    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hwnd, screenDC, &ptDst, &size, g_memDC, &ptSrc, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screenDC);
}

// ----------------------------- Window Proc --------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        // CHANGE THIS URL
        const wchar_t* gifUrl = L"http://192.168.1.20:9051/download/Laughing.gif";

        if (!DownloadToTempFile(gifUrl, g_tempGifPath))
        {
            MessageBoxW(hwnd, L"Failed to download GIF.", L"GifFace", MB_ICONERROR);
            return -1;
        }

        if (!InitGifFromFile(g_tempGifPath))
        {
            MessageBoxW(hwnd, L"Downloaded GIF could not be loaded.", L"GifFace", MB_ICONERROR);
            return -1;
        }

        if (!CreateBackbuffer(g_w, g_h))
        {
            MessageBoxW(hwnd, L"Failed to create ARGB backbuffer.", L"GifFace", MB_ICONERROR);
            return -1;
        }

        ApplyOverlayStyles(hwnd);

        // Hotkey to close
        RegisterHotKey(hwnd, g_hotkeyId, MOD_CONTROL | MOD_ALT, 'Q');

        // Start timers
        SetTimer(hwnd, g_gifTimerId, g_delaysMs.empty() ? 100 : g_delaysMs[0], nullptr);
        SetTimer(hwnd, g_moveTimerId, 16, nullptr); // ~60fps movement

        // Initial draw (also positions the window)
        RenderLayered(hwnd);
        return 0;
    }

    case WM_NCHITTEST:
        // True click-through
        return HTTRANSPARENT;

    case WM_ERASEBKGND:
        // We render via UpdateLayeredWindow; no background erase.
        return 1;

    case WM_HOTKEY:
        if ((int)wParam == g_hotkeyId)
            DestroyWindow(hwnd);
        return 0;

    case WM_TIMER:
        if (wParam == g_gifTimerId && g_gif && g_frameCount > 0)
        {
            g_frameIndex = (g_frameIndex + 1) % g_frameCount;

            UINT nextDelay = (g_frameIndex < g_delaysMs.size()) ? g_delaysMs[g_frameIndex] : 100;
            KillTimer(hwnd, g_gifTimerId);
            SetTimer(hwnd, g_gifTimerId, nextDelay, nullptr);

            RenderLayered(hwnd);
            return 0;
        }
        if (wParam == g_moveTimerId)
        {
            BounceStep();
            RenderLayered(hwnd);
            return 0;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, g_gifTimerId);
        KillTimer(hwnd, g_moveTimerId);
        UnregisterHotKey(hwnd, g_hotkeyId);

        FreeBackbuffer();
        SafeDeleteGif();

        // Optional: delete the downloaded temp file
        if (g_tempGifPath[0] != L'\0')
        {
            DeleteFileW(g_tempGifPath);
            g_tempGifPath[0] = L'\0';
        }

        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ----------------------------- Entry --------------------------------

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    GdiplusStartupInput si;
    if (GdiplusStartup(&g_gdiplusToken, &si, nullptr) != Ok)
        return 0;

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"GifFaceWindow";
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    RegisterClassW(&wc);

    // Layered + toolwindow + transparent (mouse) + topmost
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        wc.lpszClassName,
        L"",
        WS_POPUP,
        g_x, g_y, g_w, g_h,
        nullptr, nullptr, hInst, nullptr
    );

    if (!hwnd)
    {
        GdiplusShutdown(g_gdiplusToken);
        return 0;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(g_gdiplusToken);
    return 0;
}
