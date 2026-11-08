// a70-hexrpcd: waits for the ADSP, then becomes hexagonrpcd serving the A70 sensor files.
// The boot job can fire before the ADSP is up, and hexagonrpcd exits if it cannot attach.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define STATE "/sys/bus/msm_subsys/devices/subsys0/state"

static int adsp_online(void) {
    char buf[16] = {0};
    int fd = open(STATE, O_RDONLY | O_CLOEXEC), n;
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 0 && !strncmp(buf, "ONLINE", 6);
}

int main(void) {
    int i;
    for (i = 0; i < 120 && !adsp_online(); i++) sleep(1);
    sleep(3);
    execl("/system/bin/hexagonrpcd", "hexagonrpcd", "-f", "/dev/adsprpc-smd-secure", "-d", "adsp", "-s", "-R",
          "/data/a70fw/sensors/root", (char *)0);
    perror("exec hexagonrpcd");
    return 1;
}
