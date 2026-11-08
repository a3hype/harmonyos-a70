/*
 * One-shot check: brings the chip up through the vendor library, sends Reset, then one Remote
 * Name Request, prints the Command Status byte, and powers down. Exit code is that status.
 * 0x00 means the controller accepts paging, 0x09 is the failure being hunted.
 * Usage: a70-hci-probe AA:BB:CC:DD:EE:FF
 */
#include <dlfcn.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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

static int rd(int fd, uint8_t *p, size_t n, int ms)
{
    while (n) {
        struct pollfd pf = {fd, POLLIN, 0};
        if (poll(&pf, 1, ms) <= 0)
            return -1;
        ssize_t g = read(fd, p, n);
        if (g <= 0)
            return -1;
        p += g;
        n -= (size_t)g;
    }
    return 0;
}

/* returns status of the matching CC/CS, or -1 */
static int cmd(int fd, uint16_t op, const uint8_t *p, uint8_t n)
{
    uint8_t b[260] = {1, (uint8_t)op, (uint8_t)(op >> 8), n};
    if (n)
        memcpy(b + 4, p, n);
    if (write(fd, b, 4 + (size_t)n) < 0)
        return -1;
    for (int i = 0; i < 12; i++) {
        uint8_t h[3], d[260];
        if (rd(fd, h, 1, 1500) < 0)
            return -1;
        if (h[0] != 4)
            continue;
        if (rd(fd, h + 1, 2, 500) < 0 || rd(fd, d, h[2], 500) < 0)
            return -1;
        if (h[1] == 0x0E && (d[1] | d[2] << 8) == op)
            return d[3];
        if (h[1] == 0x0F && (d[2] | d[3] << 8) == op)
            return d[0];
    }
    return -1;
}

int main(int argc, char **argv)
{
    unsigned a[6];
    if (argc < 2 || sscanf(argv[1], "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6)
        return 200;
    uint8_t q[10];
    for (int i = 0; i < 6; i++)
        q[i] = (uint8_t)a[5 - i];
    q[6] = 1; q[7] = 0; q[8] = 0; q[9] = 0;
    void *h = dlopen("/vendor/lib64/libbt_vendor.z.so", RTLD_NOW);
    if (!h)
        return 201;
    const Vi *vi = dlsym(h, "BLUETOOTH_VENDOR_LIB_INTERFACE");
    if (!vi)
        return 202;
    static const Cb cb = {sizeof(Cb), NULL, NULL, NULL, NULL};
    unsigned char zero[6] = {0};
    int fds[4] = {-1, -1, -1, -1};
    vi->init(&cb, zero);
    vi->op(0, NULL);
    int r = vi->op(2, fds);
    int st = 203;
    if (r >= 1 && fds[0] >= 0) {
        int rs = cmd(fds[0], 0x0C03, NULL, 0);
        st = cmd(fds[0], 0x0419, q, 10);
        printf("reset %d rnr %d\n", rs, st);
        /* cancel so the next power cycle starts clean */
        cmd(fds[0], 0x041A, q, 6);
        vi->op(3, NULL);
    } else {
        printf("setup failed r=%d\n", r);
    }
    vi->op(1, NULL);
    vi->close();
    return st < 0 ? 204 : st;
}
