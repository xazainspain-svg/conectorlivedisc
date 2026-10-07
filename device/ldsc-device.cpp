// ldsc-device (console): receives the Live Discord Send UDP stream and plays it on a Windows render endpoint
// (e.g. "CABLE Input"). Reconnects automatically. For the background/tray version see ldsc-tray.cpp.
#include "engine.h"

static std::atomic<bool> g_stop{false};
static BOOL WINAPI onCtrl(DWORD) { g_stop = true; return TRUE; }

int wmain(int argc, wchar_t** argv)
{
    Config cfg; bool list = false;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--list") list = true;
        else if (a == L"--device") cfg.device = next();
        else if (a == L"--port") cfg.port = _wtoi(next().c_str());
        else if (a == L"--prebuffer-ms") cfg.prebufferMs = _wtoi(next().c_str());
        else if (a == L"--max-ms") cfg.maxMs = _wtoi(next().c_str());
        else { wprintf(L"Uso: ldsc-device [--list] [--device \"nombre\"] [--port 9955] [--prebuffer-ms 10] [--max-ms 40]\n"); return 1; }
    }
    if (list)
    {
        wprintf(L"Dispositivos de salida:\n");
        for (auto& n : ldsc::listRenderDevices()) wprintf(L"  - %ls\n", n.c_str());
        return 0;
    }
    SetConsoleCtrlHandler(onCtrl, TRUE);
    Stats st;
    Log log = [](const std::wstring& m) { wprintf(L"%ls\n", m.c_str()); fflush(stdout); };
    log(L"Selecciona \"CABLE Output\" como micrófono en Discord. Ctrl+C para salir.");
    while (!g_stop)
    {
        if (runEngine(cfg, g_stop, st, log) == Result::Stopped) break;
        for (int i = 0; i < 30 && !g_stop; ++i) Sleep(100);
    }
    return 0;
}
