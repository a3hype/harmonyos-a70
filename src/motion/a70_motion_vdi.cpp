/*
 * Motion VDI for HarmonyOS on the Samsung Galaxy A70: replaces Huawei's /dev/motionhub library.
 * Only screen rotation is implemented, worked out from the SSC gravity sensor. Every request is logged.
 * SSC message layout follows hmos2sweet src/sensors/sweet_sensors_vdi.cpp (Apache-2.0, commit 1be080c).
 * SPDX-License-Identifier: Apache-2.0
 */
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "imotion_interface_vdi.h"

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

extern "C" int HiLogPrint(int type, int level, unsigned int domain, const char *tag, const char *fmt, ...);

using namespace OHOS;
using namespace OHOS::HDI::Motion::V1_1;

namespace {
constexpr int32_t TYPE_ROTATION = 4; /* HDF_MOTION_TYPE_ROTATION, MotionTypes.idl v1.1 */

void Log(int level, const char *fmt, ...)
{
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    HiLogPrint(3, level, 0xD002516, "A70_MOTION", "%{public}s", buf);
}
#define LOGI(...) Log(4, __VA_ARGS__)
#define LOGE(...) Log(6, __VA_ARGS__)

int64_t NowMs()
{
    struct timespec t;
    clock_gettime(CLOCK_BOOTTIME, &t);
    return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

struct Writer {
    std::string b;
    void Var(uint64_t v)
    {
        do {
            uint8_t c = v & 0x7f;
            v >>= 7;
            b.push_back(char(v ? (c | 0x80) : c));
        } while (v);
    }
    void Key(int f, int wt) { Var((uint64_t(f) << 3) | wt); }
    void U64(int f, uint64_t v) { Key(f, 0); Var(v); }
    void F32(int f, uint32_t v) { Key(f, 5); b.append(reinterpret_cast<const char *>(&v), 4); }
    void F64(int f, uint64_t v) { Key(f, 1); b.append(reinterpret_cast<const char *>(&v), 8); }
    void Bytes(int f, const std::string &s) { Key(f, 2); Var(s.size()); b += s; }
};

struct Field {
    int num = 0;
    int wt = 0;
    uint64_t v = 0;
    const uint8_t *p = nullptr;
    size_t len = 0;
};

class Reader {
public:
    Reader(const uint8_t *p, size_t n) : p_(p), e_(p + n) {}
    bool Next(Field &f)
    {
        uint64_t key;
        if (p_ >= e_ || !Var(key)) return false;
        f.num = int(key >> 3);
        f.wt = int(key & 7);
        f.p = nullptr;
        f.len = 0;
        f.v = 0;
        switch (f.wt) {
            case 0: return Var(f.v);
            case 1: if (e_ - p_ < 8) return false; memcpy(&f.v, p_, 8); p_ += 8; return true;
            case 5: { if (e_ - p_ < 4) return false; uint32_t u; memcpy(&u, p_, 4); f.v = u; p_ += 4; return true; }
            case 2: {
                uint64_t n;
                if (!Var(n) || uint64_t(e_ - p_) < n) return false;
                f.p = p_; f.len = size_t(n); p_ += n;
                return true;
            }
            default: return false;
        }
    }
private:
    bool Var(uint64_t &v)
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

struct Suid { uint64_t lo = 0, hi = 0; bool Valid() const { return lo || hi; } };
struct QAddr { unsigned short fam; uint32_t node; uint32_t port; };

constexpr uint32_t MSG_DISABLE = 10, MSG_SUID_REQ = 512, MSG_CONFIG = 513, MSG_SUID_EVENT = 768, MSG_EVENT = 1025;

bool FindService(uint32_t &node, uint32_t &port)
{
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    QAddr me = {};
    socklen_t l = sizeof(me);
    bool found = false;
    if (fd < 0) return false;
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&me), &l) == 0) {
        uint32_t pkt[5] = {10, 400, 0, 0, 0};
        QAddr ns = {AF_QIPCRTR, me.node, 0xfffffffeu};
        struct timeval tv = {1, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (sendto(fd, pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr *>(&ns), sizeof(ns)) >= 0) {
            for (int i = 0; i < 20 && !found; i++) {
                uint32_t r[8] = {};
                ssize_t n = recv(fd, r, sizeof(r), 0);
                if (n < 0) break;
                if (n >= 20 && r[0] == 4 && r[1] == 400) { node = r[3]; port = r[4]; found = true; }
            }
        }
    }
    close(fd);
    return found;
}

class Client {
public:
    ~Client() { if (fd_ >= 0) close(fd_); }
    bool Open(uint32_t node, uint32_t port)
    {
        fd_ = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return false;
        struct timeval tv = {0, 300000};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        node_ = node;
        port_ = port;
        return true;
    }
    bool Send(const Suid &s, uint32_t msg, const std::string &payload)
    {
        Writer su, susp, req, m;
        su.F64(1, s.lo);
        su.F64(2, s.hi);
        susp.U64(1, 1);
        susp.U64(2, 1);
        if (!payload.empty()) req.Bytes(2, payload);
        m.Bytes(1, su.b);
        m.F32(2, msg);
        m.Bytes(3, susp.b);
        m.Bytes(4, req.b);
        std::vector<uint8_t> o(7);
        size_t n = m.b.size();
        o.push_back(1);
        o.push_back(uint8_t((n + 2) & 0xff));
        o.push_back(uint8_t((n + 2) >> 8));
        o.push_back(uint8_t(n & 0xff));
        o.push_back(uint8_t(n >> 8));
        o.insert(o.end(), m.b.begin(), m.b.end());
        o.push_back(0x10); o.push_back(1); o.push_back(0); o.push_back(1);
        uint16_t txn = ++txn_, len = uint16_t(o.size() - 7);
        o[0] = 0; o[1] = uint8_t(txn); o[2] = uint8_t(txn >> 8); o[3] = 0x20; o[4] = 0;
        o[5] = uint8_t(len); o[6] = uint8_t(len >> 8);
        QAddr to = {AF_QIPCRTR, node_, port_};
        return sendto(fd_, o.data(), o.size(), 0, reinterpret_cast<sockaddr *>(&to), sizeof(to)) == ssize_t(o.size());
    }
    /* 1 = indication payload, 0 = nothing yet, -1 = failure */
    int Recv(std::vector<uint8_t> &b, const uint8_t *&pb, size_t &pl)
    {
        b.resize(65536);
        ssize_t n = recv(fd_, b.data(), b.size(), 0);
        if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
        if (n < 7 || b[0] != 4) return 0;
        uint16_t msg = uint16_t(b[3] | (b[4] << 8));
        size_t end = std::min<size_t>(size_t(n), 7u + uint16_t(b[5] | (b[6] << 8))), i = 7;
        while (i + 3 <= end) {
            uint8_t t = b[i];
            uint16_t l = uint16_t(b[i + 1] | (b[i + 2] << 8));
            const uint8_t *v = &b[i + 3];
            if (i + 3 + l > end) break;
            if ((msg == 0x21 || msg == 0x22) && t == 2 && l >= 2) {
                size_t p = size_t(v[0] | (v[1] << 8));
                if (p + 2 <= l) { pb = v + 2; pl = p; return 1; }
            }
            i += 3 + l;
        }
        return 0;
    }
private:
    int fd_ = -1;
    uint32_t node_ = 0, port_ = 0;
    uint16_t txn_ = 0;
};

Suid LookupGravity(uint32_t node, uint32_t port)
{
    Client c;
    if (!c.Open(node, port)) return {};
    Writer req;
    req.Bytes(1, "gravity");
    req.U64(2, 0);
    req.U64(3, 1);
    Suid look;
    look.lo = look.hi = 0xababababababababULL;
    if (!c.Send(look, MSG_SUID_REQ, req.b)) return {};
    std::vector<uint8_t> b;
    int64_t end = NowMs() + 3000;
    while (NowMs() < end) {
        const uint8_t *pb = nullptr;
        size_t pl = 0;
        int r = c.Recv(b, pb, pl);
        if (r < 0) break;
        if (r != 1) continue;
        Reader m(pb, pl);
        Field f;
        while (m.Next(f)) {
            if (f.num != 2 || f.wt != 2) continue;
            Reader ev(f.p, f.len);
            Field g;
            uint32_t id = 0;
            const uint8_t *pay = nullptr;
            size_t payl = 0;
            while (ev.Next(g)) {
                if (g.num == 1) id = uint32_t(g.v);
                if (g.num == 3 && g.wt == 2) { pay = g.p; payl = g.len; }
            }
            if (id != MSG_SUID_EVENT || !pay) continue;
            Reader se(pay, payl);
            Field h;
            while (se.Next(h)) {
                if (h.num == 2 && h.wt == 2) {
                    Reader sr(h.p, h.len);
                    Field k;
                    Suid s;
                    while (sr.Next(k)) {
                        if (k.num == 1) s.lo = k.v;
                        if (k.num == 2) s.hi = k.v;
                    }
                    return s;
                }
            }
            return {};
        }
    }
    return {};
}

class A70Motion : public IMotionInterfaceVdi {
public:
    ~A70Motion() override { StopRotation(); }

    int32_t InitMotion() override
    {
        LOGI("InitMotion");
        return HDF_SUCCESS;
    }

    int32_t EnableMotion(int32_t motionType) override
    {
        LOGI("EnableMotion type=%d", motionType);
        if (motionType != TYPE_ROTATION) {
            return HDF_ERR_NOT_SUPPORT;
        }
        StartRotation();
        return HDF_SUCCESS;
    }

    int32_t DisableMotion(int32_t motionType) override
    {
        LOGI("DisableMotion type=%d", motionType);
        if (motionType != TYPE_ROTATION) {
            return HDF_ERR_NOT_SUPPORT;
        }
        StopRotation();
        return HDF_SUCCESS;
    }

    int32_t RegisterMotionCallback(const sptr<IMotionCallbackVdi> cb) override
    {
        std::lock_guard<std::mutex> l(cbMu_);
        if (cb == nullptr) return HDF_ERR_INVALID_PARAM;
        for (auto &c : cbs_) {
            if (c == cb) return HDF_SUCCESS;
        }
        cbs_.push_back(cb);
        LOGI("RegisterMotionCallback, now %zu", cbs_.size());
        lastSent_ = -1; /* a new listener needs to hear the current state */
        return HDF_SUCCESS;
    }

    int32_t UnregisterMotionCallback(const sptr<IMotionCallbackVdi> cb) override
    {
        std::lock_guard<std::mutex> l(cbMu_);
        for (auto it = cbs_.begin(); it != cbs_.end(); ++it) {
            if (*it == cb) {
                cbs_.erase(it);
                break;
            }
        }
        LOGI("UnregisterMotionCallback, now %zu", cbs_.size());
        return HDF_SUCCESS;
    }

    int32_t SetMotionConfig(int32_t motionType, const std::vector<uint8_t> &data) override
    {
        LOGI("SetMotionConfig type=%d len=%zu", motionType, data.size());
        return HDF_SUCCESS;
    }

private:
    void StartRotation()
    {
        std::lock_guard<std::mutex> l(mu_);
        if (stop_) return;
        auto stop = std::make_shared<std::atomic<bool>>(false);
        stop_ = stop;
        std::thread([this, stop] { Run(*stop); }).detach();
    }

    void StopRotation()
    {
        std::lock_guard<std::mutex> l(mu_);
        if (stop_) {
            *stop_ = true;
            stop_.reset();
        }
    }

    void Report(int32_t rotation)
    {
        HdfMotionEventVdi e;
        e.motion = TYPE_ROTATION;
        e.result = 0;
        e.status = 0;
        e.datalen = 1;
        e.data.push_back(rotation);
        std::vector<sptr<IMotionCallbackVdi>> cbs;
        {
            std::lock_guard<std::mutex> l(cbMu_);
            cbs = cbs_;
            lastSent_ = rotation;
        }
        LOGI("rotation=%d to %zu listeners", rotation, cbs.size());
        for (auto &c : cbs) {
            if (c != nullptr) c->OnDataEventVdi(e);
        }
    }

    /* x > 0 when the left edge is down, y > 0 when the top is up. Needs a clear tilt, with hysteresis
     * from holding the old value until another side wins. Flat on a table keeps the last value. */
    int32_t Classify(float x, float y, int32_t cur) const
    {
        const float strong = 6.0f;
        if (std::fabs(y) >= std::fabs(x)) {
            if (y > strong) return 0;
            if (y < -strong) return 2;
        } else {
            if (x > strong) return 1;
            if (x < -strong) return 3;
        }
        return cur;
    }

    void Run(std::atomic<bool> &stop)
    {
        LOGI("rotation thread start");
        std::vector<uint8_t> b;
        int32_t cur = 0, pending = -1;
        int64_t since = 0;
        while (!stop) {
            uint32_t node = 0, port = 0;
            if (!FindService(node, port)) {
                sleep(2);
                continue;
            }
            Suid s = LookupGravity(node, port);
            if (!s.Valid()) {
                sleep(3);
                continue;
            }
            Client c;
            Writer cfg;
            float rate = 10.f;
            uint32_t u;
            memcpy(&u, &rate, 4);
            cfg.F32(1, u);
            if (!c.Open(node, port) || !c.Send(s, MSG_CONFIG, cfg.b)) {
                sleep(1);
                continue;
            }
            LOGI("gravity stream on");
            int64_t last = NowMs();
            bool broken = false;
            while (!stop && !broken) {
                const uint8_t *pb = nullptr;
                size_t pl = 0;
                int r = c.Recv(b, pb, pl);
                if (r < 0) {
                    broken = true;
                } else if (r == 0) {
                    if (NowMs() - last > 10000) {
                        LOGI("gravity silent for 10 s, reconnecting");
                        broken = true;
                    }
                    continue;
                }
                if (broken) break;
                last = NowMs();
                Reader m(pb, pl);
                Field f;
                while (m.Next(f)) {
                    if (f.num != 2 || f.wt != 2) continue;
                    Reader ev(f.p, f.len);
                    Field g;
                    uint32_t id = 0;
                    const uint8_t *pay = nullptr;
                    size_t payl = 0;
                    while (ev.Next(g)) {
                        if (g.num == 1) id = uint32_t(g.v);
                        if (g.num == 3 && g.wt == 2) { pay = g.p; payl = g.len; }
                    }
                    if (id != MSG_EVENT || !pay) continue;
                    float v[3] = {};
                    int n = 0;
                    Reader se(pay, payl);
                    Field h;
                    while (se.Next(h)) {
                        if (h.num != 1) continue;
                        if (h.wt == 5 && n < 3) {
                            uint32_t w = uint32_t(h.v);
                            memcpy(&v[n++], &w, 4);
                        } else if (h.wt == 2) {
                            for (size_t o = 0; o + 4 <= h.len && n < 3; o += 4) memcpy(&v[n++], h.p + o, 4);
                        }
                    }
                    if (n < 2) continue;
                    int32_t want = Classify(v[0], v[1], cur);
                    int64_t now = NowMs();
                    if (want != cur) {
                        if (want != pending) {
                            pending = want;
                            since = now;
                        } else if (now - since >= 300) {
                            cur = want;
                            pending = -1;
                            Report(cur);
                        }
                    } else {
                        pending = -1;
                        int32_t sent;
                        {
                            std::lock_guard<std::mutex> l(cbMu_);
                            sent = lastSent_;
                        }
                        if (sent != cur) Report(cur);
                    }
                }
            }
            if (!broken) c.Send(s, MSG_DISABLE, "");
        }
        LOGI("rotation thread end");
    }

    std::mutex mu_;
    std::shared_ptr<std::atomic<bool>> stop_;
    std::mutex cbMu_;
    std::vector<sptr<IMotionCallbackVdi>> cbs_;
    int32_t lastSent_ = -1;
};

int32_t CreateVdi(struct HdfVdiBase *base)
{
    if (base == nullptr) return HDF_FAILURE;
    reinterpret_cast<WrapperMotionVdi *>(base)->motionModule = new A70Motion();
    return HDF_SUCCESS;
}

int32_t DestroyVdi(struct HdfVdiBase *base)
{
    if (base == nullptr) return HDF_FAILURE;
    auto *w = reinterpret_cast<WrapperMotionVdi *>(base);
    delete static_cast<A70Motion *>(w->motionModule);
    w->motionModule = nullptr;
    return HDF_SUCCESS;
}

struct WrapperMotionVdi g_motionVdi = {
    .base = {
        .moduleVersion = 1,
        .moduleName = "motion_service",
        .CreateVdiInstance = CreateVdi,
        .DestoryVdiInstance = DestroyVdi,
    },
    .motionModule = nullptr,
};
} // namespace

namespace OHOS { namespace HDI { namespace Motion { namespace V1_1 {
int32_t IMotionInterfaceVdi::InitMotion() { return HDF_SUCCESS; }
} } } }

extern "C" __attribute__((visibility("default"))) struct HdfVdiBase *hdfVdiDesc =
    reinterpret_cast<struct HdfVdiBase *>(&g_motionVdi);
