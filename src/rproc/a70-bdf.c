// a70-bdf: sends the Wi-Fi board data file to the WLAN firmware over QMI, from userspace.
//
// On Android, cnss-daemon does this. Without it the firmware never reports FW_READY and
// wlan0 never appears. Message layout is from the A70 kernel's wlan_firmware_service_v01.
// Sequence and file naming follow hmos2sweet's kernel patches 0011 and 0014.
//
// Usage: a70-bdf --info              read and print the board id, send nothing
//        a70-bdf [-d DIR] [-f FILE]  send DIR/bdwlan.<board id>, or FILE if given
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/qrtr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "libqrtr.h"

#define WLFW_SERVICE 0x45
#define WLFW_VERSION 1
#define MSG_CAP 0x24
#define MSG_BDF 0x25
#define MSG_CAL_REPORT 0x26
#define MAX_SEG 6144
#define BUF_SIZE (MAX_SEG + 128)

static uint16_t next_txn = 1;
static struct sockaddr_qrtr server;
static int sock = -1;

static size_t put_tlv(uint8_t *buf, size_t off, uint8_t type, const void *val, uint16_t len)
{
    buf[off] = type;
    buf[off + 1] = len & 0xff;
    buf[off + 2] = len >> 8;
    if (len)
        memcpy(buf + off + 3, val, len);
    return off + 3 + len;
}

static size_t put_u32(uint8_t *buf, size_t off, uint8_t type, uint32_t v)
{
    uint8_t le[4] = {v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff};
    return put_tlv(buf, off, type, le, 4);
}

// Sends a request and waits for the matching response. Returns the response payload
// length with payload at resp+7, or a negative errno.
static int transact(uint16_t msg, uint8_t *req, size_t tlv_len, uint8_t *resp, size_t resp_size)
{
    uint16_t txn = next_txn++;
    req[0] = 0; // request
    req[1] = txn & 0xff;
    req[2] = txn >> 8;
    req[3] = msg & 0xff;
    req[4] = msg >> 8;
    req[5] = tlv_len & 0xff;
    req[6] = tlv_len >> 8;
    if (sendto(sock, req, tlv_len + 7, 0, (struct sockaddr *)&server, sizeof(server)) < 0)
        return -errno;
    for (int tries = 0; tries < 50; tries++) {
        struct sockaddr_qrtr from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(sock, resp, resp_size, 0, (struct sockaddr *)&from, &fl);
        if (n < 0)
            return -errno;
        if (n < 7 || from.sq_port != server.sq_port || resp[0] != 2)
            continue; // not a response, e.g. an indication
        if ((uint16_t)(resp[1] | resp[2] << 8) != txn || (uint16_t)(resp[3] | resp[4] << 8) != msg)
            continue;
        return (int)n - 7;
    }
    return -ETIMEDOUT;
}

// Finds a TLV in a response payload. Returns its value or NULL.
static const uint8_t *find_tlv(const uint8_t *p, int len, uint8_t type, int *vlen)
{
    int off = 0;
    while (off + 3 <= len) {
        int l = p[off + 1] | p[off + 2] << 8;
        if (off + 3 + l > len)
            return NULL;
        if (p[off] == type) {
            *vlen = l;
            return p + off + 3;
        }
        off += 3 + l;
    }
    return NULL;
}

// Returns 0 when the response's result TLV says success.
static int check_result(const uint8_t *payload, int len, const char *what)
{
    int vl = 0;
    const uint8_t *r = find_tlv(payload, len, 0x02, &vl);
    if (!r || vl < 4) {
        fprintf(stderr, "a70-bdf: %s: no result in response\n", what);
        return -1;
    }
    int result = r[0] | r[1] << 8, error = r[2] | r[3] << 8;
    if (result) {
        fprintf(stderr, "a70-bdf: %s rejected: result %d error %d\n", what, result, error);
        return -1;
    }
    return 0;
}

static int find_server(void)
{
    struct qrtr_ctrl_pkt pkt;
    if (qrtr_new_lookup(sock, WLFW_SERVICE, WLFW_VERSION, 0) < 0)
        return -errno;
    for (int i = 0; i < 64; i++) {
        struct sockaddr_qrtr from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(sock, &pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fl);
        if (n < 0)
            return -errno;
        if (n < (ssize_t)sizeof(pkt) || le32toh(pkt.cmd) != QRTR_TYPE_NEW_SERVER)
            continue;
        if (!pkt.server.service && !pkt.server.node && !pkt.server.port)
            break; // end of list
        if (le32toh(pkt.server.service) == WLFW_SERVICE) {
            server.sq_family = AF_QIPCRTR;
            server.sq_node = le32toh(pkt.server.node);
            server.sq_port = le32toh(pkt.server.port);
            return 0;
        }
    }
    return -ENOENT;
}

static int read_all(const char *path, uint8_t **data, size_t *size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        close(fd);
        return -EINVAL;
    }
    *data = malloc(st.st_size);
    if (!*data || read(fd, *data, st.st_size) != st.st_size) {
        close(fd);
        return -EIO;
    }
    close(fd);
    *size = st.st_size;
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = "/data/a70fw/image", *file = NULL;
    int info_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--info"))
            info_only = 1;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc)
            dir = argv[++i];
        else if (!strcmp(argv[i], "-f") && i + 1 < argc)
            file = argv[++i];
        else {
            fprintf(stderr, "a70-bdf: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    sock = qrtr_open(0);
    if (sock < 0) {
        fprintf(stderr, "a70-bdf: qrtr_open: %s\n", strerror(errno));
        return 1;
    }
    struct timeval tv = {.tv_sec = 5};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int r = find_server();
    if (r) {
        fprintf(stderr, "a70-bdf: WLFW service not found: %s\n", strerror(-r));
        return 1;
    }
    printf("a70-bdf: WLFW server at node %u port %u\n", server.sq_node, server.sq_port);

    static uint8_t req[BUF_SIZE], resp[BUF_SIZE];
    int len = transact(MSG_CAP, req, 0, resp, sizeof(resp));
    if (len < 0) {
        fprintf(stderr, "a70-bdf: capability request: %s\n", strerror(-len));
        return 1;
    }
    const uint8_t *payload = resp + 7;
    if (check_result(payload, len, "capability request"))
        return 1;

    uint32_t board_id = 0xff;
    int vl = 0;
    const uint8_t *v = find_tlv(payload, len, 0x11, &vl);
    if (v && vl >= 4)
        board_id = v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24;
    printf("a70-bdf: board id 0x%x\n", board_id);
    v = find_tlv(payload, len, 0x10, &vl);
    if (v && vl >= 8)
        printf("a70-bdf: chip id 0x%x rev 0x%x\n", v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24,
               v[4] | v[5] << 8 | v[6] << 16 | (uint32_t)v[7] << 24);
    v = find_tlv(payload, len, 0x14, &vl);
    if (v && vl > 1)
        printf("a70-bdf: firmware build %.*s\n", v[0] < vl - 1 ? v[0] : vl - 1, v + 1);

    char path[256] = "";
    if (file) {
        snprintf(path, sizeof(path), "%s", file);
    } else {
        char names[3][32];
        snprintf(names[0], sizeof(names[0]), board_id < 0x100 ? "bdwlan.b%02x" : "bdwlan.%03x", board_id);
        snprintf(names[1], sizeof(names[1]), "bdwlan.b%02x", board_id & 0xff);
        snprintf(names[2], sizeof(names[2]), "bdwlan.bin");
        for (int i = 0; i < 3; i++) {
            char candidate[256];
            snprintf(candidate, sizeof(candidate), "%s/%s", dir, names[i]);
            if (access(candidate, R_OK) == 0) {
                snprintf(path, sizeof(path), "%s", candidate);
                break;
            }
        }
    }
    printf("a70-bdf: board data file: %s\n", path[0] ? path : "NONE FOUND");
    if (info_only)
        return 0;
    if (!path[0])
        return 1;

    uint8_t *data;
    size_t size;
    r = read_all(path, &data, &size);
    if (r) {
        fprintf(stderr, "a70-bdf: read %s: %s\n", path, strerror(-r));
        return 1;
    }

    size_t left = size;
    uint32_t seg = 0;
    while (left) {
        uint16_t n = left > MAX_SEG ? MAX_SEG : (uint16_t)left;
        uint8_t valid = 1, end = left == n;
        size_t off = 7;
        off = put_tlv(req, off, 0x01, &valid, 1);
        off = put_u32(req, off, 0x10, 0); // file id
        off = put_u32(req, off, 0x11, (uint32_t)size);
        off = put_u32(req, off, 0x12, seg);
        // Data TLV: 16-bit length, then the bytes.
        req[off] = 0x13;
        req[off + 1] = (n + 2) & 0xff;
        req[off + 2] = (n + 2) >> 8;
        req[off + 3] = n & 0xff;
        req[off + 4] = n >> 8;
        memcpy(req + off + 5, data + (size - left), n);
        off += 5 + n;
        off = put_tlv(req, off, 0x14, &end, 1);
        len = transact(MSG_BDF, req, off - 7, resp, sizeof(resp));
        if (len < 0) {
            fprintf(stderr, "a70-bdf: segment %u: %s\n", seg, strerror(-len));
            return 1;
        }
        if (check_result(resp + 7, len, "BDF segment"))
            return 1;
        left -= n;
        seg++;
    }
    printf("a70-bdf: sent %zu bytes in %u segments\n", size, seg);

    // The calibration report is what makes the firmware send FW_READY.
    // Mandatory TLV 0x01: 8-bit count of meta data entries, here none.
    size_t off = 7;
    uint8_t zero = 0;
    off = put_tlv(req, off, 0x01, &zero, 1);
    len = transact(MSG_CAL_REPORT, req, off - 7, resp, sizeof(resp));
    if (len < 0) {
        fprintf(stderr, "a70-bdf: cal report: %s\n", strerror(-len));
        return 1;
    }
    if (check_result(resp + 7, len, "cal report"))
        return 1;
    printf("a70-bdf: calibration report accepted\n");
    return 0;
}
