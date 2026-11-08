/*
 * Bare HCI test, with the Bluetooth stack stopped: brings the chip up through the vendor
 * library, sends Reset, then Remote Name Request and Create Connection to one address,
 * and prints the controller's answers. Disconnects again. Sends no pairing data.
 * Usage: a70-hci-test AA:BB:CC:DD:EE:FF
 */
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    size_t size;
    void *initCb;
    void *(*alloc)(int size);
    void (*dealloc)(void *buf);
    size_t (*xmitCb)(uint16_t opcode, void *pBuf);
} Cb;
typedef struct {
    size_t size;
    int (*init)(const Cb *cb, unsigned char *addr);
    int (*op)(int opcode, void *param);
    void (*close)(void);
} Vi;

static int g_fd = -1;

static void send_cmd(uint16_t op, const uint8_t *p, uint8_t n)
{
    uint8_t b[260] = {1, (uint8_t)op, (uint8_t)(op >> 8), n};
    if (n)
        memcpy(b + 4, p, n);
    if (write(g_fd, b, 4 + (size_t)n) < 0)
        printf("write: %s\n", strerror(errno));
}

static int rd(uint8_t *p, size_t n, int ms)
{
    while (n) {
        struct pollfd pf = {g_fd, POLLIN, 0};
        if (poll(&pf, 1, ms) <= 0)
            return -1;
        ssize_t g = read(g_fd, p, n);
        if (g <= 0)
            return -1;
        p += g;
        n -= (size_t)g;
    }
    return 0;
}

static uint16_t g_handle = 0xffff;
static int g_done_rnr, g_done_conn;

/* prints events until timeout; 0 on timeout */
static void pump(int ms, int stop_on)
{
    struct timespec s, n;
    clock_gettime(CLOCK_MONOTONIC, &s);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &n);
        int left = ms - (int)((n.tv_sec - s.tv_sec) * 1000 + (n.tv_nsec - s.tv_nsec) / 1000000);
        if (left <= 0)
            return;
        uint8_t h[3], d[260];
        if (rd(h, 1, left) < 0)
            return;
        if (h[0] != 4) {
            printf("  packet type %u skipped, stream may be off\n", h[0]);
            continue;
        }
        if (rd(h + 1, 2, 1000) < 0 || rd(d, h[2], 1000) < 0)
            return;
        switch (h[1]) {
            case 0x0E:
                printf("  CommandComplete op %02x%02x status 0x%02x\n", d[2], d[1], d[3]);
                if (stop_on == 0x0E)
                    return;
                break;
            case 0x0F:
                printf("  CommandStatus op %02x%02x status 0x%02x\n", d[3], d[2], d[0]);
                if (stop_on == 0x0F && d[0])
                    return;
                break;
            case 0x03:
                g_handle = (uint16_t)(d[1] | d[2] << 8);
                printf("  ConnectionComplete status 0x%02x handle %u\n", d[0], g_handle);
                g_done_conn = 1;
                if (stop_on == 0x03)
                    return;
                break;
            case 0x07:
                printf("  RemoteNameComplete status 0x%02x name [%s]\n", d[0], d[0] ? "" : (char *)d + 7);
                g_done_rnr = 1;
                if (stop_on == 0x07)
                    return;
                break;
            case 0x05:
                printf("  DisconnectionComplete status 0x%02x reason 0x%02x\n", d[0], d[3]);
                if (stop_on == 0x05)
                    return;
                break;
            default:
                printf("  event 0x%02x len %u\n", h[1], h[2]);
        }
    }
}

int main(int argc, char **argv)
{
    unsigned a[6];
    if (argc < 2 || sscanf(argv[1], "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) {
        printf("usage: %s AA:BB:CC:DD:EE:FF\n", argv[0]);
        return 2;
    }
    uint8_t bd[6];
    for (int i = 0; i < 6; i++)
        bd[i] = (uint8_t)a[5 - i];

    void *h = dlopen("/vendor/lib64/libbt_vendor.z.so", RTLD_NOW);
    if (!h) {
        printf("dlopen: %s\n", dlerror());
        return 1;
    }
    const Vi *vi = dlsym(h, "BLUETOOTH_VENDOR_LIB_INTERFACE");
    if (!vi) {
        printf("no interface\n");
        return 1;
    }
    static const Cb cb = {sizeof(Cb), NULL, NULL, NULL, NULL};
    unsigned char zero[6] = {0};
    int fds[4] = {-1, -1, -1, -1};
    printf("init %d\n", vi->init(&cb, zero));
    printf("power on %d\n", vi->op(0, NULL));
    int r = vi->op(2, fds);
    printf("channel open %d fd %d\n", r, fds[0]);
    if (r < 1 || fds[0] < 0)
        return 1;
    g_fd = fds[0];

    printf("Reset\n");
    send_cmd(0x0C03, NULL, 0);
    pump(1500, 0x0E);

    printf("Set event mask\n");
    static const uint8_t mask[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xbf, 0x3d};
    send_cmd(0x0C01, mask, 8);
    pump(1000, 0x0E);

    printf("Remote Name Request\n");
    uint8_t rnr[10];
    memcpy(rnr, bd, 6);
    rnr[6] = 1; rnr[7] = 0; rnr[8] = 0; rnr[9] = 0;
    send_cmd(0x0419, rnr, 10);
    pump(12000, 0x07);

    printf("Create Connection\n");
    uint8_t cc[13];
    memcpy(cc, bd, 6);
    cc[6] = 0x18; cc[7] = 0xCC; /* DM1, DH1, DM3, DH3, DM5, DH5 */
    cc[8] = 1; cc[9] = 0; cc[10] = 0; cc[11] = 0; cc[12] = 1;
    send_cmd(0x0405, cc, 13);
    pump(12000, 0x03);

    if (g_handle != 0xffff) {
        printf("Disconnect\n");
        uint8_t dis[3] = {(uint8_t)g_handle, (uint8_t)(g_handle >> 8), 0x13};
        send_cmd(0x0406, dis, 3);
        pump(3000, 0x05);
    }
    vi->op(3, NULL);
    vi->op(1, NULL);
    vi->close();
    printf("done rnr=%d conn=%d\n", g_done_rnr, g_done_conn);
    return 0;
}
