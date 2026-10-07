// Live Discord Send - background app (system tray). Receives the plugin's audio and plays it on a virtual
// audio cable so Discord sees it as a microphone. Auto-starts with Windows, reconnects by itself.
#include "engine.h"
#include <shellapi.h>
#include <shlobj.h>

#define WM_TRAY (WM_APP + 1)
enum { ID_STATUS = 1, ID_AUTOSTART, ID_CONFIG, ID_EXIT };
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName = L"LiveDiscordSend";

static std::atomic<bool> g_stop{false};
static Stats g_stats;
static Config g_cfg;
static std::mutex g_msgMutex; static std::wstring g_msg = L"Iniciando...";
static HWND g_hwnd; static NOTIFYICONDATAW g_nid{}; static UINT g_taskbarCreated;
static std::wstring g_iniPath;
static uint64_t g_lastPackets = 0; static ULONGLONG g_lastPacketTick = 0;

static void setMsg(const std::wstring& m) { std::lock_guard<std::mutex> l(g_msgMutex); g_msg = m; }
static std::wstring getMsg() { std::lock_guard<std::mutex> l(g_msgMutex); return g_msg; }

static std::wstring exePath() { wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH); return p; }

static bool autostartEnabled()
{
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}
static void setAutostart(bool on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) { std::wstring v = L"\"" + exePath() + L"\""; RegSetValueExW(k, kRunName, 0, REG_SZ, (const BYTE*) v.c_str(), (DWORD) ((v.size() + 1) * sizeof(wchar_t))); }
    else RegDeleteValueW(k, kRunName);
    RegCloseKey(k);
}

static void loadConfig()
{
    wchar_t appdata[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    std::wstring dir = std::wstring(appdata) + L"\\LiveDiscordSend";
    CreateDirectoryW(dir.c_str(), nullptr);
    g_iniPath = dir + L"\\ldsc.ini";
    if (GetFileAttributesW(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        WritePrivateProfileStringW(L"ldsc", L"device", L"CABLE Input", g_iniPath.c_str());
        WritePrivateProfileStringW(L"ldsc", L"port", L"9955", g_iniPath.c_str());
        WritePrivateProfileStringW(L"ldsc", L"prebuffer_ms", L"10", g_iniPath.c_str());
        WritePrivateProfileStringW(L"ldsc", L"max_ms", L"40", g_iniPath.c_str());
    }
    wchar_t dev[256];
    GetPrivateProfileStringW(L"ldsc", L"device", L"CABLE Input", dev, 256, g_iniPath.c_str());
    g_cfg.device = dev;
    g_cfg.port = (int) GetPrivateProfileIntW(L"ldsc", L"port", 9955, g_iniPath.c_str());
    g_cfg.prebufferMs = (int) GetPrivateProfileIntW(L"ldsc", L"prebuffer_ms", 10, g_iniPath.c_str());
    g_cfg.maxMs = (int) GetPrivateProfileIntW(L"ldsc", L"max_ms", 40, g_iniPath.c_str());
}

static std::wstring statusText()
{
    if (g_stats.running)
    {
        uint64_t p = g_stats.packets;
        ULONGLONG now = GetTickCount64();
        if (p != g_lastPackets) { g_lastPackets = p; g_lastPacketTick = now; }
        std::wstring dev = g_stats.getDevice();
        if (g_lastPacketTick && now - g_lastPacketTick < 2000) return L"Transmitiendo a " + dev;
        return L"Esperando audio de Ableton (plugin en el Master)";
    }
    return getMsg();
}

static void updateTip()
{
    std::wstring t = L"Live Discord Send - " + statusText();
    wcsncpy_s(g_nid.szTip, t.c_str(), _TRUNCATE);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void addIcon()
{
    g_nid = {}; g_nid.cbSize = sizeof g_nid; g_nid.hWnd = g_hwnd; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(g_nid.szTip, L"Live Discord Send", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void showMenu()
{
    HMENU m = CreatePopupMenu();
    std::wstring s = statusText();
    AppendMenuW(m, MF_STRING | MF_DISABLED, ID_STATUS, s.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (autostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Iniciar con Windows");
    AppendMenuW(m, MF_STRING, ID_CONFIG, L"Abrir configuración (reinicia la app para aplicar)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Salir");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
}

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbarCreated) { addIcon(); return 0; }
    switch (msg)
    {
    case WM_TIMER: updateTip(); return 0;
    case WM_TRAY: if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) showMenu(); return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_AUTOSTART) setAutostart(!autostartEnabled());
        else if (LOWORD(wp) == ID_CONFIG) ShellExecuteW(nullptr, L"open", L"notepad.exe", g_iniPath.c_str(), nullptr, SW_SHOWNORMAL);
        else if (LOWORD(wp) == ID_EXIT) DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i)
    {
        if (!wcscmp(argv[i], L"--uninstall")) { setAutostart(false); return 0; }
        if (!wcscmp(argv[i], L"--install-autostart")) setAutostart(true);
    }

    HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\LiveDiscordSendTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0; // already running

    loadConfig();
    WNDCLASSW wc{}; wc.lpfnWndProc = wndProc; wc.hInstance = inst; wc.lpszClassName = L"LiveDiscordSendTray";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Live Discord Send", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    addIcon();
    SetTimer(g_hwnd, 1, 1000, nullptr);

    std::thread worker([] {
        Log log = [](const std::wstring& m) { setMsg(m); };
        while (!g_stop)
        {
            Result r = runEngine(g_cfg, g_stop, g_stats, log);
            if (r == Result::Stopped) break;
            for (int i = 0; i < 30 && !g_stop; ++i) Sleep(100); // retry every 3 s
        }
    });

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }

    g_stop = true; worker.join();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    CloseHandle(once);
    return 0;
}
