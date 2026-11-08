/*
 * libbt_vendor.z.so for HarmonyOS on the Galaxy A70.
 * Adapted from hmos2sweet src/bluetooth/sweet_bt_vendor.c (tsryzenstech/hmos2sweet).
 * Uses apbtfw11/apnv11 from /vendor/firmware (product 0x000a, rom 0x0201).
 *
 * The OpenHarmony Bluetooth HCI service loads this library and asks it to
 * power the controller on and hand over an H4 UART file descriptor. sweet has
 * a Qualcomm WCN3990 ("cherokee") on /dev/ttyHS0, powered through the
 * downstream btpower driver. Before it can be used the controller needs its
 * ROM patch (crbtfw<ver>.tlv) and NVM (crnv<ver>.bin) from the "bluetooth"
 * partition (mounted at /vendor/bt_firmware by sweet-rproc).
 *
 * The whole setup (power, version query, 3 Mbaud switch, patch and NVM
 * download, HCI reset) runs synchronously when the HCI channel is opened; the
 * host only starts reading the UART afterwards. Protocol as in Linux
 * drivers/bluetooth/btqca.c + hci_qca.c.
 */
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------ OHOS ABI */
typedef enum { HCI_CMD, HCI_EVT, HCI_ACL_OUT, HCI_ACL_IN, HCI_MAX_CHANNEL } hci_channels_t;
typedef enum { BTC_OP_RESULT_SUCCESS, BTC_OP_RESULT_FAIL } BtOpResultT;
typedef enum {
    BT_OP_POWER_ON, BT_OP_POWER_OFF, BT_OP_HCI_CHANNEL_OPEN, BT_OP_HCI_CHANNEL_CLOSE, BT_OP_INIT,
    BT_OP_GET_LPM_TIMER, BT_OP_LPM_ENABLE, BT_OP_LPM_DISABLE, BT_OP_WAKEUP_LOCK, BT_OP_WAKEUP_UNLOCK,
    BT_OP_EVENT_CALLBACK
} BtOpcodeT;
typedef void (*InitCallback)(BtOpResultT result);
typedef struct {
    size_t size;
    InitCallback initCb;
    void *(*alloc)(int size);
    void (*dealloc)(void *buf);
    size_t (*xmitCb)(uint16_t opcode, void *pBuf);
} BtVendorCallbacksT;
typedef struct {
    size_t size;
    int (*init)(const BtVendorCallbacksT *pCb, unsigned char *localBdaddr);
    int (*op)(BtOpcodeT opcode, void *param);
    void (*close)(void);
} BtVendorInterfaceT;

/* ----------------------------------------------------------- hardware */
#define UART_DEV "/dev/ttyHS0"
#define BTPOWER_DEV "/dev/btpower"
#define BT_CMD_PWR_CTRL 0xbfad
#define TIOCPMGET 0x544D
#define TIOCPMPUT 0x544E
static const char *FW_DIRS[] = {"/vendor/firmware", NULL};

#define EDL_PATCH_CMD_OPCODE 0xFC00
#define EDL_SET_BAUDRATE_CMD_OPCODE 0xFC48
#define EDL_WRITE_BD_ADDR_OPCODE 0xFC14
#define EDL_PATCH_VER_REQ_CMD 0x19
#define EDL_PATCH_TLV_REQ_CMD 0x1E
#define EDL_CMD_REQ_RES_EVT 0x00
#define EDL_TVL_DNLD_RES_EVT 0x04
#define QCA_BAUDRATE_3000000 0x0E
#define MAX_SEGMENT 243
#define HCI_RESET 0x0C03

static const BtVendorCallbacksT *g_cb;
static unsigned char g_bdaddr[6];
static int g_uart = -1;
static int g_power = -1;

/* --------------------------------------------------------------- log */
typedef int (*LogFn)(int, int, unsigned int, const char *, const char *, ...);
static void LOG(const char *fmt, ...)
{
    static LogFn fn = (LogFn)-1;
    char buf[300];
    va_list ap;
    if (fn == (LogFn)-1) {
        void *h = dlopen("libhilog.so", RTLD_NOW);
        fn = h ? (LogFn)dlsym(h, "HiLogPrint") : NULL;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (fn)
        fn(3, 4, 0xD000105, "A70_BT", "%{public}s", buf);
    int k = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (k >= 0) {
        char line[340];
        int n = snprintf(line, sizeof(line), "<6>a70-bt: %s\n", buf);
        if (write(k, line, n) < 0) {
        }
        close(k);
    }
}

/* -------------------------------------------------------------- UART */
static speed_t baud_const(int baud)
{
    switch (baud) {
        case 2400: return B2400;
        case 115200: return B115200;
        case 3000000: return B3000000;
        default: return B115200;
    }
}

static int uart_config(int fd, int baud, int flow)
{
    struct termios t;
    if (tcgetattr(fd, &t) < 0)
        return -1;
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    if (flow)
        t.c_cflag |= CRTSCTS;
    else
        t.c_cflag &= ~CRTSCTS;
    cfsetospeed(&t, baud_const(baud));
    cfsetispeed(&t, baud_const(baud));
    if (tcsetattr(fd, TCSANOW, &t) < 0)
        return -1;
    tcflush(fd, TCIOFLUSH);
    return 0;
}

static int write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int read_exact(int fd, uint8_t *p, size_t n, int timeout_ms)
{
    while (n) {
        struct pollfd pf = {fd, POLLIN, 0};
        int r = poll(&pf, 1, timeout_ms);
        if (r <= 0)
            return -1;
        ssize_t got = read(fd, p, n);
        if (got <= 0) {
            if (got < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            return -1;
        }
        p += got;
        n -= (size_t)got;
    }
    return 0;
}

static int send_cmd(int fd, uint16_t opcode, const uint8_t *param, uint8_t plen)
{
    uint8_t buf[4 + 255];
    buf[0] = 0x01; /* H4 command */
    buf[1] = opcode & 0xff;
    buf[2] = opcode >> 8;
    buf[3] = plen;
    if (plen)
        memcpy(buf + 4, param, plen);
    return write_all(fd, buf, 4 + (size_t)plen);
}

/* read one H4 event; returns the event code, payload in out (len in *len) */
static int read_event(int fd, uint8_t *out, int *len, int timeout_ms)
{
    uint8_t h[3];
    for (;;) {
        if (read_exact(fd, h, 1, timeout_ms) < 0)
            return -1;
        if (h[0] == 0x04)
            break; /* skip anything that is not an event packet */
    }
    if (read_exact(fd, h + 1, 2, timeout_ms) < 0)
        return -1;
    if (read_exact(fd, out, h[2], timeout_ms) < 0)
        return -1;
    *len = h[2];
    return h[1];
}

/* wait for the vendor (0xFF) and/or command-complete events of an EDL command */
static int wait_edl(int fd, int want_vse, int want_cc, uint8_t *vse, int *vse_len, int timeout_ms)
{
    uint8_t ev[260];
    int len;
    while (want_vse || want_cc) {
        int code = read_event(fd, ev, &len, timeout_ms);
        if (code < 0)
            return -1;
        if (code == 0xFF && want_vse) {
            if (vse) {
                memcpy(vse, ev, (size_t)len);
                *vse_len = len;
            }
            want_vse = 0;
        } else if (code == 0x0E && want_cc) {
            want_cc = 0;
        }
    }
    return 0;
}

/* ----------------------------------------------------- QCA sequence */
static int qca_read_version(int fd, uint32_t *soc_id, uint16_t *rom_ver)
{
    uint8_t cmd = EDL_PATCH_VER_REQ_CMD;
    uint8_t ev[260];
    int len;
    if (send_cmd(fd, EDL_PATCH_CMD_OPCODE, &cmd, 1) < 0)
        return -1;
    for (int tries = 0; tries < 4; tries++) {
        int code = read_event(fd, ev, &len, 1000);
        if (code < 0)
            return -1;
        uint8_t *d = NULL;
        if (code == 0xFF && len >= 2 + 12 && ev[0] == EDL_CMD_REQ_RES_EVT && (ev[1] == 0x02 || ev[1] == EDL_PATCH_VER_REQ_CMD)) {
            d = ev + 2; /* WCN3990: vendor event */
        } else if (code == 0x0E && len >= 4 + 2 + 12 && ev[1] == 0x00 && ev[2] == 0xFC) {
            d = ev + 4 + 2; /* WCN3991+: command complete, status, cresp, rtype */
        }
        if (d) {
            *soc_id = d[8] | d[9] << 8 | d[10] << 16 | (uint32_t)d[11] << 24;
            *rom_ver = (uint16_t)(d[6] | d[7] << 8);
            LOG("SoC version: product 0x%02x%02x%02x%02x patch 0x%02x%02x rom 0x%04x soc 0x%08x",
                d[3], d[2], d[1], d[0], d[5], d[4], *rom_ver, *soc_id);
            return 0;
        }
    }
    return -1;
}

static int qca_set_baud(int fd)
{
    uint8_t b = QCA_BAUDRATE_3000000;
    uart_config(fd, 115200, 0);
    if (send_cmd(fd, EDL_SET_BAUDRATE_CMD_OPCODE, &b, 1) < 0)
        return -1;
    tcdrain(fd);
    usleep(10000);
    uart_config(fd, 3000000, 1);
    /* the controller answers at the new speed; the event may be lost */
    uint8_t ev[260];
    int len;
    read_event(fd, ev, &len, 300);
    return 0;
}

static uint8_t *load_file(const char *name, size_t *size, char *found, size_t found_len)
{
    for (int i = 0; FW_DIRS[i]; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", FW_DIRS[i], name);
        FILE *f = fopen(path, "rb");
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
        if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
            fclose(f);
            *size = (size_t)n;
            snprintf(found, found_len, "%s", path);
            return buf;
        }
        free(buf);
        fclose(f);
    }
    return NULL;
}

static int qca_download(int fd, const char *name, int is_patch)
{
    size_t size = 0;
    char path[256];
    uint8_t *fw = load_file(name, &size, path, sizeof(path));
    if (!fw) {
        LOG("%s not found", name);
        for (int i = 0; FW_DIRS[i]; i++) {
            DIR *d = opendir(FW_DIRS[i]);
            if (!d) {
                LOG("  %s: cannot open (errno %d)", FW_DIRS[i], errno);
                continue;
            }
            char line[300];
            size_t n = 0;
            struct dirent *de;
            line[0] = 0;
            while ((de = readdir(d)) != NULL && n < sizeof(line) - 40) {
                if (de->d_name[0] != '.')
                    n += snprintf(line + n, sizeof(line) - n, "%s ", de->d_name);
            }
            closedir(d);
            LOG("  %s: %s", FW_DIRS[i], line);
        }
        return -1;
    }
    /* TLV: type/length word, then for a patch total_size, data_length,
     * format_version, signature, download_mode (offset 14) which tells
     * whether the controller answers each segment with a vendor event */
    /* NVM container: send only the Bluetooth section */
    if (!is_patch && size > 8 && fw[0] == 4 && fw[4] == 2) {
        size_t inner = (size_t)(fw[5] | fw[6] << 8 | fw[7] << 16);
        if (8 + inner <= size) {
            memmove(fw, fw + 4, 4 + inner);
            size = 4 + inner;
            LOG("%s: sending the Bluetooth section, %zu bytes", name, size);
        }
    }
    int mode = 0;
    if (is_patch && size > 16)
        mode = fw[14];
    int want_vse = !(mode == 1 || mode == 3);
    int want_cc = 0;
    LOG("downloading %s (%zu bytes, mode %d)", path, size, mode);
    size_t off = 0;
    int seg = 0;
    while (off < size) {
        uint8_t p[2 + MAX_SEGMENT];
        size_t n = size - off < MAX_SEGMENT ? size - off : MAX_SEGMENT;
        p[0] = EDL_PATCH_TLV_REQ_CMD;
        p[1] = (uint8_t)n;
        memcpy(p + 2, fw + off, n);
        if (send_cmd(fd, EDL_PATCH_CMD_OPCODE, p, (uint8_t)(n + 2)) < 0) {
            free(fw);
            return -1;
        }
        off += n;
        int last = off >= size;
        if (wait_edl(fd, want_vse, want_cc, NULL, NULL, last ? 300 : 2000) < 0 && !last) {
            LOG("%s: no answer to segment %d", name, seg);
            free(fw);
            return -1;
        }
        seg++;
    }
    free(fw);
    LOG("%s: %d segments sent", name, seg);
    return 0;
}

static int hci_reset(int fd)
{
    if (send_cmd(fd, HCI_RESET, NULL, 0) < 0)
        return -1;
    return wait_edl(fd, 0, 1, NULL, NULL, 2000);
}

static void set_bdaddr(int fd)
{
    int zero = 1;
    for (int i = 0; i < 6; i++)
        if (g_bdaddr[i])
            zero = 0;
    if (zero)
        return;
    uint8_t p[6];
    for (int i = 0; i < 6; i++)
        p[i] = g_bdaddr[5 - i];
    send_cmd(fd, EDL_WRITE_BD_ADDR_OPCODE, p, 6);
    wait_edl(fd, 1, 1, NULL, NULL, 500);
}

static int controller_setup(int fd)
{
    uint32_t soc_id = 0;
    uint16_t rom = 0;
    ioctl(fd, TIOCPMGET, 0);
    uart_config(fd, 115200, 1);
    usleep(100000);
    if (qca_read_version(fd, &soc_id, &rom) < 0) {
        /* WCN399x boot pulse: 0xC0 (off) at 2400 baud, 0xFC (on) at 115200 */
        LOG("no answer, sending the WCN399x power pulses");
        uint8_t off = 0xC0, on = 0xFC;
        uart_config(fd, 2400, 0);
        write_all(fd, &off, 1);
        tcdrain(fd);
        usleep(5000);
        uart_config(fd, 115200, 0);
        write_all(fd, &on, 1);
        tcdrain(fd);
        usleep(100000);
        uart_config(fd, 115200, 1);
        if (qca_read_version(fd, &soc_id, &rom) < 0) {
            LOG("controller does not answer on %s", UART_DEV);
            return -1;
        }
    }
    qca_set_baud(fd);
    unsigned v = ((rom & 0x0f00) >> 4) | (rom & 0x000f);
    char name[32];
    LOG("rom 0x%04x, firmware suffix %02x", rom, v);
    if (rom != 0x0201) {
        LOG("no firmware known for rom 0x%04x", rom);
        return -1;
    }
    if (qca_download(fd, "apbtfw11.tlv", 1) < 0)
        return -1;
    usleep(10000);
    if (qca_download(fd, "apnv11.bin", 0) < 0)
        return -1;
    usleep(50000);
    LOG("version after upload:");
    if (qca_read_version(fd, &soc_id, &rom) < 0)
        LOG("no version answer after upload");
    set_bdaddr(fd);
    if (hci_reset(fd) < 0) {
        LOG("HCI reset failed");
        return -1;
    }
    LOG("controller ready at 3 Mbaud");
    return 0;
}

static int power(int on)
{
    if (g_power < 0)
        g_power = open(BTPOWER_DEV, O_RDWR | O_CLOEXEC);
    if (g_power < 0) {
        LOG("open %s: %s", BTPOWER_DEV, strerror(errno));
        return -1;
    }
    if (ioctl(g_power, BT_CMD_PWR_CTRL, (unsigned long)on) < 0) {
        LOG("power %d: %s", on, strerror(errno));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------- interface */
static int vendor_init(const BtVendorCallbacksT *cb, unsigned char *addr)
{
    g_cb = cb;
    if (addr)
        memcpy(g_bdaddr, addr, 6);
    return 0;
}

static int vendor_op(BtOpcodeT opcode, void *param)
{
    switch (opcode) {
        case BT_OP_POWER_ON:
            power(0);
            usleep(20000);
            return power(1);
        case BT_OP_POWER_OFF:
            return power(0);
        case BT_OP_HCI_CHANNEL_OPEN: {
            int *fds = param;
            g_uart = open(UART_DEV, O_RDWR | O_NOCTTY | O_CLOEXEC);
            if (g_uart < 0) {
                LOG("open %s: %s", UART_DEV, strerror(errno));
                return -1;
            }
            if (controller_setup(g_uart) < 0) {
                close(g_uart);
                g_uart = -1;
                return -1;
            }
            for (int i = 0; i < HCI_MAX_CHANNEL; i++)
                fds[i] = g_uart;
            return 1;
        }
        case BT_OP_HCI_CHANNEL_CLOSE:
            if (g_uart >= 0) {
                ioctl(g_uart, TIOCPMPUT, 0);
                close(g_uart);
                g_uart = -1;
            }
            return 0;
        case BT_OP_INIT:
            if (g_cb && g_cb->initCb)
                g_cb->initCb(BTC_OP_RESULT_SUCCESS);
            return 0;
        case BT_OP_GET_LPM_TIMER:
            if (param)
                *(uint32_t *)param = 3000;
            return 0;
        default:
            return 0;
    }
}

static void vendor_close(void)
{
    if (g_uart >= 0)
        close(g_uart);
    g_uart = -1;
    if (g_power >= 0) {
        ioctl(g_power, BT_CMD_PWR_CTRL, 0UL);
        close(g_power);
    }
    g_power = -1;
}

__attribute__((visibility("default"))) const BtVendorInterfaceT BLUETOOTH_VENDOR_LIB_INTERFACE = {
    sizeof(BtVendorInterfaceT), vendor_init, vendor_op, vendor_close,
};
