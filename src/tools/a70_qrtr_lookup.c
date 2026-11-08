// Lists QRTR services visible now. Read-only. Service 400 is the SSC client manager (inferred from
// Qualcomm's sns_client QMI service id; a lookup here only shows whether it is registered).
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif
#define QRTR_PORT_CTRL 0xfffffffeu
#define TYPE_NEW_SERVER 4
#define TYPE_NEW_LOOKUP 10

struct sockaddr_qrtr {
    unsigned short sq_family;
    uint32_t sq_node;
    uint32_t sq_port;
};
struct ctrl_pkt {
    uint32_t cmd;
    uint32_t service, instance, node, port;
};

int main(void) {
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
    struct sockaddr_qrtr me, to;
    socklen_t len = sizeof(me);
    struct ctrl_pkt pkt;
    struct timeval tv = {3, 0};
    int count = 0;
    if (fd < 0) {
        printf("socket failed: %s\n", strerror(errno));
        return 1;
    }
    if (getsockname(fd, (struct sockaddr *)&me, &len)) {
        printf("getsockname failed: %s\n", strerror(errno));
        return 1;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    memset(&pkt, 0, sizeof(pkt));
    pkt.cmd = TYPE_NEW_LOOKUP;
    memset(&to, 0, sizeof(to));
    to.sq_family = AF_QIPCRTR;
    to.sq_node = me.sq_node;
    to.sq_port = QRTR_PORT_CTRL;
    if (sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        printf("sendto failed: %s\n", strerror(errno));
        return 1;
    }
    printf("local node %u\n", me.sq_node);
    for (;;) {
        ssize_t n = recv(fd, &pkt, sizeof(pkt), 0);
        if (n < 0) {
            printf("recv ended: %s\n", strerror(errno));
            break;
        }
        if (n < (ssize_t)sizeof(pkt) || pkt.cmd != TYPE_NEW_SERVER) continue;
        if (pkt.service == 0 && pkt.instance == 0 && pkt.node == 0 && pkt.port == 0) break;
        printf("service %u version %u instance %u node %u port %u\n", pkt.service, pkt.instance & 0xff,
               pkt.instance >> 8, pkt.node, pkt.port);
        count++;
    }
    printf("total %d\n", count);
    return 0;
}
