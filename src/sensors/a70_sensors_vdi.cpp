/*
 * Sensor VDI for HarmonyOS on the Samsung Galaxy A70 (a70q, sm6150).
 * Based on hmos2sweet src/sensors/sweet_sensors_vdi.cpp at commit 1be080c (Apache-2.0).
 * Changed for the A70: proximity added (SSC event 769), names and logs.
 *
 * The sensors of sm6150 live in the Snapdragon Sensor Core (SSC/SEE) on the
 * ADSP. Android talks to it through the QMI "SNS client" service (id 400) with
 * protobuf messages; this library does the same over a QRTR socket, without
 * any Qualcomm code:
 *
 *   1. SUID lookup: ask the lookup sensor which sensor serves "accel", "gyro"...
 *   2. stream: send a config request (sample rate or on-change) to that SUID,
 *   3. events come back as QMI indications carrying sns_client_event_msg.
 *
 * The HarmonyOS sensor host (libsensor_interface_service_2.1.z.so) loads this
 * file as /vendor/lib64/libhdi_sensor_impl.z.so (VDI, OpenHarmony 5.1 ABI).
 *
 * Protobuf field numbers and QMI TLVs were checked against the sweet vendor
 * libraries (libsnsapi.so serializers, SNS_CLIENT_SVC v01 tables).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "isensor_interface_vdi.h"

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

using namespace OHOS;
using namespace OHOS::HDI::Sensor::V1_1;

namespace {
/* ------------------------------------------------------------------ log */
using LogFn = int (*)(int, int, unsigned int, const char *, const char *, ...);
void Log(int level, const char *fmt, ...)
{
    static LogFn fn = reinterpret_cast<LogFn>(-1);
    static int kmsg = -2;
    if (fn == reinterpret_cast<LogFn>(-1)) {
        void *h = dlopen("libhilog.so", RTLD_NOW);
        fn = h ? reinterpret_cast<LogFn>(dlsym(h, "HiLogPrint")) : nullptr;
    }
    if (kmsg == -2) {
        kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    }
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (fn) {
        fn(3 /* LOG_CORE */, level, 0xD002516, "A70_SENSORS", "%{public}s", buf);
    }
    if (kmsg >= 0 && level >= 4) {
        char k[440];
        int n = snprintf(k, sizeof(k), "<%d>a70-sensors: %s\n", level >= 6 ? 3 : 6, buf);
        (void)!write(kmsg, k, n > 0 ? n : 0);
    }
}
#define LOGD(...) Log(3, __VA_ARGS__)
#define LOGI(...) Log(4, __VA_ARGS__)
#define LOGE(...) Log(6, __VA_ARGS__)

int64_t BootNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

/* ------------------------------------------------------------- protobuf */
struct PbOut {
    std::string b;
    void Varint(uint64_t v)
    {
        do {
            uint8_t c = v & 0x7f;
            v >>= 7;
            b.push_back(char(v ? (c | 0x80) : c));
        } while (v);
    }
    void Key(int field, int wt) { Varint((uint64_t(field) << 3) | wt); }
    void U64(int f, uint64_t v) { Key(f, 0); Varint(v); }
    void Fixed32(int f, uint32_t v) { Key(f, 5); b.append(reinterpret_cast<const char *>(&v), 4); }
    void Fixed64(int f, uint64_t v) { Key(f, 1); b.append(reinterpret_cast<const char *>(&v), 8); }
    void Float(int f, float v) { uint32_t u; memcpy(&u, &v, 4); Fixed32(f, u); }
    void Bytes(int f, const std::string &s) { Key(f, 2); Varint(s.size()); b += s; }
};

struct PbField {
    int num = 0;
    int wt = 0;
    uint64_t v = 0;               /* varint / fixed value */
    const uint8_t *p = nullptr;   /* length delimited data */
    size_t len = 0;
};

class PbIn {
public:
    PbIn(const uint8_t *p, size_t n) : p_(p), e_(p + n) {}
    bool Next(PbField &f)
    {
        uint64_t key;
        if (p_ >= e_ || !Varint(key)) {
            return false;
        }
        f.num = int(key >> 3);
        f.wt = int(key & 7);
        f.p = nullptr;
        f.len = 0;
        switch (f.wt) {
            case 0: return Varint(f.v);
            case 1: if (e_ - p_ < 8) return false; memcpy(&f.v, p_, 8); p_ += 8; return true;
            case 5: { if (e_ - p_ < 4) return false; uint32_t u; memcpy(&u, p_, 4); f.v = u; p_ += 4; return true; }
            case 2: {
                uint64_t n;
                if (!Varint(n) || uint64_t(e_ - p_) < n) return false;
                f.p = p_; f.len = size_t(n); p_ += n;
                return true;
            }
            default: return false;
        }
    }
private:
    bool Varint(uint64_t &v)
    {
        v = 0;
        for (int s = 0; p_ < e_ && s < 64; s += 7) {
            uint8_t c = *p_++;
            v |= uint64_t(c & 0x7f) << s;
            if (!(c & 0x80)) return true;
        }
        return false;
    }
    const uint8_t *p_;
    const uint8_t *e_;
};

/* -------------------------------------------------------- SSC messages */
struct Suid {
    uint64_t lo = 0;
    uint64_t hi = 0;
    bool Valid() const { return lo || hi; }
};
const Suid LOOKUP_SUID = {0xababababababababULL, 0xababababababababULL};

/* message ids (sns_std.proto, sns_std_sensor.proto, sns_suid.proto, sns_client.proto) */
constexpr uint32_t MSG_CLIENT_DISABLE = 10;
constexpr uint32_t MSG_SUID_REQ = 512;
constexpr uint32_t MSG_SENSOR_CONFIG = 513;
constexpr uint32_t MSG_ON_CHANGE_CONFIG = 514;
constexpr uint32_t MSG_SUID_EVENT = 768;
constexpr uint32_t MSG_SENSOR_EVENT = 1025;
constexpr uint32_t MSG_PROXIMITY_EVENT = 769; /* field 1: 0 far, 1 near, 2 data only (a70, one cover test) */
constexpr uint32_t MSG_ERROR_EVENT = 130;

std::string EncodeSuid(const Suid &s)
{
    PbOut o;
    o.Fixed64(1, s.lo);
    o.Fixed64(2, s.hi);
    return o.b;
}

/* sns_client_request_msg { suid=1, msg_id=2 (fixed32), susp_config=3 { proc=1, delivery=2 }, request=4 { payload=2 } } */
std::string EncodeRequest(const Suid &suid, uint32_t msgId, const std::string &payload)
{
    PbOut susp;
    susp.U64(1, 1); /* SNS_STD_CLIENT_PROCESSOR_APSS */
    susp.U64(2, 1); /* SNS_CLIENT_DELIVERY_NO_WAKEUP */
    PbOut req;
    if (!payload.empty()) {
        req.Bytes(2, payload);
    }
    PbOut m;
    m.Bytes(1, EncodeSuid(suid));
    m.Fixed32(2, msgId);
    m.Bytes(3, susp.b);
    m.Bytes(4, req.b);
    return m.b;
}

Suid DecodeSuid(const uint8_t *p, size_t n)
{
    Suid s;
    PbIn in(p, n);
    PbField f;
    while (in.Next(f)) {
        if (f.num == 1 && f.wt == 1) s.lo = f.v;
        if (f.num == 2 && f.wt == 1) s.hi = f.v;
    }
    return s;
}

/* ------------------------------------------------------- QMI over QRTR */
struct SockAddrQrtr {
    unsigned short family;
    uint32_t node;
    uint32_t port;
};

constexpr uint32_t SNS_CLIENT_SERVICE = 400;
constexpr uint16_t QMI_SNS_CLIENT_REQ = 0x20;
constexpr uint16_t QMI_SNS_REPORT_IND = 0x21;
constexpr uint16_t QMI_SNS_JUMBO_REPORT_IND = 0x22;
constexpr uint32_t QRTR_TYPE_NEW_SERVER = 4;
constexpr uint32_t QRTR_TYPE_DEL_SERVER = 5;
constexpr uint32_t QRTR_TYPE_NEW_LOOKUP = 10;
constexpr uint32_t QRTR_PORT_CTRL = 0xfffffffeu;

bool FindService(uint32_t &node, uint32_t &port, int timeoutMs)
{
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOGE("socket(AF_QIPCRTR): %s", strerror(errno));
        return false;
    }
    SockAddrQrtr me = {};
    socklen_t sl = sizeof(me);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&me), &sl) < 0) {
        close(fd);
        return false;
    }
    uint32_t pkt[5] = {QRTR_TYPE_NEW_LOOKUP, SNS_CLIENT_SERVICE, 0, 0, 0};
    SockAddrQrtr ns = {AF_QIPCRTR, me.node, QRTR_PORT_CTRL};
    if (sendto(fd, pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr *>(&ns), sizeof(ns)) < 0) {
        LOGE("QRTR lookup: %s", strerror(errno));
        close(fd);
        return false;
    }
    struct timeval tv = {0, 200000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    bool found = false;
    int64_t end = BootNs() + int64_t(timeoutMs) * 1000000;
    while (!found && BootNs() < end) {
        uint32_t r[8] = {};
        ssize_t n = recv(fd, r, sizeof(r), 0);
        if (n < 20) {
            continue;
        }
        if (r[0] == QRTR_TYPE_NEW_SERVER && r[1] == SNS_CLIENT_SERVICE) {
            node = r[3];
            port = r[4];
            found = true;
        }
    }
    close(fd);
    return found;
}

/* one QMI client of the SNS service: one socket per stream */
class SnsClient {
public:
    ~SnsClient() { Close(); }
    bool Open(uint32_t node, uint32_t port)
    {
        Close();
        fd_ = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            return false;
        }
        struct timeval tv = {0, 300000};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        node_ = node;
        port_ = port;
        return true;
    }
    void Close()
    {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = -1;
    }
    bool Send(const std::string &pb)
    {
        if (fd_ < 0 || pb.size() > 1000) {
            return false;
        }
        std::vector<uint8_t> m(7);
        auto tlv = [&m](uint8_t t, const void *v, uint16_t len) {
            m.push_back(t);
            m.push_back(uint8_t(len));
            m.push_back(uint8_t(len >> 8));
            const uint8_t *b = static_cast<const uint8_t *>(v);
            m.insert(m.end(), b, b + len);
        };
        std::vector<uint8_t> payload(2 + pb.size());
        payload[0] = uint8_t(pb.size());
        payload[1] = uint8_t(pb.size() >> 8);
        memcpy(payload.data() + 2, pb.data(), pb.size());
        tlv(0x01, payload.data(), uint16_t(payload.size()));
        uint8_t jumbo = 1;
        tlv(0x10, &jumbo, 1);
        uint16_t txn = ++txn_;
        uint16_t len = uint16_t(m.size() - 7);
        m[0] = 0; /* request */
        m[1] = uint8_t(txn);
        m[2] = uint8_t(txn >> 8);
        m[3] = uint8_t(QMI_SNS_CLIENT_REQ);
        m[4] = 0;
        m[5] = uint8_t(len);
        m[6] = uint8_t(len >> 8);
        SockAddrQrtr to = {AF_QIPCRTR, node_, port_};
        return sendto(fd_, m.data(), m.size(), 0, reinterpret_cast<sockaddr *>(&to), sizeof(to)) == ssize_t(m.size());
    }
    /* returns: 1 = indication payload in `pb`, 2 = response (result in `err`), 0 = timeout, -1 = error */
    int Recv(std::vector<uint8_t> &buf, const uint8_t *&pb, size_t &pbLen, uint16_t &err)
    {
        buf.resize(65536);
        ssize_t n = recv(fd_, buf.data(), buf.size(), 0);
        if (n < 0) {
            return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
        }
        if (n < 7) {
            return 0;
        }
        uint8_t type = buf[0];
        uint16_t msg = uint16_t(buf[3] | (buf[4] << 8));
        size_t end = std::min<size_t>(size_t(n), 7u + uint16_t(buf[5] | (buf[6] << 8)));
        size_t i = 7;
        err = 0;
        while (i + 3 <= end) {
            uint8_t t = buf[i];
            uint16_t l = uint16_t(buf[i + 1] | (buf[i + 2] << 8));
            const uint8_t *v = &buf[i + 3];
            if (i + 3 + l > end) {
                break;
            }
            if (type == 2 && t == 0x02 && l >= 4) {
                err = uint16_t(v[2] | (v[3] << 8)); /* qmi_response: result, error */
            }
            if (type == 4 && (msg == QMI_SNS_REPORT_IND || msg == QMI_SNS_JUMBO_REPORT_IND) && t == 0x02 && l >= 2) {
                size_t pl = size_t(v[0] | (v[1] << 8));
                if (pl + 2 <= l) {
                    pb = v + 2;
                    pbLen = pl;
                    return 1;
                }
            }
            i += 3 + l;
        }
        return type == 2 ? 2 : 0;
    }
private:
    int fd_ = -1;
    uint32_t node_ = 0;
    uint32_t port_ = 0;
    uint16_t txn_ = 0;
};

/* ------------------------------------------------------------ sensors */
enum Kind { CONTINUOUS, ON_CHANGE, VIRTUAL_GRAVITY };

struct SensorDef {
    int32_t type;           /* HDF sensor type id (SensorTypes.idl) */
    const char *name;
    const char *dataType;   /* SSC data type */
    Kind kind;
    int floats;             /* values reported to HarmonyOS */
    float maxRange;
    float resolution;
    float power;
};

const SensorDef SENSORS[] = {
    {1, "accelerometer", "accel", CONTINUOUS, 3, 156.9f, 0.0024f, 0.17f},
    {2, "gyroscope", "gyro", CONTINUOUS, 3, 34.9f, 0.0011f, 0.55f},
    {5, "ambient_light", "ambient_light", ON_CHANGE, 3, 60000.f, 0.01f, 0.1f},
    {12, "proximity", "proximity", ON_CHANGE, 1, 5.f, 5.f, 0.1f},
    {6, "magnetic_field", "mag", CONTINUOUS, 3, 4912.f, 0.15f, 0.5f},
    {257, "gravity", "gravity", VIRTUAL_GRAVITY, 3, 19.6f, 0.0024f, 0.2f},
    {259, "rotation_vector", "rotv", CONTINUOUS, 4, 1.f, 0.0001f, 0.8f},
    {262, "game_rotation_vector", "game_rv", CONTINUOUS, 4, 1.f, 0.0001f, 0.7f},
};

constexpr float DEFAULT_RATE_HZ = 50.f;

class SweetSensors;

struct Stream {
    const SensorDef *def = nullptr;
    Suid suid;
    float rateHz = DEFAULT_RATE_HZ;
    std::atomic<bool> enabled{false};    /* asked by HarmonyOS */
    int users = 0;                       /* enabled + internal users (gravity from accel) */
    std::shared_ptr<std::atomic<bool>> stop; /* stop flag of the running stream thread */
    std::atomic<bool> reconfig{false};
};

class SweetSensors : public ISensorInterfaceVdi {
public:
    SweetSensors()
    {
        for (const auto &d : SENSORS) {
            auto *s = new Stream;
            s->def = &d;
            streams_[d.type] = s;
        }
    }
    ~SweetSensors() override
    {
        for (auto &it : streams_) {
            StopStream(it.second);
            delete it.second;
        }
    }

    int32_t Init() override
    {
        LOGI("sensor VDI for a70 (SSC over QRTR)");
        /* discovery runs in the background: the ADSP sensor domain comes up
         * well after the sensor host */
        std::thread([this] { Discover(); }).detach();
        return HDF_SUCCESS;
    }

    int32_t GetAllSensorInfo(std::vector<HdfSensorInformationVdi> &info) override
    {
        for (const auto &d : SENSORS) {
            HdfSensorInformationVdi i;
            i.sensorName = d.name;
            i.vendorName = "Qualcomm SSC";
            i.firmwareVersion = "1";
            i.hardwareVersion = "sm6150";
            i.sensorTypeId = d.type;
            i.sensorId = d.type;
            i.maxRange = d.maxRange;
            i.accuracy = d.resolution;
            i.power = d.power;
            i.minDelay = d.kind == ON_CHANGE ? 0 : 5000000;      /* 200 Hz */
            i.maxDelay = 1000000000;
            i.fifoMaxEventCount = 0;
            i.reserved = 0;
            info.push_back(i);
        }
        return HDF_SUCCESS;
    }

    int32_t Enable(int32_t sensorId) override
    {
        std::lock_guard<std::mutex> l(mu_);
        Stream *s = Find(sensorId);
        if (!s) {
            return HDF_ERR_NOT_SUPPORT;
        }
        if (!s->enabled) {
            s->enabled = true;
            Acquire(s);
        }
        LOGI("enable %s (%.1f Hz)", s->def->name, s->rateHz);
        return HDF_SUCCESS;
    }

    int32_t Disable(int32_t sensorId) override
    {
        std::lock_guard<std::mutex> l(mu_);
        Stream *s = Find(sensorId);
        if (!s) {
            return HDF_ERR_NOT_SUPPORT;
        }
        if (s->enabled) {
            s->enabled = false;
            Release(s);
        }
        LOGI("disable %s", s->def->name);
        return HDF_SUCCESS;
    }

    int32_t SetBatch(int32_t sensorId, int64_t samplingInterval, int64_t reportInterval) override
    {
        (void)reportInterval;
        std::lock_guard<std::mutex> l(mu_);
        Stream *s = Find(sensorId);
        if (!s) {
            return HDF_ERR_NOT_SUPPORT;
        }
        float hz = samplingInterval > 0 ? float(1e9 / double(samplingInterval)) : DEFAULT_RATE_HZ;
        hz = std::max(1.f, std::min(200.f, hz));
        if (s->def->kind == VIRTUAL_GRAVITY) {
            Stream *a = Find(1);
            if (a && hz > a->rateHz) {
                a->rateHz = hz;
                a->reconfig = true;
            }
        }
        if (std::fabs(hz - s->rateHz) > 0.01f) {
            s->rateHz = hz;
            s->reconfig = true;
        }
        return HDF_SUCCESS;
    }

    int32_t SetMode(int32_t sensorId, int32_t mode) override { (void)sensorId; (void)mode; return HDF_SUCCESS; }
    int32_t SetOption(int32_t sensorId, uint32_t option) override { (void)sensorId; (void)option; return HDF_SUCCESS; }

    int32_t Register(int32_t groupId, const sptr<ISensorCallbackVdi> &callbackObj) override
    {
        std::lock_guard<std::mutex> l(cbMu_);
        if (callbackObj == nullptr) {
            return HDF_ERR_INVALID_PARAM;
        }
        for (auto &c : cbs_[groupId]) {
            if (c == callbackObj) {
                return HDF_SUCCESS;
            }
        }
        cbs_[groupId].push_back(callbackObj);
        LOGI("callback registered for group %d", groupId);
        return HDF_SUCCESS;
    }

    int32_t Unregister(int32_t groupId, const sptr<ISensorCallbackVdi> &callbackObj) override
    {
        std::lock_guard<std::mutex> l(cbMu_);
        auto &v = cbs_[groupId];
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (*it == callbackObj) {
                v.erase(it);
                break;
            }
        }
        return HDF_SUCCESS;
    }

    int32_t SetSaBatch(int32_t sensorId, int64_t samplingInterval, int64_t reportInterval) override
    {
        return SetBatch(sensorId, samplingInterval, reportInterval);
    }

private:
    Stream *Find(int32_t id)
    {
        auto it = streams_.find(id);
        return it == streams_.end() ? nullptr : it->second;
    }

    /* mu_ held */
    void Acquire(Stream *s)
    {
        if (s->def->kind == VIRTUAL_GRAVITY && !s->suid.Valid()) {
            /* no SSC gravity: derive it from the accelerometer */
            Stream *a = Find(1);
            gravityFromAccel_ = true;
            if (a) {
                Acquire(a);
            }
            return;
        }
        if (s->users++ == 0) {
            StartStream(s);
        }
    }

    void Release(Stream *s)
    {
        if (s->def->kind == VIRTUAL_GRAVITY && gravityFromAccel_) {
            gravityFromAccel_ = false;
            Stream *a = Find(1);
            if (a) {
                Release(a);
            }
            return;
        }
        if (s->users > 0 && --s->users == 0) {
            StopStream(s);
        }
    }

    /* mu_ held. Stream threads are never joined here (they may be waiting for
     * mu_): each run has its own stop flag and ends on its own. */
    void StartStream(Stream *s)
    {
        StopStream(s);
        auto stop = std::make_shared<std::atomic<bool>>(false);
        s->stop = stop;
        s->reconfig = false;
        std::thread([this, s, stop] { RunStream(s, *stop); }).detach();
    }

    void StopStream(Stream *s)
    {
        if (s->stop) {
            *s->stop = true;
            s->stop.reset();
        }
    }

    bool WaitService(uint32_t &node, uint32_t &port, const std::atomic<bool> *stop)
    {
        for (int i = 0; ; i++) {
            {
                std::lock_guard<std::mutex> l(svcMu_);
                if (svcValid_) {
                    node = svcNode_;
                    port = svcPort_;
                    return true;
                }
            }
            if (FindService(node, port, 1000)) {
                std::lock_guard<std::mutex> l(svcMu_);
                svcNode_ = node;
                svcPort_ = port;
                svcValid_ = true;
                LOGI("SNS client service at %u:%u", node, port);
                return true;
            }
            if (stop && *stop) {
                return false;
            }
            if (i == 0) {
                LOGI("waiting for the SSC (ADSP sensor domain)");
            }
            sleep(2);
        }
    }

    void InvalidateService()
    {
        std::lock_guard<std::mutex> l(svcMu_);
        svcValid_ = false;
    }

    Suid LookupSuid(uint32_t node, uint32_t port, const char *dataType)
    {
        SnsClient c;
        if (!c.Open(node, port)) {
            return {};
        }
        PbOut req;
        req.Bytes(1, dataType);
        req.U64(2, 0); /* register_updates */
        req.U64(3, 1); /* default_only */
        if (!c.Send(EncodeRequest(LOOKUP_SUID, MSG_SUID_REQ, req.b))) {
            return {};
        }
        std::vector<uint8_t> buf;
        int64_t end = BootNs() + 3000000000LL;
        while (BootNs() < end) {
            const uint8_t *pb = nullptr;
            size_t pbLen = 0;
            uint16_t err = 0;
            int r = c.Recv(buf, pb, pbLen, err);
            if (r < 0) {
                break;
            }
            if (r == 2 && err) {
                LOGE("SUID request for %s: QMI error %u", dataType, err);
                return {};
            }
            if (r != 1) {
                continue;
            }
            /* sns_client_event_msg { suid=1, events=2 { msg_id=1, timestamp=2, payload=3 } } */
            PbIn msg(pb, pbLen);
            PbField f;
            while (msg.Next(f)) {
                if (f.num != 2 || f.wt != 2) continue;
                PbIn ev(f.p, f.len);
                PbField g;
                uint32_t id = 0;
                const uint8_t *pl = nullptr;
                size_t pll = 0;
                while (ev.Next(g)) {
                    if (g.num == 1) id = uint32_t(g.v);
                    if (g.num == 3 && g.wt == 2) { pl = g.p; pll = g.len; }
                }
                if (id != MSG_SUID_EVENT || !pl) continue;
                /* sns_suid_event { data_type=1, suid=2 (repeated) } */
                PbIn se(pl, pll);
                PbField h;
                while (se.Next(h)) {
                    if (h.num == 2 && h.wt == 2) {
                        return DecodeSuid(h.p, h.len);
                    }
                }
                return {}; /* answered: no such sensor */
            }
        }
        return {};
    }

    void Discover()
    {
        uint32_t node = 0, port = 0;
        if (!WaitService(node, port, nullptr)) {
            return;
        }
        /* a70: the service answers before hexagonrpcd has let the physical sensors
         * register, and those early lookups say "none". Keep asking for 5 minutes. */
        for (int pass = 0; pass < 100; pass++) {
            bool missing = false;
            for (auto &it : streams_) {
                Stream *s = it.second;
                {
                    std::lock_guard<std::mutex> l(mu_);
                    if (s->suid.Valid()) {
                        continue;
                    }
                }
                Suid id = LookupSuid(node, port, s->def->dataType);
                std::lock_guard<std::mutex> l(mu_);
                s->suid = id;
                if (id.Valid()) {
                    LOGI("%s: SUID %016llx%016llx (pass %d)", s->def->dataType, (unsigned long long)id.hi,
                         (unsigned long long)id.lo, pass);
                } else {
                    missing = true;
                }
            }
            {
                std::lock_guard<std::mutex> l(mu_);
                if (!discovered_) {
                    discovered_ = true;
                    discoverCv_.notify_all();
                }
            }
            if (!missing) {
                LOGI("all sensors found (pass %d)", pass);
                return;
            }
            sleep(3);
        }
        LOGE("some sensors never appeared");
    }

    bool WaitDiscovery(const std::atomic<bool> &stop)
    {
        std::unique_lock<std::mutex> l(mu_);
        while (!discovered_ && !stop) {
            discoverCv_.wait_for(l, std::chrono::milliseconds(500));
        }
        return discovered_;
    }

    void RunStream(Stream *s, std::atomic<bool> &stop)
    {
        if (!WaitDiscovery(stop)) {
            return;
        }
        Suid suid;
        while (!stop) {
            {
                std::lock_guard<std::mutex> l(mu_);
                suid = s->suid;
            }
            if (suid.Valid()) {
                break;
            }
            sleep(1);
        }
        if (stop) {
            return;
        }
        std::vector<uint8_t> buf;
        while (!stop) {
            uint32_t node = 0, port = 0;
            if (!WaitService(node, port, &stop)) {
                return;
            }
            SnsClient c;
            if (!c.Open(node, port) || !Configure(c, s, suid)) {
                InvalidateService();
                sleep(1);
                continue;
            }
            int64_t lastEvent = BootNs();
            bool broken = false;
            while (!stop && !broken) {
                if (s->reconfig.exchange(false)) {
                    Configure(c, s, suid);
                }
                const uint8_t *pb = nullptr;
                size_t pbLen = 0;
                uint16_t err = 0;
                int r = c.Recv(buf, pb, pbLen, err);
                if (r < 0) {
                    broken = true;
                } else if (r == 1) {
                    lastEvent = BootNs();
                    HandleEvents(s, pb, pbLen);
                } else if (s->def->kind != ON_CHANGE && BootNs() - lastEvent > 10000000000LL) {
                    /* a continuous sensor silent for 10 s: the ADSP probably restarted */
                    LOGI("%s: no data for 10 s, reconnecting", s->def->name);
                    InvalidateService();
                    broken = true;
                }
            }
            if (!broken) {
                c.Send(EncodeRequest(suid, MSG_CLIENT_DISABLE, ""));
            }
        }
    }

    bool Configure(SnsClient &c, Stream *s, const Suid &suid)
    {
        bool ok;
        if (s->def->kind == ON_CHANGE) {
            ok = c.Send(EncodeRequest(suid, MSG_ON_CHANGE_CONFIG, ""));
        } else {
            PbOut cfg;
            cfg.Float(1, s->rateHz);
            ok = c.Send(EncodeRequest(suid, MSG_SENSOR_CONFIG, cfg.b));
        }
        if (!ok) {
            LOGE("%s: config request failed: %s", s->def->name, strerror(errno));
        }
        return ok;
    }

    void HandleEvents(Stream *s, const uint8_t *pb, size_t len)
    {
        PbIn msg(pb, len);
        PbField f;
        while (msg.Next(f)) {
            if (f.num != 2 || f.wt != 2) continue;
            PbIn ev(f.p, f.len);
            PbField g;
            uint32_t id = 0;
            const uint8_t *pl = nullptr;
            size_t pll = 0;
            while (ev.Next(g)) {
                if (g.num == 1) id = uint32_t(g.v);
                if (g.num == 3 && g.wt == 2) { pl = g.p; pll = g.len; }
            }
            if (id == MSG_ERROR_EVENT) {
                LOGE("%s: SSC error event", s->def->name);
                continue;
            }
            if (id == MSG_PROXIMITY_EVENT && pl && s->def->type == 12) {
                PbIn pe(pl, pll);
                PbField p;
                while (pe.Next(p)) {
                    if (p.num == 1 && p.wt == 0 && p.v <= 1) {
                        float dist = p.v == 1 ? 0.f : 5.f; /* HarmonyOS: 0 is near */
                        LOGI("proximity %s", p.v == 1 ? "near" : "far");
                        Deliver(s, &dist, 1);
                    }
                }
                continue;
            }
            if (id != MSG_SENSOR_EVENT || !pl) continue;            /* sns_std_sensor_event { data=1 (repeated float, packed or not), status=2 } */
            float v[8] = {};
            int n = 0;
            PbIn se(pl, pll);
            PbField h;
            while (se.Next(h)) {
                if (h.num != 1) continue;
                if (h.wt == 5 && n < 8) {
                    uint32_t u = uint32_t(h.v);
                    memcpy(&v[n++], &u, 4);
                } else if (h.wt == 2) {
                    for (size_t k = 0; k + 4 <= h.len && n < 8; k += 4) {
                        memcpy(&v[n++], h.p + k, 4);
                    }
                }
            }
            if (n == 0) continue;
            Deliver(s, v, n);
        }
    }

    void Deliver(Stream *s, const float *v, int n)
    {
        int64_t ts = BootNs();
        const SensorDef &d = *s->def;
        float out[4] = {};
        for (int i = 0; i < d.floats && i < n; i++) {
            out[i] = v[i];
        }
        if (d.type == 5) {           /* ambient light: lux, colour temperature, infrared */
            out[1] = 0.f;
            out[2] = 0.f;
        }
        if (s->enabled) {
            Send(d.type, d.kind == ON_CHANGE ? 2 : 1, ts, out, d.floats);
        }
        if (d.type == 1 && gravityFromAccel_ && n >= 3) {
            /* low-pass filter: what is left of the acceleration when the phone
             * is not shaken is gravity */
            const float alpha = 0.85f;
            for (int i = 0; i < 3; i++) {
                grav_[i] = gravInit_ ? alpha * grav_[i] + (1.f - alpha) * v[i] : v[i];
            }
            gravInit_ = true;
            Send(257, 1, ts, grav_, 3);
        }
    }

    void Send(int32_t type, int32_t mode, int64_t ts, const float *vals, int count)
    {
        HdfSensorEventsVdi e;
        e.sensorId = type;
        e.version = 1;
        e.timestamp = ts;
        e.option = 0;
        e.mode = mode;
        e.dataLen = uint32_t(count * sizeof(float));
        const uint8_t *b = reinterpret_cast<const uint8_t *>(vals);
        e.data.assign(b, b + e.dataLen);
        std::vector<sptr<ISensorCallbackVdi>> cbs;
        {
            std::lock_guard<std::mutex> l(cbMu_);
            cbs = cbs_[0]; /* traditional sensors group */
        }
        for (auto &cb : cbs) {
            if (cb != nullptr) {
                cb->OnDataEventVdi(e);
            }
        }
    }

    std::mutex mu_;
    std::condition_variable discoverCv_;
    bool discovered_ = false;
    std::map<int32_t, Stream *> streams_;
    std::mutex cbMu_;
    std::map<int32_t, std::vector<sptr<ISensorCallbackVdi>>> cbs_;
    std::mutex svcMu_;
    bool svcValid_ = false;
    uint32_t svcNode_ = 0;
    uint32_t svcPort_ = 0;
    std::atomic<bool> gravityFromAccel_{false};
    float grav_[3] = {};
    bool gravInit_ = false;
};

int32_t CreateVdi(struct HdfVdiBase *base)
{
    if (base == nullptr) {
        return HDF_FAILURE;
    }
    auto *w = reinterpret_cast<WrapperSensorVdi *>(base);
    w->sensorModule = new SweetSensors();
    return HDF_SUCCESS;
}

int32_t DestroyVdi(struct HdfVdiBase *base)
{
    if (base == nullptr) {
        return HDF_FAILURE;
    }
    auto *w = reinterpret_cast<WrapperSensorVdi *>(base);
    delete static_cast<SweetSensors *>(w->sensorModule);
    w->sensorModule = nullptr;
    return HDF_SUCCESS;
}

struct WrapperSensorVdi g_sensorVdi = {
    .base = {
        .moduleVersion = 1,
        .moduleName = "sensor_Service",
        .CreateVdiInstance = CreateVdi,
        .DestoryVdiInstance = DestroyVdi,
    },
    .sensorModule = nullptr,
};
} // namespace

extern "C" __attribute__((visibility("default"))) struct HdfVdiBase *hdfVdiDesc =
    reinterpret_cast<struct HdfVdiBase *>(&g_sensorVdi);
