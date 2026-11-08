/*
 * libaudio_primary_impl.z.so for HarmonyOS on the Redmi Note 10 Pro (sweet).
 *
 * The HarmonyOS audio HDI host loads /vendor/lib64/libaudio_primary_impl.z.so
 * through the OpenHarmony "VDI" interface (AudioManagerCreateIfInstance ->
 * IAudioManagerVdi / IAudioAdapterVdi / IAudioRenderVdi / IAudioCaptureVdi).
 * Huawei's library behind that interface drives the Kirin HiFi DSP; this one
 * drives the Qualcomm sm6150 sound card (ADSP + Bolero codec + two Awinic
 * aw882xx speaker amplifiers) with tinyalsa:
 *
 *   playback  PCM 0  (MultiMedia1, "deep-buffer-playback")  first stream
 *             PCM 13 (MultiMedia5, "low-latency-playback")  a concurrent one
 *   capture   PCM 0  (MultiMedia1, "audio-record")
 *
 * Mixer routes come from the stock sweet XMLs (mixer_paths_idp.xml with the
 * mixer_paths_overlay_static.xml overlay), so device paths ("speaker",
 * "headphones", "handset", "handset-mic"...) are the same as on MIUI.
 *
 * Without a sound card (ADSP not booted) streams still "play": frames are
 * consumed in real time and capture returns silence, so the audio server
 * never stalls.
 */
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

#include <tinyalsa/asoundlib.h>

#include "iaudio_manager_vdi.h"

#define EXPORT __attribute__((visibility("default")))

namespace {
constexpr int32_t OK = 0;
constexpr int32_t ERR = -1;
constexpr int32_t ERR_NOT_SUPPORT = -2;
constexpr int32_t ERR_INVALID = -3;

constexpr unsigned int CARD = 0;
constexpr unsigned int PCM_DEEP_BUFFER = 0;
constexpr unsigned int PCM_LOW_LATENCY = 13;
constexpr unsigned int PCM_RECORD = 0;
constexpr unsigned int PERIOD_FRAMES = 960; /* 20 ms at 48 kHz */
constexpr unsigned int PERIOD_COUNT = 4;

const char *MIXER_XML[] = {
    "/data/a70fw/audio/mixer_paths_idp.xml",
};

/* ------------------------------------------------------------------ log */
using LogFn = int (*)(int, int, unsigned int, const char *, const char *, ...);
void Log(int level, const char *fmt, ...)
{
    static LogFn fn = reinterpret_cast<LogFn>(-1);
    if (fn == reinterpret_cast<LogFn>(-1)) {
        void *h = dlopen("libhilog.so", RTLD_NOW);
        fn = h ? reinterpret_cast<LogFn>(dlsym(h, "HiLogPrint")) : nullptr;
    }
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (fn) {
        fn(3 /* LOG_CORE */, level, 0xD002512, "SWEET_AUDIO", "%{public}s", buf);
    }
}
#define LOGI(...) Log(4, __VA_ARGS__)
#define LOGE(...) Log(6, __VA_ARGS__)

int64_t NowNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

/* ---------------------------------------------------------- mixer paths */
struct CtlSetting {
    std::string name;
    int id; /* -1: every value */
    std::string value;
};
struct PathItem {
    bool isPath;
    CtlSetting ctl;
    std::string path;
};

class Route {
public:
    std::mutex lock;

    bool Ready()
    {
        if (mixer_) {
            return true;
        }
        mixer_ = mixer_open(CARD);
        if (!mixer_) {
            return false;
        }
        LOGI("mixer open: %s, %u controls", mixer_get_name(mixer_), mixer_get_num_ctls(mixer_));
        if (!loaded_) {
            for (const char *f : MIXER_XML) {
                Load(f, f != MIXER_XML[0]);
            }
            loaded_ = true;
        }
        for (auto &c : defaults_) {
            Set(c);
        }
        LOGI("mixer defaults applied: %zu controls, %zu paths", defaults_.size(), paths_.size());
        return true;
    }

    bool HasPath(const std::string &name) const
    {
        return paths_.count(name) != 0;
    }

    void Apply(const std::string &name)
    {
        if (!mixer_ || name.empty() || !HasPath(name)) {
            return;
        }
        if (refs_[name]++ > 0) {
            return;
        }
        std::vector<CtlSetting> flat;
        Flatten(name, flat, 0);
        for (auto &c : flat) {
            Set(c);
        }
        LOGI("route: apply \"%s\" (%zu controls)", name.c_str(), flat.size());
    }

    void Reset(const std::string &name)
    {
        if (!mixer_ || name.empty() || !HasPath(name)) {
            return;
        }
        auto it = refs_.find(name);
        if (it == refs_.end() || it->second == 0 || --it->second > 0) {
            return;
        }
        std::vector<CtlSetting> flat;
        Flatten(name, flat, 0);
        for (auto &c : flat) {
            auto d = defaultIndex_.find(c.name);
            if (d != defaultIndex_.end()) {
                for (auto &dc : d->second) {
                    Set(dc);
                }
            } else {
                auto r = initial_.find(c.name);
                if (r != initial_.end()) {
                    Set(r->second);
                }
            }
        }
        LOGI("route: reset \"%s\"", name.c_str());
    }

    void SetCtl(const char *name, const char *value)
    {
        if (mixer_) {
            Set(CtlSetting{name, -1, value});
        }
    }

    /* controls that take several integers (voice session gain / mute) */
    int SetCtlInts(const char *name, const std::vector<long> &vals)
    {
        if (!mixer_) {
            return -ENODEV;
        }
        struct mixer_ctl *ctl = mixer_get_ctl_by_name(mixer_, name);
        if (!ctl) {
            return -ENOENT;
        }
        std::vector<long> v(vals);
        v.resize(mixer_ctl_get_num_values(ctl), 0);
        return mixer_ctl_set_array(ctl, v.data(), v.size());
    }

private:
    struct mixer *mixer_ = nullptr;
    bool loaded_ = false;
    std::vector<CtlSetting> defaults_;
    std::map<std::string, std::vector<CtlSetting>> defaultIndex_;
    std::map<std::string, CtlSetting> initial_;
    std::map<std::string, std::vector<PathItem>> paths_;
    std::map<std::string, int> refs_;

    static std::string Attr(const std::string &tag, const char *key)
    {
        std::string k = std::string(" ") + key + "=\"";
        size_t p = tag.find(k);
        if (p == std::string::npos) {
            k = std::string("\t") + key + "=\"";
            p = tag.find(k);
            if (p == std::string::npos) {
                return std::string();
            }
        }
        p += k.size();
        size_t e = tag.find('"', p);
        return e == std::string::npos ? std::string() : tag.substr(p, e - p);
    }

    void Load(const char *file, bool overlay)
    {
        FILE *f = fopen(file, "re");
        if (!f) {
            LOGE("mixer paths %s: %s", file, strerror(errno));
            return;
        }
        std::string s;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            s.append(buf, n);
        }
        fclose(f);

        std::vector<std::string> stack; /* open <path> names */
        std::map<std::string, bool> replaced;
        size_t p = 0;
        while ((p = s.find('<', p)) != std::string::npos) {
            if (s.compare(p, 4, "<!--") == 0) {
                size_t e = s.find("-->", p);
                p = (e == std::string::npos) ? s.size() : e + 3;
                continue;
            }
            size_t e = s.find('>', p);
            if (e == std::string::npos) {
                break;
            }
            std::string tag = s.substr(p, e - p + 1);
            p = e + 1;
            bool selfClose = tag.size() > 2 && tag[tag.size() - 2] == '/';
            if (tag.compare(0, 5, "<ctl ") == 0 || tag.compare(0, 5, "<ctl\t") == 0) {
                CtlSetting c;
                c.name = Attr(tag, "name");
                c.value = Attr(tag, "value");
                std::string id = Attr(tag, "id");
                c.id = id.empty() ? -1 : atoi(id.c_str());
                if (c.name.empty()) {
                    continue;
                }
                if (stack.empty()) {
                    AddDefault(c);
                } else {
                    paths_[stack.back()].push_back(PathItem{false, c, std::string()});
                }
            } else if (tag.compare(0, 6, "<path ") == 0) {
                std::string name = Attr(tag, "name");
                if (selfClose) {
                    if (!stack.empty()) {
                        paths_[stack.back()].push_back(PathItem{true, CtlSetting(), name});
                    }
                } else {
                    if (overlay && stack.empty() && !replaced[name]) {
                        paths_[name].clear(); /* first-level overlay paths replace */
                        replaced[name] = true;
                    }
                    paths_[name];
                    stack.push_back(name);
                }
            } else if (tag.compare(0, 7, "</path>") == 0) {
                if (!stack.empty()) {
                    stack.pop_back();
                }
            }
        }
        LOGI("mixer paths %s loaded", file);
    }

    void AddDefault(const CtlSetting &c)
    {
        for (auto &d : defaults_) {
            if (d.name == c.name && d.id == c.id) {
                d.value = c.value;
                defaultIndex_[c.name].clear();
                for (auto &x : defaults_) {
                    if (x.name == c.name) {
                        defaultIndex_[c.name].push_back(x);
                    }
                }
                return;
            }
        }
        defaults_.push_back(c);
        defaultIndex_[c.name].push_back(c);
    }

    void Flatten(const std::string &name, std::vector<CtlSetting> &out, int depth)
    {
        if (depth > 10) {
            return;
        }
        auto it = paths_.find(name);
        if (it == paths_.end()) {
            return;
        }
        for (auto &item : it->second) {
            if (item.isPath) {
                Flatten(item.path, out, depth + 1);
            } else {
                out.push_back(item.ctl);
            }
        }
    }

    void Remember(struct mixer_ctl *ctl, const std::string &name)
    {
        if (initial_.count(name)) {
            return;
        }
        CtlSetting c{name, -1, std::string()};
        if (mixer_ctl_get_type(ctl) == MIXER_CTL_TYPE_ENUM) {
            int v = mixer_ctl_get_value(ctl, 0);
            const char *str = mixer_ctl_get_enum_string(ctl, v);
            c.value = str ? str : "";
        } else if (mixer_ctl_get_type(ctl) == MIXER_CTL_TYPE_INT || mixer_ctl_get_type(ctl) == MIXER_CTL_TYPE_BOOL) {
            c.value = std::to_string(mixer_ctl_get_value(ctl, 0));
        } else {
            return;
        }
        initial_[name] = c;
    }

    void Set(const CtlSetting &c)
    {
        /* Xiaomi ultrasound proximity: its AFE commands crash the ADSP
         * (AfeS fatal) without the matching calibration, leave it alone */
        if (c.name.compare(0, 13, "Mi_Ultrasound") == 0) {
            return;
        }
        struct mixer_ctl *ctl = mixer_get_ctl_by_name(mixer_, c.name.c_str());
        if (!ctl) {
            return;
        }
        Remember(ctl, c.name);
        enum mixer_ctl_type type = mixer_ctl_get_type(ctl);
        unsigned int num = mixer_ctl_get_num_values(ctl);
        char *end = nullptr;
        long v = strtol(c.value.c_str(), &end, 0);
        bool numeric = !c.value.empty() && end && *end == 0;
        int r = 0;
        if (type == MIXER_CTL_TYPE_ENUM) {
            if (numeric) {
                r = mixer_ctl_set_value(ctl, 0, int(v));
            } else {
                r = mixer_ctl_set_enum_by_string(ctl, c.value.c_str());
            }
        } else if (numeric) {
            if (c.id >= 0) {
                r = (unsigned(c.id) < num) ? mixer_ctl_set_value(ctl, c.id, int(v)) : -EINVAL;
            } else {
                for (unsigned int i = 0; i < num && r == 0; i++) {
                    r = mixer_ctl_set_value(ctl, i, int(v));
                }
            }
        } else {
            return;
        }
        if (r < 0) {
            LOGE("ctl \"%s\" = \"%s\": %d", c.name.c_str(), c.value.c_str(), r);
        }
    }
};

Route g_route;

/* ------------------------------------------------------------- devices */
std::string OutDevicePath(uint32_t pin)
{
    switch (pin) {
        case PIN_VDI_OUT_HEADSET:
        case PIN_VDI_OUT_HEADPHONE:
        case PIN_VDI_OUT_LINEOUT:
            return "headphones";
        case PIN_VDI_OUT_EARPIECE:
            return "handset";
        default:
            return "speaker";
    }
}

std::string InDevicePath(uint32_t pin)
{
    if (pin == PIN_VDI_IN_HS_MIC && g_route.HasPath("headset-mic")) {
        return "headset-mic";
    }
    return "handset-mic";
}

uint32_t g_outPin = PIN_VDI_OUT_SPEAKER;
uint32_t g_inPin = PIN_VDI_IN_MIC;

/* sample format helpers: the front ends run S16_LE, the audio server may hand
 * us 16/24/32 bit or float samples */
uint32_t BytesPerSample(enum AudioFormatVdi f)
{
    switch (f) {
        case AUDIO_VDI_FORMAT_TYPE_PCM_8_BIT:
            return 1;
        case AUDIO_VDI_FORMAT_TYPE_PCM_24_BIT:
            return 3;
        case AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT:
        case AUDIO_VDI_FORMAT_TYPE_PCM_FLOAT:
            return 4;
        default:
            return 2;
    }
}

inline int16_t Clamp16(float x)
{
    if (x > 32767.f) {
        return 32767;
    }
    if (x < -32768.f) {
        return -32768;
    }
    return int16_t(lrintf(x));
}

void ToS16(enum AudioFormatVdi f, const int8_t *in, size_t samples, int16_t *out, float gain)
{
    for (size_t i = 0; i < samples; i++) {
        float s;
        switch (f) {
            case AUDIO_VDI_FORMAT_TYPE_PCM_8_BIT:
                s = float(int(uint8_t(in[i])) - 128) * 256.f;
                break;
            case AUDIO_VDI_FORMAT_TYPE_PCM_24_BIT: {
                const uint8_t *b = reinterpret_cast<const uint8_t *>(in) + i * 3;
                int32_t v = int32_t(uint32_t(b[0]) << 8 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 24);
                s = float(v >> 16);
                break;
            }
            case AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT: {
                int32_t v;
                memcpy(&v, in + i * 4, 4);
                s = float(v >> 16);
                break;
            }
            case AUDIO_VDI_FORMAT_TYPE_PCM_FLOAT: {
                float v;
                memcpy(&v, in + i * 4, 4);
                s = v * 32767.f;
                break;
            }
            default: {
                int16_t v;
                memcpy(&v, in + i * 2, 2);
                s = float(v);
                break;
            }
        }
        out[i] = Clamp16(s * gain);
    }
}

void FromS16(enum AudioFormatVdi f, const int16_t *in, size_t samples, int8_t *out)
{
    for (size_t i = 0; i < samples; i++) {
        int16_t v = in[i];
        switch (f) {
            case AUDIO_VDI_FORMAT_TYPE_PCM_8_BIT:
                out[i] = int8_t((v >> 8) + 128);
                break;
            case AUDIO_VDI_FORMAT_TYPE_PCM_24_BIT: {
                uint8_t *b = reinterpret_cast<uint8_t *>(out) + i * 3;
                int32_t w = int32_t(v) << 8;
                b[0] = uint8_t(w);
                b[1] = uint8_t(w >> 8);
                b[2] = uint8_t(w >> 16);
                break;
            }
            case AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT: {
                int32_t w = int32_t(v) << 16;
                memcpy(out + i * 4, &w, 4);
                break;
            }
            case AUDIO_VDI_FORMAT_TYPE_PCM_FLOAT: {
                float w = float(v) / 32768.f;
                memcpy(out + i * 4, &w, 4);
                break;
            }
            default:
                memcpy(out + i * 2, &v, 2);
                break;
        }
    }
}

/* ------------------------------------------------------------- streams */
bool g_pcmBusy[2] = {false, false}; /* deep buffer, low latency */

struct Render {
    struct IAudioRenderVdi vtbl; /* must stay first */
    std::mutex lock;
    struct AudioSampleAttributesVdi attrs;
    uint32_t pin;
    struct pcm *pcm = nullptr;
    int fe = -1; /* index into g_pcmBusy */
    std::string usecasePath, devicePath;
    uint64_t written = 0;
    int64_t fakeClock = 0; /* when no PCM: when the consumed frames end */
    bool mute = false;
    float volume = 1.f;
    float gain = 0.f;
    float speed = 1.f;
    enum AudioChannelModeVdi channelMode = AUDIO_VDI_CHANNEL_NORMAL;
    bool openFailedLogged = false;
    int64_t retryAt = 0;
    std::vector<int16_t> conv;
};

struct Capture {
    struct IAudioCaptureVdi vtbl; /* must stay first */
    std::mutex lock;
    struct AudioSampleAttributesVdi attrs;
    uint32_t pin;
    struct pcm *pcm = nullptr;
    std::string usecasePath, devicePath;
    uint64_t read = 0;
    int64_t fakeClock = 0;
    bool mute = false;
    float volume = 1.f;
    float gain = 0.f;
    bool openFailedLogged = false;
    int64_t retryAt = 0;
    std::vector<int16_t> conv;
};

std::mutex g_streamsLock;
std::vector<Render *> g_renders;
std::vector<Capture *> g_captures;

uint32_t FrameBytes(const struct AudioSampleAttributesVdi &a)
{
    return BytesPerSample(a.format) * (a.channelCount ? a.channelCount : 2);
}

uint32_t RateOf(const struct AudioSampleAttributesVdi &a)
{
    return a.sampleRate ? a.sampleRate : 48000;
}

void BackendFor(const std::string &device, uint32_t rate)
{
    const char *r = rate == 44100 ? "KHZ_44P1" : (rate <= 48000 ? "KHZ_48" : "KHZ_96");
    if (device == "headphones") {
        g_route.SetCtl("RX_CDC_DMA_RX_0 SampleRate", r);
        g_route.SetCtl("RX_CDC_DMA_RX_0 Format", "S16_LE");
    } else {
        /* WSA881x amplifier on the WSA codec DMA port, 24 bit like the stock HAL */
        g_route.SetCtl("WSA_CDC_DMA_RX_0 SampleRate", r);
        g_route.SetCtl("WSA_CDC_DMA_RX_0 Format", "S24_LE");
    }
}

/* caller holds r->lock */
void RenderClose(Render *r)
{
    if (!r->pcm) {
        return;
    }
    pcm_close(r->pcm);
    r->pcm = nullptr;
    std::lock_guard<std::mutex> g(g_route.lock);
    g_route.Reset(r->usecasePath);
    g_route.Reset(r->devicePath);
    if (r->fe >= 0) {
        g_pcmBusy[r->fe] = false;
    }
    r->fe = -1;
    LOGI("render %p closed (%llu frames)", r, (unsigned long long)r->written);
}

/* caller holds r->lock */
bool RenderOpen(Render *r)
{
    if (r->pcm) {
        return true;
    }
    if (NowNs() < r->retryAt) {
        return false;
    }
    r->retryAt = NowNs() + 3000000000LL; /* a failed open is retried every 3 s */
    std::lock_guard<std::mutex> g(g_route.lock);
    if (!g_route.Ready()) {
        if (!r->openFailedLogged) {
            LOGE("render: no sound card yet (ADSP not up?), playing to nowhere");
            r->openFailedLogged = true;
        }
        return false;
    }
    int fe = !g_pcmBusy[0] ? 0 : (!g_pcmBusy[1] ? 1 : -1);
    if (fe < 0) {
        return false;
    }
    std::string base = fe == 0 ? "deep-buffer-playback" : "low-latency-playback";
    r->devicePath = OutDevicePath(r->pin);
    std::string withDev = base + " " + r->devicePath;
    r->usecasePath = g_route.HasPath(withDev) ? withDev : base;

    struct pcm_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.channels = r->attrs.channelCount ? r->attrs.channelCount : 2;
    cfg.rate = RateOf(r->attrs);
    cfg.period_size = PERIOD_FRAMES * cfg.rate / 48000;
    cfg.period_count = PERIOD_COUNT;
    cfg.format = PCM_FORMAT_S16_LE;
    /* like the Qualcomm Android HAL (deep buffer): never stop the stream on
     * an underrun. With the default stop threshold every late write turns
     * into an xrun, the driver stops the stream from the ADSP interrupt
     * (EOS) and restarts it: audible gaps, and on sm6150 a path that can
     * lock up the APR/glink interrupt. The ADSP plays silence by itself
     * when no buffer is queued. */
    cfg.start_threshold = cfg.period_size;
    cfg.stop_threshold = INT_MAX;
    cfg.avail_min = cfg.period_size;

    BackendFor(r->devicePath, cfg.rate);
    g_route.Apply(r->devicePath);
    g_route.Apply(r->usecasePath);
    unsigned int dev = fe == 0 ? PCM_DEEP_BUFFER : PCM_LOW_LATENCY;
    struct pcm *p = pcm_open(CARD, dev, PCM_OUT | PCM_MONOTONIC, &cfg);
    if (!p || !pcm_is_ready(p)) {
        if (!r->openFailedLogged) {
            LOGE("render: pcm %u open failed: %s", dev, p ? pcm_get_error(p) : "no memory");
            r->openFailedLogged = true;
        }
        if (p) {
            pcm_close(p);
        }
        g_route.Reset(r->usecasePath);
        g_route.Reset(r->devicePath);
        return false;
    }
    r->pcm = p;
    r->fe = fe;
    g_pcmBusy[fe] = true;
    r->openFailedLogged = false;
    r->retryAt = 0;
    LOGI("render %p: pcm %u open %u Hz %u ch, %s + %s", r, dev, cfg.rate, cfg.channels, r->usecasePath.c_str(),
        r->devicePath.c_str());
    return true;
}

void ConsumeRealtime(int64_t &clock, uint64_t frames, uint32_t rate)
{
    int64_t now = NowNs();
    if (clock < now - 100000000LL) {
        clock = now;
    }
    clock += int64_t(frames) * 1000000000LL / rate;
    int64_t wait = clock - now;
    if (wait > 0) {
        usleep(useconds_t(wait / 1000));
    }
}

Render *R(struct IAudioRenderVdi *self)
{
    return reinterpret_cast<Render *>(self);
}
Capture *C(struct IAudioCaptureVdi *self)
{
    return reinterpret_cast<Capture *>(self);
}

/* ---------------------------------------------------------- render ops */
int32_t RGetLatency(struct IAudioRenderVdi *self, uint32_t *ms)
{
    if (!ms) {
        return ERR_INVALID;
    }
    *ms = PERIOD_FRAMES * PERIOD_COUNT * 1000 / 48000;
    return OK;
}

int32_t RRenderFrame(struct IAudioRenderVdi *self, const int8_t *frame, uint32_t frameLen, uint64_t *replyBytes)
{
    Render *r = R(self);
    if (!frame || !replyBytes) {
        return ERR_INVALID;
    }
    std::lock_guard<std::mutex> g(r->lock);
    uint32_t fb = FrameBytes(r->attrs);
    uint64_t frames = frameLen / fb;
    size_t samples = frames * (r->attrs.channelCount ? r->attrs.channelCount : 2);
    *replyBytes = frameLen;
    if (!RenderOpen(r)) {
        ConsumeRealtime(r->fakeClock, frames, RateOf(r->attrs));
        r->written += frames;
        return OK;
    }
    r->conv.resize(samples);
    float gain = r->mute ? 0.f : r->volume;
    ToS16(r->attrs.format, frame, samples, r->conv.data(), gain);
    int ret = pcm_writei(r->pcm, r->conv.data(), frames);
    if (ret < 0) {
        LOGE("render: write failed: %s", pcm_get_error(r->pcm));
        RenderClose(r);
        ConsumeRealtime(r->fakeClock, frames, RateOf(r->attrs));
    }
    r->written += frames;
    return OK;
}

int32_t RGetRenderPosition(struct IAudioRenderVdi *self, uint64_t *frames, struct AudioTimeStampVdi *time)
{
    Render *r = R(self);
    if (!frames || !time) {
        return ERR_INVALID;
    }
    std::lock_guard<std::mutex> g(r->lock);
    uint64_t pos = r->written;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (r->pcm) {
        unsigned int avail = 0;
        struct timespec hts;
        if (pcm_get_htimestamp(r->pcm, &avail, &hts) == 0) {
            uint64_t buffered = pcm_get_buffer_size(r->pcm) > avail ? pcm_get_buffer_size(r->pcm) - avail : 0;
            pos = pos > buffered ? pos - buffered : 0;
            ts = hts;
        }
    }
    *frames = pos;
    time->tvSec = ts.tv_sec;
    time->tvNSec = ts.tv_nsec;
    return OK;
}

int32_t RSetRenderSpeed(struct IAudioRenderVdi *self, float speed)
{
    R(self)->speed = speed;
    return OK;
}
int32_t RGetRenderSpeed(struct IAudioRenderVdi *self, float *speed)
{
    if (!speed) {
        return ERR_INVALID;
    }
    *speed = R(self)->speed;
    return OK;
}
int32_t RSetChannelMode(struct IAudioRenderVdi *self, enum AudioChannelModeVdi mode)
{
    R(self)->channelMode = mode;
    return OK;
}
int32_t RGetChannelMode(struct IAudioRenderVdi *self, enum AudioChannelModeVdi *mode)
{
    if (!mode) {
        return ERR_INVALID;
    }
    *mode = R(self)->channelMode;
    return OK;
}
int32_t RRegCallback(struct IAudioRenderVdi *, RenderCallbackVdi, void *)
{
    return OK;
}
int32_t RDrainBuffer(struct IAudioRenderVdi *, enum AudioDrainNotifyTypeVdi *)
{
    return ERR_NOT_SUPPORT;
}
int32_t RIsSupportsDrain(struct IAudioRenderVdi *, bool *support)
{
    if (support) {
        *support = false;
    }
    return OK;
}
int32_t RCheckSceneCapability(struct IAudioRenderVdi *, const struct AudioSceneDescriptorVdi *, bool *supported)
{
    if (supported) {
        *supported = true;
    }
    return OK;
}

void RenderReroute(Render *r, uint32_t pin)
{
    std::lock_guard<std::mutex> g(r->lock);
    if (r->pin == pin) {
        return;
    }
    r->pin = pin;
    if (r->pcm) {
        RenderClose(r); /* reopened on the next frame with the new paths */
    }
}

/* --------------------------------------------------------- voice calls
 * Circuit switched call audio runs entirely in the ADSP/modem: the HAL only
 * routes the VoiceMMode1 front end to the codec ports (stock mixer paths
 * "voicemmode1-call <device>") and starts its hostless PCMs, like the
 * Qualcomm Android HAL does for USECASE_VOICEMMODE1_CALL. */
constexpr long VOICEMMODE1_VSID = 0x11C05000;

struct VoiceCall {
    bool active = false;
    struct pcm *rx = nullptr;
    struct pcm *tx = nullptr;
    std::string usecase, rxDev, txDev;
    uint32_t pin = PIN_VDI_OUT_EARPIECE;
    float volume = 1.f;
    bool micMute = false;
};
VoiceCall g_voice;

int VoicePcmDevice()
{
    /* "00-02: VoiceMMode1 (*) :  : playback 1 : capture 1" */
    FILE *f = fopen("/proc/asound/pcm", "re");
    if (!f) {
        return -1;
    }
    char line[256];
    int dev = -1;
    while (fgets(line, sizeof(line), f)) {
        int card = -1, d = -1;
        if (strstr(line, "VoiceMMode1") && sscanf(line, "%d-%d", &card, &d) == 2 && card == int(CARD)) {
            dev = d;
            break;
        }
    }
    fclose(f);
    return dev;
}

void VoiceApplyGain()
{
    long vol = lrintf(g_voice.volume * 5.f);
    vol = vol < 0 ? 0 : (vol > 5 ? 5 : vol);
    g_route.SetCtlInts("Voice Rx Gain", {vol, VOICEMMODE1_VSID, 20});
    g_route.SetCtlInts("Voice Tx Mute", {g_voice.micMute ? 1 : 0, VOICEMMODE1_VSID, 20});
}

void VoiceRoutes(uint32_t pin, std::string &usecase, std::string &rx, std::string &tx)
{
    std::string dev = OutDevicePath(pin);
    usecase = "voicemmode1-call " + dev;
    if (!g_route.HasPath(usecase)) {
        usecase = "voicemmode1-call";
    }
    rx = g_route.HasPath("voice-" + dev) ? "voice-" + dev : dev;
    if (dev == "headphones") {
        tx = g_route.HasPath("voice-headset-mic") ? "voice-headset-mic" : "headset-mic";
    } else if (dev == "speaker") {
        tx = g_route.HasPath("voice-speaker-mic") ? "voice-speaker-mic" : "speaker-mic";
    } else {
        tx = "handset-mic";
    }
}

void VoiceStopLocked()
{
    if (!g_voice.active) {
        return;
    }
    if (g_voice.rx) {
        pcm_stop(g_voice.rx);
        pcm_close(g_voice.rx);
    }
    if (g_voice.tx) {
        pcm_stop(g_voice.tx);
        pcm_close(g_voice.tx);
    }
    g_voice.rx = g_voice.tx = nullptr;
    g_route.Reset(g_voice.usecase);
    g_route.Reset(g_voice.txDev);
    g_route.Reset(g_voice.rxDev);
    g_voice.active = false;
    LOGI("voice: call audio stopped");
}

void VoiceStartLocked(uint32_t pin)
{
    if (!g_route.Ready()) {
        LOGE("voice: no sound card");
        return;
    }
    if (g_voice.active && g_voice.pin == pin) {
        return;
    }
    VoiceStopLocked();
    g_voice.pin = pin;
    VoiceRoutes(pin, g_voice.usecase, g_voice.rxDev, g_voice.txDev);
    BackendFor(g_voice.rxDev.find("headphones") != std::string::npos ? "headphones" : g_voice.rxDev, 48000);
    g_route.Apply(g_voice.rxDev);
    g_route.Apply(g_voice.txDev);
    g_route.Apply(g_voice.usecase);
    int dev = VoicePcmDevice();
    struct pcm_config cfg = {};
    cfg.channels = 1;
    cfg.rate = 8000;
    cfg.period_size = 160;
    cfg.period_count = 2;
    cfg.format = PCM_FORMAT_S16_LE;
    cfg.start_threshold = 0;
    cfg.stop_threshold = INT_MAX;
    if (dev >= 0) {
        g_voice.rx = pcm_open(CARD, unsigned(dev), PCM_OUT, &cfg);
        g_voice.tx = pcm_open(CARD, unsigned(dev), PCM_IN, &cfg);
        if (g_voice.rx && !pcm_is_ready(g_voice.rx)) {
            LOGE("voice: rx pcm %d: %s", dev, pcm_get_error(g_voice.rx));
            pcm_close(g_voice.rx);
            g_voice.rx = nullptr;
        }
        if (g_voice.tx && !pcm_is_ready(g_voice.tx)) {
            LOGE("voice: tx pcm %d: %s", dev, pcm_get_error(g_voice.tx));
            pcm_close(g_voice.tx);
            g_voice.tx = nullptr;
        }
        if (g_voice.rx) pcm_start(g_voice.rx);
        if (g_voice.tx) pcm_start(g_voice.tx);
    } else {
        LOGE("voice: no VoiceMMode1 PCM in /proc/asound/pcm");
    }
    g_voice.active = true;
    VoiceApplyGain();
    LOGI("voice: call audio on pcm %d: %s + %s + %s (rx %s, tx %s)", dev, g_voice.usecase.c_str(),
        g_voice.rxDev.c_str(), g_voice.txDev.c_str(), g_voice.rx ? "ok" : "FAILED", g_voice.tx ? "ok" : "FAILED");
}

void VoiceScene(bool inCall, uint32_t pin)
{
    std::lock_guard<std::mutex> g(g_route.lock);
    if (inCall) {
        VoiceStartLocked(pin ? pin : g_voice.pin);
    } else {
        VoiceStopLocked();
    }
}

int32_t RSelectScene(struct IAudioRenderVdi *self, const struct AudioSceneDescriptorVdi *scene)
{
    if (!scene) {
        return ERR_INVALID;
    }
    uint32_t pin = (scene->desc.pins != PIN_VDI_NONE && !(scene->desc.pins & (1u << 27))) ? scene->desc.pins : 0;
    if (pin) {
        RenderReroute(R(self), pin);
    }
    LOGI("render: scene %u pins 0x%x", unsigned(scene->scene.id), unsigned(scene->desc.pins));
    VoiceScene(scene->scene.id == AUDIO_VDI_IN_CALL, pin);
    return OK;
}
int32_t RSetMute(struct IAudioRenderVdi *self, bool mute)
{
    R(self)->mute = mute;
    return OK;
}
int32_t RGetMute(struct IAudioRenderVdi *self, bool *mute)
{
    if (!mute) {
        return ERR_INVALID;
    }
    *mute = R(self)->mute;
    return OK;
}
int32_t RSetVolume(struct IAudioRenderVdi *self, float volume)
{
    if (volume < 0.f || volume > 1.f) {
        return ERR_INVALID;
    }
    R(self)->volume = volume;
    return OK;
}
int32_t RGetVolume(struct IAudioRenderVdi *self, float *volume)
{
    if (!volume) {
        return ERR_INVALID;
    }
    *volume = R(self)->volume;
    return OK;
}
int32_t RGetGainThreshold(struct IAudioRenderVdi *, float *min, float *max)
{
    if (!min || !max) {
        return ERR_INVALID;
    }
    *min = 0.f;
    *max = 15.f;
    return OK;
}
int32_t RGetGain(struct IAudioRenderVdi *self, float *gain)
{
    if (!gain) {
        return ERR_INVALID;
    }
    *gain = R(self)->gain;
    return OK;
}
int32_t RSetGain(struct IAudioRenderVdi *self, float gain)
{
    R(self)->gain = gain;
    return OK;
}
int32_t RGetFrameSize(struct IAudioRenderVdi *self, uint64_t *size)
{
    if (!size) {
        return ERR_INVALID;
    }
    *size = FrameBytes(R(self)->attrs);
    return OK;
}
int32_t RGetFrameCount(struct IAudioRenderVdi *self, uint64_t *count)
{
    if (!count) {
        return ERR_INVALID;
    }
    *count = R(self)->written;
    return OK;
}
int32_t RSetSampleAttributes(struct IAudioRenderVdi *self, const struct AudioSampleAttributesVdi *attrs)
{
    if (!attrs) {
        return ERR_INVALID;
    }
    Render *r = R(self);
    std::lock_guard<std::mutex> g(r->lock);
    RenderClose(r);
    r->attrs = *attrs;
    return OK;
}
int32_t RGetSampleAttributes(struct IAudioRenderVdi *self, struct AudioSampleAttributesVdi *attrs)
{
    if (!attrs) {
        return ERR_INVALID;
    }
    *attrs = R(self)->attrs;
    return OK;
}
int32_t RGetCurrentChannelId(struct IAudioRenderVdi *self, uint32_t *channelId)
{
    if (!channelId) {
        return ERR_INVALID;
    }
    *channelId = R(self)->fe < 0 ? 0 : uint32_t(R(self)->fe);
    return OK;
}
int32_t RSetExtraParams(struct IAudioRenderVdi *, const char *)
{
    return OK;
}
int32_t RGetExtraParams(struct IAudioRenderVdi *, char *keyValueList, uint32_t len)
{
    if (keyValueList && len) {
        keyValueList[0] = 0;
    }
    return OK;
}
int32_t RReqMmapBuffer(struct IAudioRenderVdi *, int32_t, struct AudioMmapBufferDescriptorVdi *)
{
    return ERR_NOT_SUPPORT;
}
int32_t RGetMmapPosition(struct IAudioRenderVdi *, uint64_t *, struct AudioTimeStampVdi *)
{
    return ERR_NOT_SUPPORT;
}
int32_t RAddAudioEffect(struct IAudioRenderVdi *, uint64_t)
{
    return OK;
}
int32_t RRemoveAudioEffect(struct IAudioRenderVdi *, uint64_t)
{
    return OK;
}
int32_t RGetFrameBufferSize(struct IAudioRenderVdi *self, uint64_t *bufferSize)
{
    if (!bufferSize) {
        return ERR_INVALID;
    }
    *bufferSize = uint64_t(PERIOD_FRAMES) * PERIOD_COUNT * FrameBytes(R(self)->attrs);
    return OK;
}
int32_t RStart(struct IAudioRenderVdi *self)
{
    Render *r = R(self);
    std::lock_guard<std::mutex> g(r->lock);
    r->fakeClock = 0;
    return OK; /* the PCM opens on the first frame */
}
int32_t RStop(struct IAudioRenderVdi *self)
{
    Render *r = R(self);
    std::lock_guard<std::mutex> g(r->lock);
    RenderClose(r);
    return OK;
}
int32_t RPause(struct IAudioRenderVdi *self)
{
    return RStop(self);
}
int32_t RResume(struct IAudioRenderVdi *self)
{
    return RStart(self);
}
int32_t RFlush(struct IAudioRenderVdi *)
{
    return OK;
}
int32_t RTurnStandbyMode(struct IAudioRenderVdi *self)
{
    return RStop(self);
}
int32_t RAudioDevDump(struct IAudioRenderVdi *self, int32_t, int32_t fd)
{
    Render *r = R(self);
    if (fd >= 0) {
        dprintf(fd, "sweet render %p: pcm %s fe %d %u Hz %u ch fmt %d written %llu path %s + %s\n", r,
            r->pcm ? "open" : "closed", r->fe, RateOf(r->attrs), r->attrs.channelCount, int(r->attrs.format),
            (unsigned long long)r->written, r->usecasePath.c_str(), r->devicePath.c_str());
    }
    return OK;
}
int32_t RIsSupportsPauseAndResume(struct IAudioRenderVdi *, bool *supportPause, bool *supportResume)
{
    if (supportPause) {
        *supportPause = true;
    }
    if (supportResume) {
        *supportResume = true;
    }
    return OK;
}
int32_t RSetBufferSize(struct IAudioRenderVdi *, uint32_t)
{
    return OK;
}

/* --------------------------------------------------------- capture ops */
void CaptureClose(Capture *c)
{
    if (!c->pcm) {
        return;
    }
    pcm_close(c->pcm);
    c->pcm = nullptr;
    std::lock_guard<std::mutex> g(g_route.lock);
    g_route.Reset(c->usecasePath);
    g_route.Reset(c->devicePath);
    LOGI("capture %p closed (%llu frames)", c, (unsigned long long)c->read);
}

bool CaptureOpen(Capture *c)
{
    if (c->pcm) {
        return true;
    }
    if (NowNs() < c->retryAt) {
        return false;
    }
    c->retryAt = NowNs() + 3000000000LL;
    std::lock_guard<std::mutex> g(g_route.lock);
    if (!g_route.Ready()) {
        if (!c->openFailedLogged) {
            LOGE("capture: no sound card yet, recording silence");
            c->openFailedLogged = true;
        }
        return false;
    }
    c->devicePath = InDevicePath(c->pin);
    c->usecasePath = "audio-record";
    struct pcm_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.channels = c->attrs.channelCount ? c->attrs.channelCount : 2;
    cfg.rate = RateOf(c->attrs);
    cfg.period_size = cfg.rate / 50; /* 20 ms */
    cfg.period_count = PERIOD_COUNT;
    cfg.format = PCM_FORMAT_S16_LE;
    g_route.Apply(c->devicePath);
    g_route.Apply(c->usecasePath);
    struct pcm *p = pcm_open(CARD, PCM_RECORD, PCM_IN | PCM_MONOTONIC, &cfg);
    if (!p || !pcm_is_ready(p)) {
        if (!c->openFailedLogged) {
            LOGE("capture: pcm %u open failed: %s", PCM_RECORD, p ? pcm_get_error(p) : "no memory");
            c->openFailedLogged = true;
        }
        if (p) {
            pcm_close(p);
        }
        g_route.Reset(c->usecasePath);
        g_route.Reset(c->devicePath);
        return false;
    }
    c->pcm = p;
    c->openFailedLogged = false;
    c->retryAt = 0;
    LOGI("capture %p: pcm %u open %u Hz %u ch, %s + %s", c, PCM_RECORD, cfg.rate, cfg.channels,
        c->usecasePath.c_str(), c->devicePath.c_str());
    return true;
}

int32_t CCaptureFrame(struct IAudioCaptureVdi *self, int8_t *frame, uint32_t *frameLen, uint64_t *replyBytes)
{
    Capture *c = C(self);
    if (!frame || !frameLen || !replyBytes) {
        return ERR_INVALID;
    }
    std::lock_guard<std::mutex> g(c->lock);
    uint32_t fb = FrameBytes(c->attrs);
    uint64_t frames = *frameLen / fb;
    size_t samples = frames * (c->attrs.channelCount ? c->attrs.channelCount : 2);
    *replyBytes = frames * fb;
    if (!CaptureOpen(c)) {
        memset(frame, 0, *replyBytes);
        ConsumeRealtime(c->fakeClock, frames, RateOf(c->attrs));
        c->read += frames;
        return OK;
    }
    c->conv.resize(samples);
    int ret = pcm_readi(c->pcm, c->conv.data(), frames);
    if (ret < 0) {
        LOGE("capture: read failed: %s", pcm_get_error(c->pcm));
        CaptureClose(c);
        memset(c->conv.data(), 0, samples * 2);
    }
    if (c->mute) {
        memset(c->conv.data(), 0, samples * 2);
    }
    FromS16(c->attrs.format, c->conv.data(), samples, frame);
    c->read += frames;
    return OK;
}
int32_t CCaptureFrameEc(struct IAudioCaptureVdi *self, struct AudioCaptureFrameInfoVdi *info)
{
    if (!info) {
        return ERR_INVALID;
    }
    uint32_t len = info->frameLen;
    int32_t r = CCaptureFrame(self, info->frame, &len, &info->replyBytes);
    if (info->frameEc && info->frameEcLen) {
        memset(info->frameEc, 0, info->frameEcLen);
    }
    info->replyBytesEc = info->frameEcLen;
    return r;
}
int32_t CGetCapturePosition(struct IAudioCaptureVdi *self, uint64_t *frames, struct AudioTimeStampVdi *time)
{
    if (!frames || !time) {
        return ERR_INVALID;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    *frames = C(self)->read;
    time->tvSec = ts.tv_sec;
    time->tvNSec = ts.tv_nsec;
    return OK;
}
int32_t CCheckSceneCapability(struct IAudioCaptureVdi *, const struct AudioSceneDescriptorVdi *, bool *supported)
{
    if (supported) {
        *supported = true;
    }
    return OK;
}
int32_t CSelectScene(struct IAudioCaptureVdi *self, const struct AudioSceneDescriptorVdi *scene)
{
    if (!scene) {
        return ERR_INVALID;
    }
    Capture *c = C(self);
    std::lock_guard<std::mutex> g(c->lock);
    if (scene->desc.pins != PIN_VDI_NONE && c->pin != scene->desc.pins) {
        c->pin = scene->desc.pins;
        CaptureClose(c);
    }
    return OK;
}
int32_t CSetMute(struct IAudioCaptureVdi *self, bool mute)
{
    C(self)->mute = mute;
    return OK;
}
int32_t CGetMute(struct IAudioCaptureVdi *self, bool *mute)
{
    if (!mute) {
        return ERR_INVALID;
    }
    *mute = C(self)->mute;
    return OK;
}
int32_t CSetVolume(struct IAudioCaptureVdi *self, float volume)
{
    C(self)->volume = volume;
    return OK;
}
int32_t CGetVolume(struct IAudioCaptureVdi *self, float *volume)
{
    if (!volume) {
        return ERR_INVALID;
    }
    *volume = C(self)->volume;
    return OK;
}
int32_t CGetGainThreshold(struct IAudioCaptureVdi *, float *min, float *max)
{
    if (!min || !max) {
        return ERR_INVALID;
    }
    *min = 0.f;
    *max = 15.f;
    return OK;
}
int32_t CGetGain(struct IAudioCaptureVdi *self, float *gain)
{
    if (!gain) {
        return ERR_INVALID;
    }
    *gain = C(self)->gain;
    return OK;
}
int32_t CSetGain(struct IAudioCaptureVdi *self, float gain)
{
    C(self)->gain = gain;
    return OK;
}
int32_t CGetFrameSize(struct IAudioCaptureVdi *self, uint64_t *size)
{
    if (!size) {
        return ERR_INVALID;
    }
    *size = FrameBytes(C(self)->attrs);
    return OK;
}
int32_t CGetFrameCount(struct IAudioCaptureVdi *self, uint64_t *count)
{
    if (!count) {
        return ERR_INVALID;
    }
    *count = C(self)->read;
    return OK;
}
int32_t CSetSampleAttributes(struct IAudioCaptureVdi *self, const struct AudioSampleAttributesVdi *attrs)
{
    if (!attrs) {
        return ERR_INVALID;
    }
    Capture *c = C(self);
    std::lock_guard<std::mutex> g(c->lock);
    CaptureClose(c);
    c->attrs = *attrs;
    return OK;
}
int32_t CGetSampleAttributes(struct IAudioCaptureVdi *self, struct AudioSampleAttributesVdi *attrs)
{
    if (!attrs) {
        return ERR_INVALID;
    }
    *attrs = C(self)->attrs;
    return OK;
}
int32_t CGetCurrentChannelId(struct IAudioCaptureVdi *, uint32_t *channelId)
{
    if (!channelId) {
        return ERR_INVALID;
    }
    *channelId = 0;
    return OK;
}
int32_t CSetExtraParams(struct IAudioCaptureVdi *, const char *)
{
    return OK;
}
int32_t CGetExtraParams(struct IAudioCaptureVdi *, char *keyValueList, uint32_t len)
{
    if (keyValueList && len) {
        keyValueList[0] = 0;
    }
    return OK;
}
int32_t CReqMmapBuffer(struct IAudioCaptureVdi *, int32_t, struct AudioMmapBufferDescriptorVdi *)
{
    return ERR_NOT_SUPPORT;
}
int32_t CGetMmapPosition(struct IAudioCaptureVdi *, uint64_t *, struct AudioTimeStampVdi *)
{
    return ERR_NOT_SUPPORT;
}
int32_t CAddAudioEffect(struct IAudioCaptureVdi *, uint64_t)
{
    return OK;
}
int32_t CRemoveAudioEffect(struct IAudioCaptureVdi *, uint64_t)
{
    return OK;
}
int32_t CGetFrameBufferSize(struct IAudioCaptureVdi *self, uint64_t *bufferSize)
{
    if (!bufferSize) {
        return ERR_INVALID;
    }
    *bufferSize = uint64_t(RateOf(C(self)->attrs) / 50) * PERIOD_COUNT * FrameBytes(C(self)->attrs);
    return OK;
}
int32_t CStart(struct IAudioCaptureVdi *self)
{
    Capture *c = C(self);
    std::lock_guard<std::mutex> g(c->lock);
    c->fakeClock = 0;
    return OK;
}
int32_t CStop(struct IAudioCaptureVdi *self)
{
    Capture *c = C(self);
    std::lock_guard<std::mutex> g(c->lock);
    CaptureClose(c);
    return OK;
}
int32_t CPause(struct IAudioCaptureVdi *self)
{
    return CStop(self);
}
int32_t CResume(struct IAudioCaptureVdi *self)
{
    return CStart(self);
}
int32_t CFlush(struct IAudioCaptureVdi *)
{
    return OK;
}
int32_t CTurnStandbyMode(struct IAudioCaptureVdi *self)
{
    return CStop(self);
}
int32_t CAudioDevDump(struct IAudioCaptureVdi *self, int32_t, int32_t fd)
{
    Capture *c = C(self);
    if (fd >= 0) {
        dprintf(fd, "sweet capture %p: pcm %s %u Hz %u ch read %llu\n", c, c->pcm ? "open" : "closed",
            RateOf(c->attrs), c->attrs.channelCount, (unsigned long long)c->read);
    }
    return OK;
}
int32_t CIsSupportsPauseAndResume(struct IAudioCaptureVdi *, bool *supportPause, bool *supportResume)
{
    if (supportPause) {
        *supportPause = true;
    }
    if (supportResume) {
        *supportResume = true;
    }
    return OK;
}

/* --------------------------------------------------------- adapter ops */
struct Adapter {
    struct IAudioAdapterVdi vtbl;
    bool micMute = false;
    int32_t nextRoute = 1;
};
Adapter g_adapter;

int32_t AInitAllPorts(struct IAudioAdapterVdi *)
{
    return OK;
}

int32_t ACreateRender(struct IAudioAdapterVdi *, const struct AudioDeviceDescriptorVdi *desc,
    const struct AudioSampleAttributesVdi *attrs, struct IAudioRenderVdi **render)
{
    if (!desc || !attrs || !render) {
        return ERR_INVALID;
    }
    if (attrs->type == AUDIO_VDI_MMAP_NOIRQ || attrs->type == AUDIO_VDI_MMAP_VOIP ||
        attrs->type == AUDIO_VDI_OFFLOAD) {
        LOGI("render: category %d not supported (pins 0x%x)", int(attrs->type), unsigned(desc->pins));
        return ERR_NOT_SUPPORT;
    }
    Render *r = new Render();
    struct IAudioRenderVdi &v = r->vtbl;
    v.GetLatency = RGetLatency;
    v.RenderFrame = RRenderFrame;
    v.GetRenderPosition = RGetRenderPosition;
    v.SetRenderSpeed = RSetRenderSpeed;
    v.GetRenderSpeed = RGetRenderSpeed;
    v.SetChannelMode = RSetChannelMode;
    v.GetChannelMode = RGetChannelMode;
    v.RegCallback = RRegCallback;
    v.DrainBuffer = RDrainBuffer;
    v.IsSupportsDrain = RIsSupportsDrain;
    v.CheckSceneCapability = RCheckSceneCapability;
    v.SelectScene = RSelectScene;
    v.SetMute = RSetMute;
    v.GetMute = RGetMute;
    v.SetVolume = RSetVolume;
    v.GetVolume = RGetVolume;
    v.GetGainThreshold = RGetGainThreshold;
    v.GetGain = RGetGain;
    v.SetGain = RSetGain;
    v.GetFrameSize = RGetFrameSize;
    v.GetFrameCount = RGetFrameCount;
    v.SetSampleAttributes = RSetSampleAttributes;
    v.GetSampleAttributes = RGetSampleAttributes;
    v.GetCurrentChannelId = RGetCurrentChannelId;
    v.SetExtraParams = RSetExtraParams;
    v.GetExtraParams = RGetExtraParams;
    v.ReqMmapBuffer = RReqMmapBuffer;
    v.GetMmapPosition = RGetMmapPosition;
    v.AddAudioEffect = RAddAudioEffect;
    v.RemoveAudioEffect = RRemoveAudioEffect;
    v.GetFrameBufferSize = RGetFrameBufferSize;
    v.Start = RStart;
    v.Stop = RStop;
    v.Pause = RPause;
    v.Resume = RResume;
    v.Flush = RFlush;
    v.TurnStandbyMode = RTurnStandbyMode;
    v.AudioDevDump = RAudioDevDump;
    v.IsSupportsPauseAndResume = RIsSupportsPauseAndResume;
    v.SetBufferSize = RSetBufferSize;
    r->attrs = *attrs;
    r->pin = desc->pins != PIN_VDI_NONE ? uint32_t(desc->pins) : g_outPin;
    {
        std::lock_guard<std::mutex> g(g_streamsLock);
        g_renders.push_back(r);
    }
    LOGI("render %p created: category %d, %u Hz, %u ch, format 0x%x, pins 0x%x", r, int(attrs->type),
        attrs->sampleRate, attrs->channelCount, unsigned(attrs->format), unsigned(desc->pins));
    *render = &r->vtbl;
    return OK;
}

int32_t ADestroyRender(struct IAudioAdapterVdi *, struct IAudioRenderVdi *render)
{
    if (!render) {
        return ERR_INVALID;
    }
    Render *r = R(render);
    {
        std::lock_guard<std::mutex> g(g_streamsLock);
        for (auto it = g_renders.begin(); it != g_renders.end(); ++it) {
            if (*it == r) {
                g_renders.erase(it);
                break;
            }
        }
    }
    {
        std::lock_guard<std::mutex> g(r->lock);
        RenderClose(r);
    }
    delete r;
    return OK;
}

int32_t ACreateCapture(struct IAudioAdapterVdi *, const struct AudioDeviceDescriptorVdi *desc,
    const struct AudioSampleAttributesVdi *attrs, struct IAudioCaptureVdi **capture)
{
    if (!desc || !attrs || !capture) {
        return ERR_INVALID;
    }
    if (attrs->type == AUDIO_VDI_MMAP_NOIRQ || attrs->type == AUDIO_VDI_MMAP_VOIP) {
        return ERR_NOT_SUPPORT;
    }
    Capture *c = new Capture();
    struct IAudioCaptureVdi &v = c->vtbl;
    v.CaptureFrame = CCaptureFrame;
    v.CaptureFrameEc = CCaptureFrameEc;
    v.GetCapturePosition = CGetCapturePosition;
    v.CheckSceneCapability = CCheckSceneCapability;
    v.SelectScene = CSelectScene;
    v.SetMute = CSetMute;
    v.GetMute = CGetMute;
    v.SetVolume = CSetVolume;
    v.GetVolume = CGetVolume;
    v.GetGainThreshold = CGetGainThreshold;
    v.GetGain = CGetGain;
    v.SetGain = CSetGain;
    v.GetFrameSize = CGetFrameSize;
    v.GetFrameCount = CGetFrameCount;
    v.SetSampleAttributes = CSetSampleAttributes;
    v.GetSampleAttributes = CGetSampleAttributes;
    v.GetCurrentChannelId = CGetCurrentChannelId;
    v.SetExtraParams = CSetExtraParams;
    v.GetExtraParams = CGetExtraParams;
    v.ReqMmapBuffer = CReqMmapBuffer;
    v.GetMmapPosition = CGetMmapPosition;
    v.AddAudioEffect = CAddAudioEffect;
    v.RemoveAudioEffect = CRemoveAudioEffect;
    v.GetFrameBufferSize = CGetFrameBufferSize;
    v.Start = CStart;
    v.Stop = CStop;
    v.Pause = CPause;
    v.Resume = CResume;
    v.Flush = CFlush;
    v.TurnStandbyMode = CTurnStandbyMode;
    v.AudioDevDump = CAudioDevDump;
    v.IsSupportsPauseAndResume = CIsSupportsPauseAndResume;
    c->attrs = *attrs;
    c->pin = desc->pins != PIN_VDI_NONE ? uint32_t(desc->pins) : g_inPin;
    {
        std::lock_guard<std::mutex> g(g_streamsLock);
        g_captures.push_back(c);
    }
    LOGI("capture %p created: source %d, %u Hz, %u ch, format 0x%x, pins 0x%x", c, attrs->sourceType,
        attrs->sampleRate, attrs->channelCount, unsigned(attrs->format), unsigned(desc->pins));
    *capture = &c->vtbl;
    return OK;
}

int32_t ADestroyCapture(struct IAudioAdapterVdi *, struct IAudioCaptureVdi *capture)
{
    if (!capture) {
        return ERR_INVALID;
    }
    Capture *c = C(capture);
    {
        std::lock_guard<std::mutex> g(g_streamsLock);
        for (auto it = g_captures.begin(); it != g_captures.end(); ++it) {
            if (*it == c) {
                g_captures.erase(it);
                break;
            }
        }
    }
    {
        std::lock_guard<std::mutex> g(c->lock);
        CaptureClose(c);
    }
    delete c;
    return OK;
}

enum AudioFormatVdi g_formats[] = {AUDIO_VDI_FORMAT_TYPE_PCM_16_BIT, AUDIO_VDI_FORMAT_TYPE_PCM_24_BIT,
    AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT};
enum AudioSampleFormatVdi g_sampleFormats[] = {AUDIO_VDI_SAMPLE_FORMAT_S16, AUDIO_VDI_SAMPLE_FORMAT_S24,
    AUDIO_VDI_SAMPLE_FORMAT_S32};
char g_subPortDesc[] = "primary";
struct AudioSubPortCapabilityVdi g_subPort = {0, g_subPortDesc, PORT_VDI_PASSTHROUGH_LPCM};

int32_t AGetPortCapability(struct IAudioAdapterVdi *, const struct AudioPortVdi *port,
    struct AudioPortCapabilityVdi *cap)
{
    if (!port || !cap) {
        return ERR_INVALID;
    }
    memset(cap, 0, sizeof(*cap));
    cap->deviceType = port->dir;
    cap->deviceId = port->portId;
    cap->hardwareMode = true;
    cap->formatNum = 3;
    cap->formats = g_formats;
    cap->formatsLen = 3;
    cap->sampleRateMasks = AUDIO_VDI_SAMPLE_RATE_MASK_8000 | AUDIO_VDI_SAMPLE_RATE_MASK_16000 |
        AUDIO_VDI_SAMPLE_RATE_MASK_32000 | AUDIO_VDI_SAMPLE_RATE_MASK_44100 | AUDIO_VDI_SAMPLE_RATE_MASK_48000;
    cap->channelMasks = AUDIO_VDI_CHANNEL_STEREO;
    cap->channelCount = 2;
    cap->subPorts = &g_subPort;
    cap->subPortsLen = 1;
    cap->supportSampleFormats = g_sampleFormats;
    cap->supportSampleFormatsLen = 3;
    return OK;
}
int32_t ASetPassthroughMode(struct IAudioAdapterVdi *, const struct AudioPortVdi *, enum AudioPortPassthroughModeVdi mode)
{
    return mode == PORT_VDI_PASSTHROUGH_LPCM ? OK : ERR_NOT_SUPPORT;
}
int32_t AGetPassthroughMode(struct IAudioAdapterVdi *, const struct AudioPortVdi *, enum AudioPortPassthroughModeVdi *mode)
{
    if (!mode) {
        return ERR_INVALID;
    }
    *mode = PORT_VDI_PASSTHROUGH_LPCM;
    return OK;
}
int32_t AGetDeviceStatus(struct IAudioAdapterVdi *, struct AudioDeviceStatusVdi *status)
{
    if (!status) {
        return ERR_INVALID;
    }
    status->pnpStatus = 0;
    return OK;
}

int32_t AUpdateAudioRoute(struct IAudioAdapterVdi *, const struct AudioRouteVdi *route, int32_t *routeHandle)
{
    if (!route || !routeHandle) {
        return ERR_INVALID;
    }
    uint32_t outPin = 0, inPin = 0;
    for (uint32_t i = 0; i < route->sinksLen; i++) {
        const struct AudioRouteNodeVdi &n = route->sinks[i];
        if (n.type == AUDIO_VDI_PORT_DEVICE_TYPE && !(uint32_t(n.ext.device.type) & (1u << 27))) {
            outPin = n.ext.device.type;
        }
    }
    for (uint32_t i = 0; i < route->sourcesLen; i++) {
        const struct AudioRouteNodeVdi &n = route->sources[i];
        if (n.type == AUDIO_VDI_PORT_DEVICE_TYPE && (uint32_t(n.ext.device.type) & (1u << 27))) {
            inPin = n.ext.device.type;
        }
    }
    if (outPin) {
        LOGI("route: output -> pin 0x%x (%s)", outPin, OutDevicePath(outPin).c_str());
        g_outPin = outPin;
        {
            std::lock_guard<std::mutex> vg(g_route.lock);
            if (g_voice.active) {
                VoiceStartLocked(outPin);
            }
        }
        std::lock_guard<std::mutex> g(g_streamsLock);
        for (Render *r : g_renders) {
            RenderReroute(r, outPin);
        }
    }
    if (inPin) {
        LOGI("route: input -> pin 0x%x", inPin);
        g_inPin = inPin;
    }
    *routeHandle = g_adapter.nextRoute++;
    return OK;
}
int32_t AReleaseAudioRoute(struct IAudioAdapterVdi *, int32_t)
{
    return OK;
}
int32_t ASetMicMute(struct IAudioAdapterVdi *, bool mute)
{
    g_adapter.micMute = mute;
    {
        std::lock_guard<std::mutex> vg(g_route.lock);
        g_voice.micMute = mute;
        if (g_voice.active) {
            VoiceApplyGain();
        }
    }
    std::lock_guard<std::mutex> g(g_streamsLock);
    for (Capture *c : g_captures) {
        c->mute = mute;
    }
    return OK;
}
int32_t AGetMicMute(struct IAudioAdapterVdi *, bool *mute)
{
    if (!mute) {
        return ERR_INVALID;
    }
    *mute = g_adapter.micMute;
    return OK;
}
int32_t ASetVoiceVolume(struct IAudioAdapterVdi *, float volume)
{
    std::lock_guard<std::mutex> vg(g_route.lock);
    g_voice.volume = volume;
    if (g_voice.active) {
        VoiceApplyGain();
    }
    return OK;
}
int32_t ASetExtraParams(struct IAudioAdapterVdi *, enum AudioExtParamKeyVdi, const char *, const char *)
{
    return OK;
}
int32_t AGetExtraParams(struct IAudioAdapterVdi *, enum AudioExtParamKeyVdi, const char *, char *value, uint32_t len)
{
    if (value && len) {
        value[0] = 0;
    }
    return OK;
}
int32_t ARegExtraParamObserver(struct IAudioAdapterVdi *, struct IAudioCallbackVdi *, int8_t)
{
    return OK;
}

/* --------------------------------------------------------- manager ops */
const char *ADAPTER_NAME = "primary";

int32_t MGetAllAdapters(struct IAudioManagerVdi *, struct AudioAdapterDescriptorVdi *descs, uint32_t *descsLen)
{
    if (!descs || !descsLen || *descsLen == 0) {
        return ERR_INVALID;
    }
    struct AudioPortVdi *ports = static_cast<struct AudioPortVdi *>(calloc(2, sizeof(struct AudioPortVdi)));
    if (!ports) {
        return ERR;
    }
    ports[0].dir = PORT_VDI_OUT;
    ports[0].portId = 0;
    ports[0].portName = strdup("AOP");
    ports[1].dir = PORT_VDI_IN;
    ports[1].portId = 11;
    ports[1].portName = strdup("AIP");
    descs[0].adapterName = strdup(ADAPTER_NAME);
    descs[0].ports = ports;
    descs[0].portsLen = 2;
    *descsLen = 1;
    LOGI("GetAllAdapters: primary (speaker/headphones/earpiece, mic)");
    return OK;
}

int32_t MLoadAdapter(struct IAudioManagerVdi *, const struct AudioAdapterDescriptorVdi *desc,
    struct IAudioAdapterVdi **adapter)
{
    if (!desc || !adapter) {
        return ERR_INVALID;
    }
    if (!desc->adapterName || strcmp(desc->adapterName, ADAPTER_NAME) != 0) {
        LOGE("LoadAdapter: unknown adapter %s", desc->adapterName ? desc->adapterName : "(null)");
        return ERR_NOT_SUPPORT;
    }
    struct IAudioAdapterVdi &v = g_adapter.vtbl;
    v.InitAllPorts = AInitAllPorts;
    v.CreateRender = ACreateRender;
    v.DestroyRender = ADestroyRender;
    v.CreateCapture = ACreateCapture;
    v.DestroyCapture = ADestroyCapture;
    v.GetPortCapability = AGetPortCapability;
    v.SetPassthroughMode = ASetPassthroughMode;
    v.GetPassthroughMode = AGetPassthroughMode;
    v.GetDeviceStatus = AGetDeviceStatus;
    v.UpdateAudioRoute = AUpdateAudioRoute;
    v.ReleaseAudioRoute = AReleaseAudioRoute;
    v.SetMicMute = ASetMicMute;
    v.GetMicMute = AGetMicMute;
    v.SetVoiceVolume = ASetVoiceVolume;
    v.SetExtraParams = ASetExtraParams;
    v.GetExtraParams = AGetExtraParams;
    v.RegExtraParamObserver = ARegExtraParamObserver;
    {
        std::lock_guard<std::mutex> g(g_route.lock);
        bool ready = g_route.Ready();
        LOGI("LoadAdapter primary: sound card %s", ready ? "ready" : "not there yet");
    }
    *adapter = &v;
    return OK;
}

int32_t MUnloadAdapter(struct IAudioManagerVdi *, struct IAudioAdapterVdi *)
{
    return OK;
}

int32_t MReleaseAudioManagerObject(struct IAudioManagerVdi *)
{
    return OK;
}

struct IAudioManagerVdi g_manager = {MGetAllAdapters, MLoadAdapter, MUnloadAdapter, MReleaseAudioManagerObject};
} // namespace

extern "C" EXPORT struct IAudioManagerVdi *AudioManagerCreateIfInstance(void)
{
    LOGI("sweet audio VDI (tinyalsa, sm6150)");
    return &g_manager;
}

extern "C" EXPORT int32_t AudioManagerDestroyIfInstance(struct IAudioManagerVdi *)
{
    return OK;
}
