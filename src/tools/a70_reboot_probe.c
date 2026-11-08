// Tests whether the reboot syscall reaches the kernel from under XGSI's seccomp filter.
// Uses only an invalid command, so it cannot reboot or power off the phone.
// EINVAL means the kernel received the call. EPERM with CAP_SYS_BOOT present points at the filter.
// Result goes to /data/local/tmp/reboot_probe.log because init services have no visible stdout.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MAGIC1 0xfee1dead
#define MAGIC2 672274793
#define CMD_INVALID 0x12345678

int main(void) {
    FILE *out = fopen("/data/local/tmp/reboot_probe.log", "w");
    FILE *st = fopen("/proc/self/status", "r");
    char line[256];
    long r;
    if (!out) out = stdout;
    if (st) {
        while (fgets(line, sizeof(line), st)) {
            if (!strncmp(line, "Uid:", 4) || !strncmp(line, "CapEff:", 7) || !strncmp(line, "Seccomp:", 8) ||
                !strncmp(line, "NoNewPrivs:", 11))
                fputs(line, out);
        }
        fclose(st);
    }
    errno = 0;
    r = syscall(__NR_reboot, MAGIC1, MAGIC2, CMD_INVALID, 0);
    fprintf(out, "reboot(invalid) ret=%ld errno=%d (%s)\n", r, errno, strerror(errno));
    fclose(out);
    return 0;
}
