// ldsc-device: receives the Live Discord Send UDP stream and plays it on a Windows
// render endpoint (e.g. "CABLE Input" from a virtual audio cable) via low-latency WASAPI.
// Then pick the cable's capture side ("CABLE Output") as the microphone in Discord.
#include <winsock2.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static std::atomic<bool> g_stop{false};
static BOOL WINAPI onCtrl(DWORD) { g_stop = true; return TRUE; }

template <class T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

static std::wstring lower(std::wstring s) { for (auto& c : s) c = (wchar_t) towlower(c); return s; }
static std::wstring widen(const char* s) { int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0); std::wstring w(n ? n - 1 : 0, L'\0'); if (n) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n); return w; }

static std::wstring friendlyName(IMMDevice* d)
{
    IPropertyStore* ps = nullptr; PROPVARIANT v; PropVariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) out = v.pwszVal;
    PropVariantClear(&v); rel(ps);
    return out;
}

// ---- single-producer/single-consumer ring of stereo float frames ----
struct Ring
{
    static constexpr uint32_t N = 1u << 16; // frames (power of two)
    std::vector<float> data = std::vector<float>(N * 2, 0.f);
    std::atomic<uint64_t> w{0}, r{0};
    uint64_t available() const { return w.load(std::memory_order_acquire) - r.load(std::memory_order_acquire); }
    void push(const float* lr, size_t frames)
    {
        uint64_t wi = w.load(std::memory_order_relaxed);
        if (wi + frames - r.load(std::memory_order_acquire) > N) return; // full: drop
        for (size_t i = 0; i < frames; ++i) { size_t k = (size_t) ((wi + i) & (N - 1)) * 2; data[k] = lr[i * 2]; data[k + 1] = lr[i * 2 + 1]; }
        w.store(wi + frames, std::memory_order_release);
    }
    void pop(float* lr, size_t frames)
    {
        uint64_t ri = r.load(std::memory_order_relaxed);
        for (size_t i = 0; i < frames; ++i) { size_t k = (size_t) ((ri + i) & (N - 1)) * 2; lr[i * 2] = data[k]; lr[i * 2 + 1] = data[k + 1]; }
        r.store(ri + frames, std::memory_order_release);
    }
    void skip(uint64_t frames) { r.store(r.load() + frames, std::memory_order_release); }
};

struct Resampler // streaming linear, stereo float
{
    double inRate = 0, outRate = 48000, pos = 0; float prev[2] = {0, 0}; bool has = false;
    void process(const float* in, size_t frames, double rateIn, std::vector<float>& out)
    {
        out.clear();
        if (!frames) return;
        if (rateIn != inRate) { inRate = rateIn; pos = 0; has = false; }
        if (!has) { prev[0] = in[0]; prev[1] = in[1]; has = true; }
        const double step = inRate / outRate;
        const size_t n = frames + 1;
        auto at = [&](size_t i, int ch) { return i == 0 ? prev[ch] : in[(i - 1) * 2 + ch]; };
        while ((size_t) pos + 1 < n)
        {
            size_t i = (size_t) pos; float f = (float) (pos - (double) i);
            for (int ch = 0; ch < 2; ++ch) { float a = at(i, ch); out.push_back(a + (at(i + 1, ch) - a) * f); }
            pos += step;
        }
        pos -= (double) (n - 1);
        prev[0] = in[(frames - 1) * 2]; prev[1] = in[(frames - 1) * 2 + 1];
    }
};

struct Config { std::wstring device = L"CABLE Input"; int port = 9955; int prebufferMs = 10; int maxMs = 40; bool list = false; };

int wmain(int argc, wchar_t** argv)
{
    Config cfg;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--list") cfg.list = true;
        else if (a == L"--device") cfg.device = next();
        else if (a == L"--port") cfg.port = _wtoi(next().c_str());
        else if (a == L"--prebuffer-ms") cfg.prebufferMs = _wtoi(next().c_str());
        else if (a == L"--max-ms") cfg.maxMs = _wtoi(next().c_str());
        else { wprintf(L"Uso: ldsc-device [--list] [--device \"nombre\"] [--port 9955] [--prebuffer-ms 10] [--max-ms 40]\n"); return 1; }
    }
    SetConsoleCtrlHandler(onCtrl, TRUE);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) { wprintf(L"No se pudo abrir WASAPI\n"); return 1; }
    IMMDeviceCollection* col = nullptr; en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    UINT count = 0; col->GetCount(&count);
    IMMDevice* dev = nullptr;
    if (cfg.list) wprintf(L"Dispositivos de salida:\n");
    for (UINT i = 0; i < count; ++i)
    {
        IMMDevice* d = nullptr; col->Item(i, &d);
        std::wstring name = friendlyName(d);
        if (cfg.list) wprintf(L"  - %ls\n", name.c_str());
        if (!dev && lower(name).find(lower(cfg.device)) != std::wstring::npos) { dev = d; wprintf(L"Dispositivo: %ls\n", name.c_str()); } else rel(d);
    }
    if (cfg.list) return 0;
    if (!dev)
    {
        wprintf(L"No encuentro un dispositivo que contenga \"%ls\".\nInstala un cable de audio virtual (p. ej. VB-CABLE, vb-audio.com/Cable) o usa --list y --device.\n", cfg.device.c_str());
        return 2;
    }

    IAudioClient3* ac = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void**) &ac))) { wprintf(L"IAudioClient3 no disponible (Windows 10+ requerido)\n"); return 1; }
    WAVEFORMATEX* fmt = nullptr; ac->GetMixFormat(&fmt);
    bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE*) fmt)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    if (!(isFloat && fmt->wBitsPerSample == 32) || fmt->nChannels < 2) { wprintf(L"Formato de mezcla no soportado (se espera float32 estéreo)\n"); return 1; }
    const int devCh = fmt->nChannels; const double devRate = fmt->nSamplesPerSec;

    UINT32 defP = 0, fundP = 0, minP = 0, maxP = 0;
    HRESULT hr = ac->GetSharedModeEnginePeriod(fmt, &defP, &fundP, &minP, &maxP);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(hr)) hr = ac->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minP, fmt, nullptr);
    if (FAILED(hr)) // fallback: regular shared stream, 10 ms
    {
        rel(ac); dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void**) &ac);
        hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000, 0, fmt, nullptr);
        minP = (UINT32) (devRate / 100);
    }
    if (FAILED(hr)) { wprintf(L"No se pudo inicializar el dispositivo (0x%08lx)\n", (unsigned long) hr); return 1; }
    ac->SetEventHandle(ev);
    IAudioRenderClient* rc = nullptr; ac->GetService(IID_PPV_ARGS(&rc));
    UINT32 bufFrames = 0; ac->GetBufferSize(&bufFrames);
    wprintf(L"Formato: %.0f Hz, %d canales, periodo del motor: %.1f ms\n", devRate, devCh, 1000.0 * minP / devRate);

    Ring ring;
    // ---- UDP receiver ----
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons((u_short) cfg.port); addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (sockaddr*) &addr, sizeof addr) != 0) { wprintf(L"No se pudo abrir el puerto UDP %d (¿está el bridge de Discord abierto? Solo uno a la vez)\n", cfg.port); return 1; }
    DWORD tmo = 100; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*) &tmo, sizeof tmo);
    std::atomic<uint64_t> packets{0}, lost{0};
    std::thread rx([&] {
        std::vector<uint8_t> buf(2048); std::vector<float> f, o; Resampler rs; rs.outRate = devRate;
        bool haveSeq = false; uint32_t last = 0;
        while (!g_stop)
        {
            int n = recv(s, (char*) buf.data(), (int) buf.size(), 0);
            if (n < 16) continue;
            uint32_t magic, seq, sr; uint16_t ch, fr;
            memcpy(&magic, &buf[0], 4); memcpy(&seq, &buf[4], 4); memcpy(&sr, &buf[8], 4); memcpy(&ch, &buf[12], 2); memcpy(&fr, &buf[14], 2);
            if (magic != 0x4353444c || ch != 2 || n < 16 + fr * 4) continue;
            if (haveSeq && seq != last + 1) lost++;
            haveSeq = true; last = seq; packets++;
            f.resize((size_t) fr * 2);
            const int16_t* p = (const int16_t*) &buf[16];
            for (size_t i = 0; i < f.size(); ++i) f[i] = p[i] / 32768.0f;
            rs.process(f.data(), fr, sr, o);
            ring.push(o.data(), o.size() / 2);
        }
    });

    // ---- render loop ----
    DWORD task = 0; HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    const uint64_t target = (uint64_t) (devRate * cfg.prebufferMs / 1000), maxF = (uint64_t) (devRate * cfg.maxMs / 1000);
    bool primed = false; uint64_t underruns = 0, dropped = 0, lastPrint = GetTickCount64();
    std::vector<float> tmp(4096 * 2);
    ac->Start();
    wprintf(L"Escuchando en udp://127.0.0.1:%d. Selecciona \"CABLE Output\" como micrófono en Discord. Ctrl+C para salir.\n", cfg.port);
    while (!g_stop)
    {
        if (WaitForSingleObject(ev, 100) != WAIT_OBJECT_0) continue;
        UINT32 pad = 0; ac->GetCurrentPadding(&pad);
        UINT32 frames = bufFrames - pad;
        if (!frames) continue;
        BYTE* out = nullptr;
        if (FAILED(rc->GetBuffer(frames, &out))) continue;
        float* o = (float*) out;
        uint64_t avail = ring.available();
        if (avail > maxF) { ring.skip(avail - target); dropped += avail - target; avail = target; }
        if (!primed && avail >= target) primed = true;
        UINT32 done = 0;
        while (primed && done < frames && ring.available())
        {
            UINT32 n = (UINT32) std::min<uint64_t>({ (uint64_t) frames - done, ring.available(), 4096 });
            ring.pop(tmp.data(), n);
            for (UINT32 i = 0; i < n; ++i)
            {
                float* fr = o + (size_t) (done + i) * devCh;
                for (int c = 0; c < devCh; ++c) fr[c] = 0.f;
                fr[0] = tmp[i * 2]; fr[1] = tmp[i * 2 + 1];
            }
            done += n;
        }
        if (done < frames) { memset(o + (size_t) done * devCh, 0, (size_t) (frames - done) * devCh * sizeof(float)); if (primed) { underruns++; primed = false; } }
        rc->ReleaseBuffer(frames, 0);
        if (GetTickCount64() - lastPrint > 5000)
        {
            lastPrint = GetTickCount64();
            if (lost || dropped || underruns) wprintf(L"paquetes=%llu perdidos=%llu descartados=%llufr underruns=%llu\n", (unsigned long long) packets.load(), (unsigned long long) lost.load(), (unsigned long long) dropped, (unsigned long long) underruns);
        }
    }
    ac->Stop(); rx.join(); closesocket(s); WSACleanup();
    if (mm) AvRevertMmThreadCharacteristics(mm);
    CoTaskMemFree(fmt); rel(rc); rel(ac); rel(dev); rel(col); rel(en);
    return 0;
}
