// Shared audio engine: UDP (from the Live Discord Send plugin) -> low-latency WASAPI render endpoint.
#pragma once
#include <winsock2.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <atomic>
#include <cstdarg>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Config { std::wstring device = L"CABLE Input"; int port = 9955; int prebufferMs = 10; int maxMs = 40; };

enum class Result { Stopped, NoDevice, InitFailed, PortBusy, DeviceLost };

struct Stats
{
    std::atomic<uint64_t> packets{0}, lost{0}, dropped{0}, underruns{0};
    std::atomic<bool> running{false};
    std::atomic<int> rate{0};
    std::mutex m; std::wstring device;
    void setDevice(const std::wstring& d) { std::lock_guard<std::mutex> l(m); device = d; }
    std::wstring getDevice() { std::lock_guard<std::mutex> l(m); return device; }
};

using Log = std::function<void(const std::wstring&)>;

namespace ldsc
{
template <class T> inline void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
inline std::wstring lower(std::wstring s) { for (auto& c : s) c = (wchar_t) towlower(c); return s; }
inline std::wstring fmt(const wchar_t* f, ...)
{
    wchar_t b[512]; va_list a; va_start(a, f); _vsnwprintf_s(b, 512, _TRUNCATE, f, a); va_end(a); return b;
}

inline std::wstring friendlyName(IMMDevice* d)
{
    IPropertyStore* ps = nullptr; PROPVARIANT v; PropVariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) out = v.pwszVal;
    PropVariantClear(&v); rel(ps);
    return out;
}

inline std::vector<std::wstring> listRenderDevices()
{
    std::vector<std::wstring> names;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr; IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
        SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)))
    {
        UINT n = 0; col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) { IMMDevice* d = nullptr; if (SUCCEEDED(col->Item(i, &d))) { names.push_back(friendlyName(d)); rel(d); } }
    }
    rel(col); rel(en);
    if (SUCCEEDED(co)) CoUninitialize();
    return names;
}

// Single-producer/single-consumer ring of stereo float frames.
struct Ring
{
    static constexpr uint32_t N = 1u << 16;
    std::vector<float> data = std::vector<float>((size_t) N * 2, 0.f);
    std::atomic<uint64_t> w{0}, r{0};
    uint64_t available() const { return w.load(std::memory_order_acquire) - r.load(std::memory_order_acquire); }
    void push(const float* lr, size_t frames)
    {
        uint64_t wi = w.load(std::memory_order_relaxed);
        if (wi + frames - r.load(std::memory_order_acquire) > N) return;
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

struct Defer { std::function<void()> f; ~Defer() { if (f) f(); } };
} // namespace ldsc

// Runs until `stop` is set or something fails. Safe to call again afterwards (auto-reconnect loop).
inline Result runEngine(const Config& cfg, std::atomic<bool>& stop, Stats& st, const Log& log)
{
    using namespace ldsc;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Defer coGuard{ [&] { if (SUCCEEDED(co)) CoUninitialize(); } };

    IMMDeviceEnumerator* en = nullptr; IMMDeviceCollection* col = nullptr; IMMDevice* dev = nullptr;
    IAudioClient3* ac = nullptr; IAudioRenderClient* rc = nullptr; WAVEFORMATEX* wf = nullptr; HANDLE ev = nullptr;
    SOCKET sock = INVALID_SOCKET; std::thread rx; std::atomic<bool> quit{false};
    bool wsaOk = false, started = false; HANDLE mm = nullptr;
    Defer cleanup{ [&] {
        quit = true;
        st.running = false;
        if (started && ac) ac->Stop();
        if (rx.joinable()) rx.join();
        if (sock != INVALID_SOCKET) closesocket(sock);
        if (wsaOk) WSACleanup();
        if (mm) AvRevertMmThreadCharacteristics(mm);
        if (wf) CoTaskMemFree(wf);
        rel(rc); rel(ac); rel(dev); rel(col); rel(en);
        if (ev) CloseHandle(ev);
    } };

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) { log(L"No se pudo abrir WASAPI"); return Result::InitFailed; }
    en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    UINT count = 0; if (col) col->GetCount(&count);
    std::wstring devName;
    for (UINT i = 0; i < count && !dev; ++i)
    {
        IMMDevice* d = nullptr; col->Item(i, &d);
        std::wstring name = friendlyName(d);
        if (lower(name).find(lower(cfg.device)) != std::wstring::npos) { dev = d; devName = name; } else rel(d);
    }
    if (!dev) { log(fmt(L"No encuentro el dispositivo \"%ls\" (instala VB-CABLE o cambia 'device' en la configuración)", cfg.device.c_str())); return Result::NoDevice; }
    st.setDevice(devName);

    if (FAILED(dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void**) &ac))) { log(L"Se requiere Windows 10 o superior (IAudioClient3)"); return Result::InitFailed; }
    ac->GetMixFormat(&wf);
    bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE*) wf)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    if (!(isFloat && wf->wBitsPerSample == 32) || wf->nChannels < 2) { log(L"Formato del dispositivo no soportado (se espera float32 estéreo)"); return Result::InitFailed; }
    const int devCh = wf->nChannels; const double devRate = wf->nSamplesPerSec;

    UINT32 defP = 0, fundP = 0, minP = 0, maxP = 0;
    HRESULT hr = ac->GetSharedModeEnginePeriod(wf, &defP, &fundP, &minP, &maxP);
    ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(hr)) hr = ac->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minP, wf, nullptr);
    if (FAILED(hr))
    {
        rel(ac);
        if (FAILED(dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void**) &ac))) return Result::InitFailed;
        hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000, 0, wf, nullptr);
        minP = (UINT32) (devRate / 100);
    }
    if (FAILED(hr)) { log(fmt(L"No se pudo inicializar el dispositivo (0x%08lx)", (unsigned long) hr)); return Result::InitFailed; }
    ac->SetEventHandle(ev);
    if (FAILED(ac->GetService(IID_PPV_ARGS(&rc)))) return Result::InitFailed;
    UINT32 bufFrames = 0; ac->GetBufferSize(&bufFrames);
    st.rate = (int) devRate;

    Ring ring;
    WSADATA wsa; wsaOk = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons((u_short) cfg.port); addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sock, (sockaddr*) &addr, sizeof addr) != 0) { log(fmt(L"Puerto UDP %d ocupado (¿bridge de Discord abierto?)", cfg.port)); return Result::PortBusy; }
    DWORD tmo = 100; setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*) &tmo, sizeof tmo);

    st.packets = 0; st.lost = 0; st.dropped = 0; st.underruns = 0;
    rx = std::thread([&] {
        std::vector<uint8_t> buf(2048); std::vector<float> f, o; Resampler rs; rs.outRate = devRate;
        bool haveSeq = false; uint32_t last = 0;
        while (!quit && !stop)
        {
            int n = recv(sock, (char*) buf.data(), (int) buf.size(), 0);
            if (n < 16) continue;
            uint32_t magic, seq, sr; uint16_t ch, fr;
            memcpy(&magic, &buf[0], 4); memcpy(&seq, &buf[4], 4); memcpy(&sr, &buf[8], 4); memcpy(&ch, &buf[12], 2); memcpy(&fr, &buf[14], 2);
            if (magic != 0x4353444c || ch != 2 || n < 16 + fr * 4) continue;
            if (haveSeq && seq != last + 1) st.lost++;
            haveSeq = true; last = seq; st.packets++;
            f.resize((size_t) fr * 2);
            const int16_t* p = (const int16_t*) &buf[16];
            for (size_t i = 0; i < f.size(); ++i) f[i] = p[i] / 32768.0f;
            rs.process(f.data(), fr, sr, o);
            ring.push(o.data(), o.size() / 2);
        }
    });

    DWORD task = 0; mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    const uint64_t target = (uint64_t) (devRate * cfg.prebufferMs / 1000), maxF = (uint64_t) (devRate * cfg.maxMs / 1000);
    bool primed = false; std::vector<float> tmp(4096 * 2);
    ac->Start(); started = true; st.running = true;
    log(fmt(L"Activo: %ls (%.0f Hz, periodo %.1f ms)", devName.c_str(), devRate, 1000.0 * minP / devRate));

    Result result = Result::Stopped;
    while (!stop)
    {
        UINT32 pad = 0; HRESULT h;
        if (WaitForSingleObject(ev, 100) != WAIT_OBJECT_0)
        {
            h = ac->GetCurrentPadding(&pad); // also detects an unplugged / disabled device
            if (h == AUDCLNT_E_DEVICE_INVALIDATED) { result = Result::DeviceLost; break; }
            continue;
        }
        h = ac->GetCurrentPadding(&pad);
        if (h == AUDCLNT_E_DEVICE_INVALIDATED) { result = Result::DeviceLost; break; }
        if (FAILED(h)) continue;
        UINT32 frames = bufFrames - pad;
        if (!frames) continue;
        BYTE* out = nullptr;
        h = rc->GetBuffer(frames, &out);
        if (h == AUDCLNT_E_DEVICE_INVALIDATED) { result = Result::DeviceLost; break; }
        if (FAILED(h)) continue;
        float* o = (float*) out;
        uint64_t avail = ring.available();
        if (avail > maxF) { ring.skip(avail - target); st.dropped += avail - target; avail = target; }
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
        if (done < frames) { memset(o + (size_t) done * devCh, 0, (size_t) (frames - done) * devCh * sizeof(float)); if (primed) { st.underruns++; primed = false; } }
        rc->ReleaseBuffer(frames, 0);
    }
    if (result == Result::DeviceLost) log(L"El dispositivo de audio desapareció; reintentando...");
    return result;
}
