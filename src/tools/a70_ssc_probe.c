// Asks the Snapdragon Sensor Core what it has and reads a sensor for a few seconds. Writes nothing.
// Message layout follows hmos2sweet src/sensors/sweet_sensors_vdi.cpp (Apache-2.0, commit 1be080c).
// Usage: a70_ssc_probe list | a70_ssc_probe read TYPE SECONDS [cont|change]
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif
#define QRTR_PORT_CTRL 0xfffffffeu
#define SNS_SERVICE 400
#define MSG_DISABLE 10
#define MSG_SUID_REQ 512
#define MSG_CONFIG 513
#define MSG_ONCHANGE 514
#define MSG_SUID_EVENT 768
#define MSG_SENSOR_EVENT 1025
#define MSG_ERROR_EVENT 130

struct qaddr { unsigned short fam; uint32_t node, port; };
struct buf { uint8_t d[1200]; size_t n; };
struct suid { uint64_t lo, hi; };

static uint32_t g_node, g_port;
static uint16_t g_txn;

static void b_var(struct buf *b, uint64_t v) {
    do {
        uint8_t c = v & 0x7f;
        v >>= 7;
        b->d[b->n++] = v ? (c | 0x80) : c;
    } while (v);
}
static void b_key(struct buf *b, int f, int wt) { b_var(b, ((uint64_t)f << 3) | wt); }
static void b_u64(struct buf *b, int f, uint64_t v) { b_key(b, f, 0); b_var(b, v); }
static void b_raw(struct buf *b, const void *p, size_t n) { memcpy(b->d + b->n, p, n); b->n += n; }
static void b_f32(struct buf *b, int f, uint32_t v) { b_key(b, f, 5); b_raw(b, &v, 4); }
static void b_f64(struct buf *b, int f, uint64_t v) { b_key(b, f, 1); b_raw(b, &v, 8); }
static void b_bytes(struct buf *b, int f, const void *p, size_t n) { b_key(b, f, 2); b_var(b, n); b_raw(b, p, n); }

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_BOOTTIME, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int find_service(void) {
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
    struct qaddr me = {0}, ns = {0};
    socklen_t l = sizeof(me);
    uint32_t pkt[5] = {10, SNS_SERVICE, 0, 0, 0};
    struct timeval tv = {3, 0};
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&me, &l)) return -1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ns.fam = AF_QIPCRTR; ns.node = me.node; ns.port = QRTR_PORT_CTRL;
    if (sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&ns, sizeof(ns)) < 0) return -1;
    for (;;) {
        uint32_t r[8];
        ssize_t n = recv(fd, r, sizeof(r), 0);
        if (n < 0) break;
        if (n >= 20 && r[0] == 4 && r[1] == SNS_SERVICE) {
            g_node = r[3]; g_port = r[4];
            close(fd);
            return 0;
        }
    }
    close(fd);
    return -1;
}

static int client_open(void) {
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
    struct timeval tv = {0, 300000};
    if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static int send_req(int fd, struct suid s, uint32_t msg, const void *payload, size_t plen) {
    struct buf su = {.n = 0}, susp = {.n = 0}, req = {.n = 0}, m = {.n = 0};
    uint8_t out[1400];
    size_t i = 7;
    struct qaddr to = {AF_QIPCRTR, g_node, g_port};
    uint16_t txn = ++g_txn, len;
    b_f64(&su, 1, s.lo); b_f64(&su, 2, s.hi);
    b_u64(&susp, 1, 1); b_u64(&susp, 2, 1);
    if (plen) b_bytes(&req, 2, payload, plen);
    b_bytes(&m, 1, su.d, su.n);
    b_f32(&m, 2, msg);
    b_bytes(&m, 3, susp.d, susp.n);
    b_bytes(&m, 4, req.d, req.n);
    out[i++] = 1;
    out[i++] = (m.n + 2) & 0xff; out[i++] = (m.n + 2) >> 8;
    out[i++] = m.n & 0xff; out[i++] = m.n >> 8;
    memcpy(out + i, m.d, m.n); i += m.n;
    out[i++] = 0x10; out[i++] = 1; out[i++] = 0; out[i++] = 1;
    len = i - 7;
    out[0] = 0; out[1] = txn & 0xff; out[2] = txn >> 8; out[3] = 0x20; out[4] = 0;
    out[5] = len & 0xff; out[6] = len >> 8;
    return sendto(fd, out, i, 0, (struct sockaddr *)&to, sizeof(to)) == (ssize_t)i ? 0 : -1;
}

// 1 = indication in *pb, 2 = response (qmi error in *err), 0 = nothing, -1 = failure
static int recv_msg(int fd, uint8_t *b, size_t cap, const uint8_t **pb, size_t *pl, unsigned *err) {
    ssize_t n = recv(fd, b, cap, 0);
    size_t i = 7, end;
    uint16_t msg;
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    if (n < 7) return 0;
    msg = b[3] | (b[4] << 8);
    end = 7u + (size_t)(b[5] | (b[6] << 8));
    if (end > (size_t)n) end = n;
    *err = 0;
    while (i + 3 <= end) {
        uint8_t t = b[i];
        uint16_t l = b[i + 1] | (b[i + 2] << 8);
        const uint8_t *v = b + i + 3;
        if (i + 3 + l > end) break;
        if (b[0] == 2 && t == 2 && l >= 4) *err = v[2] | (v[3] << 8);
        if (b[0] == 4 && (msg == 0x21 || msg == 0x22) && t == 2 && l >= 2) {
            size_t p = v[0] | (v[1] << 8);
            if (p + 2 <= l) { *pb = v + 2; *pl = p; return 1; }
        }
        i += 3 + l;
    }
    return b[0] == 2 ? 2 : 0;
}

struct field { int num, wt; uint64_t v; const uint8_t *p; size_t len; };
static int pb_next(const uint8_t **cur, const uint8_t *e, struct field *f) {
    const uint8_t *p = *cur;
    uint64_t key = 0, n = 0;
    int s;
    if (p >= e) return 0;
    for (s = 0; p < e; s += 7) { uint8_t c = *p++; key |= (uint64_t)(c & 0x7f) << s; if (!(c & 0x80)) break; }
    f->num = key >> 3; f->wt = key & 7; f->p = NULL; f->len = 0; f->v = 0;
    switch (f->wt) {
        case 0:
            for (s = 0; p < e; s += 7) { uint8_t c = *p++; f->v |= (uint64_t)(c & 0x7f) << s; if (!(c & 0x80)) break; }
            break;
        case 1: if (e - p < 8) return 0; memcpy(&f->v, p, 8); p += 8; break;
        case 5: { uint32_t u; if (e - p < 4) return 0; memcpy(&u, p, 4); f->v = u; p += 4; break; }
        case 2:
            for (s = 0; p < e; s += 7) { uint8_t c = *p++; n |= (uint64_t)(c & 0x7f) << s; if (!(c & 0x80)) break; }
            if ((uint64_t)(e - p) < n) return 0;
            f->p = p; f->len = n; p += n;
            break;
        default: return 0;
    }
    *cur = p;
    return 1;
}

static const struct suid LOOKUP = {0xababababababababULL, 0xababababababababULL};
static uint8_t g_rx[65536];

// Number of SUIDs found for a data type, or -1 on no answer. First is stored in *out.
static int lookup(const char *type, struct suid *out) {
    int fd = client_open(), count = -1;
    struct buf req = {.n = 0};
    int64_t end = now_ms() + 3000;
    if (fd < 0) return -1;
    b_bytes(&req, 1, type, strlen(type));
    b_u64(&req, 2, 0);
    b_u64(&req, 3, 0); // all sensors of this type, not just the default
    if (send_req(fd, LOOKUP, MSG_SUID_REQ, req.d, req.n)) { close(fd); return -1; }
    while (now_ms() < end && count < 0) {
        const uint8_t *pb = NULL, *c, *e;
        size_t pl = 0;
        unsigned err = 0;
        struct field f;
        int r = recv_msg(fd, g_rx, sizeof(g_rx), &pb, &pl, &err);
        if (r < 0) break;
        if (r == 2 && err) { printf("  qmi error %u\n", err); break; }
        if (r != 1) continue;
        c = pb; e = pb + pl;
        while (pb_next(&c, e, &f)) {
            const uint8_t *ec, *ee, *pay = NULL;
            size_t paylen = 0;
            uint32_t id = 0;
            struct field g;
            if (f.num != 2 || f.wt != 2) continue;
            ec = f.p; ee = f.p + f.len;
            while (pb_next(&ec, ee, &g)) {
                if (g.num == 1) id = g.v;
                if (g.num == 3 && g.wt == 2) { pay = g.p; paylen = g.len; }
            }
            if (id != MSG_SUID_EVENT || !pay) continue;
            count = 0;
            ec = pay; ee = pay + paylen;
            while (pb_next(&ec, ee, &g)) {
                if (g.num == 2 && g.wt == 2) {
                    const uint8_t *sc = g.p, *se = g.p + g.len;
                    struct field h;
                    struct suid s = {0, 0};
                    while (pb_next(&sc, se, &h)) {
                        if (h.num == 1) s.lo = h.v;
                        if (h.num == 2) s.hi = h.v;
                    }
                    if (!count && out) *out = s;
                    count++;
                }
            }
        }
    }
    close(fd);
    return count;
}

static int cmd_list(void) {
    static const char *const types[] = {
        "accel", "gyro", "mag", "ambient_light", "proximity", "gravity", "rotv", "game_rv", "geomag_rv",
        "pressure", "humidity", "ambient_temperature", "rgb", "ultra_violet", "sar", "hall", "step_detect",
        "step_count", "sig_motion", "motion_detect", "amd", "smd", "pedometer", "orientation", "tilt",
        "stationary_detect", "linear_acceleration", "gyro_temperature", "accel_cal", "mag_cal", "gyro_cal",
        "device_orient", "wrist_tilt_gesture", "persist_stationary", "fmd", "prox_sar",
        "sec_prox", "sec_light", "sns_prox", "tof", "ppg", "offbody_detect"};
    size_t i;
    for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        struct suid s = {0, 0};
        int n = lookup(types[i], &s);
        if (n > 0) printf("%-24s %d  %016llx%016llx\n", types[i], n, (unsigned long long)s.hi, (unsigned long long)s.lo);
        else if (n == 0) printf("%-24s none\n", types[i]);
        else printf("%-24s no answer\n", types[i]);
    }
    return 0;
}

static int cmd_read(const char *type, int secs, int change) {
    struct suid s = {0, 0};
    int n = lookup(type, &s), fd, shown = 0, total = 0, errs = 0;
    int64_t end;
    struct buf cfg = {.n = 0};
    float rate = 10.f;
    uint32_t u;
    if (n <= 0) { printf("%s: not available (%d)\n", type, n); return 1; }
    fd = client_open();
    if (fd < 0) return 1;
    memcpy(&u, &rate, 4);
    b_f32(&cfg, 1, u);
    if (change ? send_req(fd, s, MSG_ONCHANGE, NULL, 0) : send_req(fd, s, MSG_CONFIG, cfg.d, cfg.n)) {
        printf("config send failed: %s\n", strerror(errno));
        return 1;
    }
    end = now_ms() + secs * 1000;
    while (now_ms() < end) {
        const uint8_t *pb = NULL, *c, *e;
        size_t pl = 0;
        unsigned err = 0;
        struct field f;
        int r = recv_msg(fd, g_rx, sizeof(g_rx), &pb, &pl, &err);
        if (r < 0) { printf("recv failed: %s\n", strerror(errno)); break; }
        if (r == 2) { printf("response qmi error=%u\n", err); continue; }
        if (r != 1) continue;
        c = pb; e = pb + pl;
        while (pb_next(&c, e, &f)) {
            const uint8_t *ec, *ee, *pay = NULL;
            size_t paylen = 0;
            uint32_t id = 0;
            struct field g;
            if (f.num != 2 || f.wt != 2) continue;
            ec = f.p; ee = f.p + f.len;
            while (pb_next(&ec, ee, &g)) {
                if (g.num == 1) id = g.v;
                if (g.num == 3 && g.wt == 2) { pay = g.p; paylen = g.len; }
            }
            if (id == MSG_ERROR_EVENT) { errs++; continue; }
            if (id != MSG_SENSOR_EVENT || !pay) {
                if (shown < 400) {
                    size_t o;
                    printf("%lld event id=%u len=%zu:", (long long)now_ms() % 100000, id, paylen);
                    if (id != MSG_SUID_EVENT && id != 128) {
                        for (o = 0; pay && o < paylen && o < 32; o++) printf(" %02x", pay[o]);
                    }
                    printf("\n");
                    shown++;
                }
                continue;
            }
            total++;
            if (shown < 12) {
                float v[8];
                int k = 0, i;
                int64_t t = now_ms();
                ec = pay; ee = pay + paylen;
                while (pb_next(&ec, ee, &g)) {
                    if (g.num != 1) continue;
                    if (g.wt == 5 && k < 8) { u = (uint32_t)g.v; memcpy(&v[k++], &u, 4); }
                    if (g.wt == 2) { size_t o; for (o = 0; o + 4 <= g.len && k < 8; o += 4) memcpy(&v[k++], g.p + o, 4); }
                }
                printf("%lld %s:", (long long)t % 100000, type);
                for (i = 0; i < k; i++) printf(" %.3f", v[i]);
                printf("\n");
                shown++;
            }
        }
    }
    printf("%s: %d events, %d error events\n", type, total, errs);
    send_req(fd, s, MSG_DISABLE, NULL, 0);
    usleep(200000);
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    if (find_service()) { printf("service 400 not found\n"); return 1; }
    printf("service 400 at %u:%u\n", g_node, g_port);
    if (argc >= 2 && !strcmp(argv[1], "list")) return cmd_list();
    if (argc >= 4 && !strcmp(argv[1], "read")) {
        return cmd_read(argv[2], atoi(argv[3]), argc >= 5 && !strcmp(argv[4], "change"));
    }
    printf("usage: list | read TYPE SECONDS [cont|change]\n");
    return 2;
}
