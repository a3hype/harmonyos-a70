// a70-shutdownd: finishes reboots and power-offs that init cannot complete under XGSI.
// init's reboot call is refused (EPERM even with CAP_SYS_BOOT), so after a request the phone sits with
// everything stopped. This daemon notices foundation, render_service and hdcd all gone, then uses
// /proc/sysrq-trigger: sync, remount read-only, then reset or power off.
// Choice: the last "poweroff shutdown" / "poweroff reboot" line in recent saved hilog files. If neither
// is found, power off at 3% battery or less, otherwise reboot.
// Usage: a70-shutdownd            daemonize and watch
//        a70-shutdownd --dry FILE...  print the decision for these log files, change nothing
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HILOG_DIR "/data/log/hilog"
#define NEED_SHUT "poweroff shutdown"
#define NEED_BOOT "poweroff reboot"
#define MAX_FILES 8
#define RECENT_SECONDS 300
#define CHARGER_COUNT "/data/a70fw/shutdownd_charger_count"
#define LOCK_NAME "a70-shutdownd"

static const char *const names[] = {"foundation", "render_service", "hdcd"};
#define NAMES 3
static pid_t pids[NAMES];

static void klog(const char *fmt, ...) {
    char buf[200];
    int fd, n = snprintf(buf, sizeof(buf), "a70sd: ");
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
    va_end(ap);
    fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
        write(fd, buf, n);
        close(fd);
    }
}

static int is_named(pid_t pid, const char *name) {
    char path[48], comm[32];
    int fd, n;
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    n = read(fd, comm, sizeof(comm) - 1);
    close(fd);
    if (n <= 0) return 0;
    comm[n] = 0;
    if (comm[n - 1] == '\n') comm[n - 1] = 0;
    return strcmp(comm, name) == 0;
}

// Fills pids[] by scanning /proc. Only runs when a remembered pid has stopped matching.
static void scan_proc(void) {
    DIR *d = opendir("/proc");
    struct dirent *e;
    int i;
    for (i = 0; i < NAMES; i++) pids[i] = 0;
    if (!d) return;
    while ((e = readdir(d))) {
        pid_t pid = atoi(e->d_name);
        if (pid <= 0) continue;
        for (i = 0; i < NAMES; i++) {
            if (!pids[i] && is_named(pid, names[i])) pids[i] = pid;
        }
    }
    closedir(d);
}

// Returns how many of the watched processes are alive, rescanning if any remembered pid is gone.
static int alive_count(void) {
    int i, n = 0, stale = 0;
    for (i = 0; i < NAMES; i++) {
        if (pids[i] && is_named(pids[i], names[i])) n++;
        else stale = 1;
    }
    if (!stale) return n;
    scan_proc();
    n = 0;
    for (i = 0; i < NAMES; i++) {
        if (pids[i]) n++;
    }
    return n;
}

// Streams one .gz through zcat. Sets *last to 's' or 'b' for the last needle found.
static void search_file(const char *path, char *last) {
    int fds[2];
    pid_t pid;
    static char buf[65536];
    size_t keep = 0, nlen = strlen(NEED_SHUT) > strlen(NEED_BOOT) ? strlen(NEED_SHUT) : strlen(NEED_BOOT);
    ssize_t n;
    if (pipe(fds)) return;
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        dup2(fds[1], 1);
        if (null >= 0) dup2(null, 2);
        close(fds[0]);
        close(fds[1]);
        execl("/system/bin/zcat", "zcat", path, (char *)0);
        _exit(127);
    }
    close(fds[1]);
    while ((n = read(fds[0], buf + keep, sizeof(buf) - keep)) > 0) {
        size_t have = keep + n, i;
        for (i = 0; i + nlen <= have; i++) {
            if (buf[i] != 'p') continue;
            if (!memcmp(buf + i, NEED_SHUT, strlen(NEED_SHUT))) *last = 's';
            else if (!memcmp(buf + i, NEED_BOOT, strlen(NEED_BOOT))) *last = 'b';
        }
        keep = have < nlen ? have : nlen - 1;
        memmove(buf, buf + have - keep, keep);
    }
    close(fds[0]);
    waitpid(pid, NULL, 0);
    // Check the tail shorter than the longest needle.
    if (keep >= strlen(NEED_BOOT)) {
        size_t i;
        for (i = 0; i + strlen(NEED_BOOT) <= keep; i++) {
            if (!memcmp(buf + i, NEED_BOOT, strlen(NEED_BOOT))) *last = 'b';
        }
    }
}

struct logf {
    char path[300];
    time_t mtime;
};

// Newest recent logs first. The newest file holding either line decides.
static char decide_from_logs(void) {
    struct logf files[MAX_FILES];
    int count = 0, i, j;
    time_t now = time(NULL);
    DIR *d = opendir(HILOG_DIR);
    struct dirent *e;
    char last = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        struct stat st;
        char path[300];
        size_t len = strlen(e->d_name);
        if (strncmp(e->d_name, "hilog.", 6) || len < 4 || strcmp(e->d_name + len - 3, ".gz")) continue;
        snprintf(path, sizeof(path), "%s/%s", HILOG_DIR, e->d_name);
        if (stat(path, &st) || now - st.st_mtime > RECENT_SECONDS) continue;
        if (count == MAX_FILES) {
            int oldest = 0;
            for (i = 1; i < count; i++) {
                if (files[i].mtime < files[oldest].mtime) oldest = i;
            }
            if (st.st_mtime <= files[oldest].mtime) continue;
            files[oldest] = files[--count];
        }
        snprintf(files[count].path, sizeof(files[count].path), "%s", path);
        files[count].mtime = st.st_mtime;
        count++;
    }
    closedir(d);
    for (i = 0; i < count; i++) {
        for (j = i + 1; j < count; j++) {
            if (files[j].mtime > files[i].mtime) {
                struct logf t = files[i];
                files[i] = files[j];
                files[j] = t;
            }
        }
    }
    for (i = 0; i < count && !last; i++) search_file(files[i].path, &last);
    return last;
}

static int battery_percent(void) {
    char buf[16];
    int fd = open("/sys/class/power_supply/battery/capacity", O_RDONLY | O_CLOEXEC), n;
    if (fd < 0) return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    return atoi(buf);
}

static void sysrq(char c) {
    int fd = open("/proc/sysrq-trigger", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        klog("cannot open sysrq-trigger\n");
        return;
    }
    write(fd, &c, 1);
    close(fd);
}

static void finish(void) {
    char why, action;
    int pct;
    klog("services gone, syncing\n");
    sync();
    sysrq('s');
    sleep(2);
    why = decide_from_logs();
    pct = battery_percent();
    if (why == 's') action = 'o';
    else if (why == 'b') action = 'b';
    else action = (pct >= 0 && pct <= 3) ? 'o' : 'b';
    klog("log=%c battery=%d action=%c\n", why ? why : '-', pct, action);
    sync();
    sysrq('u');
    sleep(2);
    klog("sysrq %c\n", action);
    sysrq(action);
    sleep(30);
    klog("still alive 30 s after sysrq %c\n", action);
}

// A cable on a powered-off phone makes the bootloader start the OS with androidboot.mode=charger, and the
// touch driver then refuses to load. A plain reboot comes back in normal mode. The counter on /data stops
// a reboot loop if that ever changes. The clock is not usable here, since it starts in 2018 on every boot.
static void handle_charger_boot(void) {
    static char cmdline[8192];
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC), n, count = 0, pct;
    char c[8];
    if (fd < 0) return;
    n = read(fd, cmdline, sizeof(cmdline) - 1);
    close(fd);
    if (n <= 0) return;
    cmdline[n] = 0;
    if (!strstr(cmdline, "androidboot.mode=charger")) {
        unlink(CHARGER_COUNT);
        return;
    }
    fd = open(CHARGER_COUNT, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        n = read(fd, c, sizeof(c) - 1);
        close(fd);
        if (n > 0) {
            c[n] = 0;
            count = atoi(c);
        }
    }
    pct = battery_percent();
    klog("charger-mode boot, count=%d battery=%d\n", count, pct);
    if (count >= 2) {
        klog("charger-mode reboot limit reached, staying up\n");
        return;
    }
    if (pct >= 0 && pct <= 5) {
        klog("battery low, staying up to charge\n");
        return;
    }
    fd = open(CHARGER_COUNT, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        n = snprintf(c, sizeof(c), "%d", count + 1);
        write(fd, c, n);
        close(fd);
    }
    sync();
    sysrq('s');
    sleep(2);
    sync();
    sysrq('u');
    sleep(2);
    klog("sysrq b, leaving charger mode\n");
    sysrq('b');
    sleep(30);
    klog("still alive 30 s after charger-mode reboot\n");
}

// The boot job fires twice, so only the first copy may stay. An abstract socket vanishes with the process,
// so it cannot leave a stale lock behind. The fd stays open for the life of the daemon on purpose.
// Returns 0 only when another copy holds the lock; any other error carries on, as watching beats not.
static int take_lock(void) {
    struct sockaddr_un addr;
    socklen_t len;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path + 1, LOCK_NAME, strlen(LOCK_NAME));
    len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(LOCK_NAME);
    if (bind(fd, (struct sockaddr *)&addr, len) == 0) return 1;
    if (errno == EADDRINUSE) {
        close(fd);
        return 0;
    }
    klog("lock failed, errno %d, continuing\n", errno);
    close(fd);
    return 1;
}

static void daemonize(void) {
    pid_t pid = fork();
    int null;
    if (pid < 0) _exit(1);
    if (pid > 0) _exit(0);
    setsid();
    signal(SIGHUP, SIG_IGN);
    pid = fork();
    if (pid < 0) _exit(1);
    if (pid > 0) _exit(0);
    chdir("/");
    null = open("/dev/null", O_RDWR);
    if (null >= 0) {
        dup2(null, 0);
        dup2(null, 1);
        dup2(null, 2);
        if (null > 2) close(null);
    }
}

int main(int argc, char **argv) {
    int gone = 0, seen = 0;
    if (argc > 2 && !strcmp(argv[1], "--dry")) {
        int i;
        for (i = 2; i < argc; i++) {
            char last = 0;
            search_file(argv[i], &last);
            printf("%s: %c\n", argv[i], last ? last : '-');
        }
        return 0;
    }
    daemonize();
    if (!take_lock()) {
        klog("already running, pid %d exiting\n", (int)getpid());
        return 0;
    }
    klog("watching, pid %d\n", (int)getpid());
    handle_charger_boot();
    for (;;) {
        int n = alive_count();
        // Armed only after seeing a watched process, so a slow boot cannot look like a shutdown.
        if (n > 0) seen = 1;
        if (seen && n == 0) {
            gone++;
            if (gone >= 3) {
                finish();
                gone = 0;
                seen = 0;
            }
            sleep(1);
        } else {
            gone = 0;
            sleep(2);
        }
    }
    return 0;
}
