// a70-rproc: starts the Qualcomm coprocessors on a Galaxy A70 under HarmonyOS.
//
// Android's vendor daemons normally do this. The approach follows hmos2sweet's sweet-rproc
// (tsryzenstech/hmos2sweet), written separately for the A70.
//
// XGSI hides all block devices from HarmonyOS, so nothing here touches a partition:
//   /data/a70fw/image   firmware files copied from apnhlos and modem
//   /data/a70fw/dsp     copy of the dsp partition
//   /data/a70fw/rmtfs   copies of modemst1, modemst2, fsg, fsc as modem_fs1, modem_fs2,
//                       modem_fsg, modem_fsc. The modem reads and writes these, never the
//                       real partitions.
//
// Usage: a70-rproc [-d TOOLDIR] [--firmware] [--helpers] [--adsp] [--cdsp] [--modem] [--wlan]
//        a70-rproc --status
//        a70-rproc --boot       the full sequence, used by the init service:
//                               all of the below except --cdsp, then board data via a70-bdf,
//                               then wifi_host, wpa_host and a wifi_manager_service restart
//   --firmware  point the kernel's firmware search at /data/a70fw/image
//   --helpers   start qrtr-ns, rmtfs, pd-mapper, tqftpserv from TOOLDIR
//   --rmtfs-rw  let rmtfs keep modem writes in its files, instead of in RAM only
//   --adsp      ask the kernel to boot the ADSP
//   --cdsp      ask the kernel to boot the CDSP
//   --modem     boot the modem by holding /dev/subsys_modem open
//   --wlan      ask the WLAN host driver to start (needs --modem and the board data file)
//
// Everything is logged to kmsg with the "a70-rproc:" prefix. It stays running until
// killed, because closing /dev/subsys_modem shuts the modem down.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define SUBSYS_DIR "/sys/bus/msm_subsys/devices"
#define MAX_CHILDREN 8
#define FW_ROOT "/data/a70fw"

static pid_t children[MAX_CHILDREN];
static int n_children;
static volatile sig_atomic_t stop;

static void on_signal(int s)
{
    (void)s;
    stop = 1;
}

static void klog(const char *fmt, ...)
{
    char body[400], line[440];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    int n = snprintf(line, sizeof(line), "a70-rproc: %s\n", body);
    fputs(line, stderr);
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (write(fd, line, n) < 0) {
            // kmsg is best effort; stderr already has the message
        }
        close(fd);
    }
}

static int write_file(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    int r = write(fd, value, strlen(value)) < 0 ? -errno : 0;
    close(fd);
    return r;
}

static int read_file(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -errno;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
        n--;
    buf[n] = 0;
    return 0;
}

static void subsys_report(const char *when)
{
    DIR *d = opendir(SUBSYS_DIR);
    if (!d) {
        klog("%s: no %s (errno %d)", when, SUBSYS_DIR, errno);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char path[300], name[64] = "?", state[64] = "?";
        if (de->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s/name", SUBSYS_DIR, de->d_name);
        read_file(path, name, sizeof(name));
        snprintf(path, sizeof(path), "%s/%s/state", SUBSYS_DIR, de->d_name);
        read_file(path, state, sizeof(state));
        klog("%s: %s (%s) %s", when, de->d_name, name, state);
    }
    closedir(d);
}

// A crashing coprocessor restarts alone instead of taking the whole phone down.
static void set_restart_levels(void)
{
    DIR *d = opendir(SUBSYS_DIR);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char path[300];
        if (de->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s/restart_level", SUBSYS_DIR, de->d_name);
        int r = write_file(path, "related");
        if (r)
            r = write_file(path, "RELATED");
        if (r)
            klog("restart_level %s: %d", de->d_name, r);
    }
    closedir(d);
}

static void spawn(const char *tooldir, const char *tool, const char *a1, const char *a2, const char *a3)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", tooldir, tool);
    if (access(path, X_OK)) {
        klog("missing %s", path);
        return;
    }
    pid_t p = fork();
    if (p == 0) {
        int k = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
        if (k >= 0) {
            dup2(k, 1);
            dup2(k, 2);
        }
        char *argv[] = {path, (char *)a1, (char *)a2, (char *)a3, NULL};
        execv(path, argv);
        _exit(127);
    }
    klog("started %s (pid %d)", tool, (int)p);
    if (p > 0 && n_children < MAX_CHILDREN)
        children[n_children++] = p;
}

// Runs a command with its output in kmsg and waits. Returns the exit status, or -1.
static int run_wait(char *const argv[])
{
    pid_t p = fork();
    if (p < 0)
        return -1;
    if (p == 0) {
        int k = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
        if (k >= 0) {
            dup2(k, 1);
            dup2(k, 2);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    while (waitpid(p, &st, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int param_is_true(const char *name)
{
    char cmd[128], out[64] = "";
    snprintf(cmd, sizeof(cmd), "/system/bin/param get %s 2>/dev/null", name);
    FILE *f = popen(cmd, "r");
    if (!f)
        return 0;
    if (!fgets(out, sizeof(out), f))
        out[0] = 0;
    pclose(f);
    return strstr(out, "true") != NULL;
}

// The firmware service appears a little after the modem is ONLINE, so keep trying.
static int send_board_data(const char *tooldir)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/a70-bdf", tooldir);
    char *const argv[] = {path, "-f", "/vendor/firmware/wlan/qca_cld/bdwlan.bin", NULL};
    for (int i = 0; i < 12 && !stop; i++) {
        int r = run_wait(argv);
        klog("board data try %d -> %d", i + 1, r);
        if (r == 0)
            return 0;
        sleep(5);
    }
    return -1;
}

// The sound card registers a few seconds after the ADSP boots. audio_host doesn't run as
// root, so it needs these nodes opened up. Never touch . or .. here.
// msm-adreno-tz sat at 290 MHz while 80+% busy, which made scrolling uneven.
// The write can fail with EINVAL early in boot, so a detached process keeps
// trying for two minutes and reads the value back. Boot does not wait for it.
static void lock_gpu_governor(void)
{
    static const char gov[] = "/sys/class/kgsl/kgsl-3d0/devfreq/governor";
    pid_t p = fork();
    if (p < 0) {
        klog("gpu governor: fork failed: %s", strerror(errno));
        return;
    }
    if (p > 0) {
        while (waitpid(p, NULL, 0) < 0 && errno == EINTR)
            ;
        return;
    }
    if (fork() != 0)
        _exit(0);
    int was_set = 0, fails = 0, last = 0;
    for (int i = 0; i < 120; i++) {
        char cur[64] = "";
        read_file(gov, cur, sizeof(cur));
        if (strcmp(cur, "performance")) {
            if (was_set)
                klog("gpu governor: changed to %s after %d s", cur, i);
            was_set = 0;
            int r = write_file(gov, "performance");
            if (r && r != last)
                klog("gpu governor -> %d after %d s", r, i);
            last = r;
            fails += r != 0;
        }
        if (!was_set) {
            read_file(gov, cur, sizeof(cur));
            if (!strcmp(cur, "performance")) {
                klog("gpu governor: performance after %d s, %d failed writes", i, fails);
                was_set = 1;
            }
        }
        sleep(1);
    }
    if (!was_set)
        klog("gpu governor: gave up, %d failed writes", fails);
    _exit(0);
}

static void open_sound_nodes(void)
{
    struct stat st;
    for (int i = 0; i < 30 && !stop && stat("/dev/snd/controlC0", &st); i++)
        sleep(1);
    DIR *d = opendir("/dev/snd");
    if (!d) {
        klog("no /dev/snd: %s", strerror(errno));
        return;
    }
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char path[300];
        if (de->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/dev/snd/%s", de->d_name);
        if (lstat(path, &st) == 0 && S_ISCHR(st.st_mode) && chmod(path, 0666) == 0)
            n++;
    }
    closedir(d);
    klog("sound nodes opened: %d", n);
}

static void begetctl(const char *verb, const char *service)
{
    char *const argv[] = {"/system/bin/begetctl", (char *)verb, (char *)service, NULL};
    klog("begetctl %s %s -> %d", verb, service, run_wait(argv));
}

// These services start at boot before wlan0 exists, and the manager doesn't recover by
// itself ("chip hdi is not started"), so start them in order once wlan0 is there.
static void start_wifi_stack(void)
{
    for (int i = 0; i < 180 && !stop && !param_is_true("bootevent.wms.ready"); i++)
        sleep(1);
    klog("window manager ready: %d", param_is_true("bootevent.wms.ready"));
    begetctl("start_service", "wifi_host");
    begetctl("start_service", "wpa_host");
    sleep(1);
    begetctl("stop_service", "wifi_manager_service");
    sleep(2);
    begetctl("start_service", "wifi_manager_service");
}

static const char *subsys_state(const char *want, char *state, size_t size)
{
    DIR *d = opendir(SUBSYS_DIR);
    state[0] = 0;
    if (!d)
        return state;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char path[300], name[64];
        if (de->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s/name", SUBSYS_DIR, de->d_name);
        if (read_file(path, name, sizeof(name)) || strcmp(name, want))
            continue;
        snprintf(path, sizeof(path), "%s/%s/state", SUBSYS_DIR, de->d_name);
        read_file(path, state, size);
    }
    closedir(d);
    return state;
}

static void wait_online(const char *name, int secs)
{
    char state[64] = "";
    for (int i = 0; i < secs && !stop; i++) {
        if (!strcmp(subsys_state(name, state, sizeof(state)), "ONLINE")) {
            klog("%s is ONLINE after %d s", name, i);
            return;
        }
        sleep(1);
    }
    klog("%s not ONLINE after %d s (state %s)", name, secs, state);
}

int main(int argc, char **argv)
{
    const char *tooldir = "/system/bin";
    int firmware = 0, helpers = 0, adsp = 0, cdsp = 0, modem = 0, wlan = 0, status = 0, rmtfs_rw = 0, boot = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc)
            tooldir = argv[++i];
        else if (!strcmp(argv[i], "--boot"))
            boot = firmware = helpers = adsp = modem = wlan = rmtfs_rw = 1;
        else if (!strcmp(argv[i], "--firmware"))
            firmware = 1;
        else if (!strcmp(argv[i], "--helpers"))
            helpers = 1;
        else if (!strcmp(argv[i], "--rmtfs-rw"))
            rmtfs_rw = 1;
        else if (!strcmp(argv[i], "--adsp"))
            adsp = 1;
        else if (!strcmp(argv[i], "--cdsp"))
            cdsp = 1;
        else if (!strcmp(argv[i], "--modem"))
            modem = 1;
        else if (!strcmp(argv[i], "--wlan"))
            wlan = 1;
        else if (!strcmp(argv[i], "--status"))
            status = 1;
        else {
            fprintf(stderr, "a70-rproc: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    subsys_report("before");
    if (status)
        return 0;

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    set_restart_levels();

    if (boot) {
        struct stat st;
        for (int i = 0; i < 120 && !stop && stat(FW_ROOT "/image/adsp.mdt", &st); i++)
            sleep(1);
    }

    if (firmware) {
        struct stat st;
        klog("firmware dir %s: %s", FW_ROOT "/image", stat(FW_ROOT "/image/adsp.mdt", &st) ? "MISSING adsp.mdt" : "ok");
        klog("firmware path -> %d", write_file("/sys/module/firmware_class/parameters/path", FW_ROOT "/image"));
        // tqftpserv keeps the modem's writable files here.
        mkdir("/data/vendor", 0771);
        mkdir("/data/vendor/tqftpserv", 0700);
    }

    if (helpers) {
        // Without -P, rmtfs opens plain files in the -o folder, not partitions.
        // -r also keeps modem writes in RAM. --rmtfs-rw saves them to those files.
        spawn(tooldir, "qrtr-ns", "-f", NULL, NULL);
        sleep(1);
        if (rmtfs_rw)
            spawn(tooldir, "rmtfs", "-o", FW_ROOT "/rmtfs", NULL);
        else
            spawn(tooldir, "rmtfs", "-r", "-o", FW_ROOT "/rmtfs");
        klog("rmtfs: plain files, %s", rmtfs_rw ? "writes saved" : "writes kept in RAM");
        spawn(tooldir, "pd-mapper", NULL, NULL, NULL);
        spawn(tooldir, "tqftpserv", NULL, NULL, NULL);
        sleep(2);
    }

    if (adsp) {
        klog("adsp boot request: %d", write_file("/sys/kernel/boot_adsp/boot", "1"));
        wait_online("adsp", 30);
    }
    if (cdsp) {
        klog("cdsp boot request: %d", write_file("/sys/kernel/boot_cdsp/boot", "1"));
        wait_online("cdsp", 30);
    }

    if (boot) {
        open_sound_nodes();
        lock_gpu_governor();
    }

    int modem_fd = -1;
    if (modem) {
        modem_fd = open("/dev/subsys_modem", O_RDONLY | O_CLOEXEC);
        klog("modem: open /dev/subsys_modem -> %d (%s)", modem_fd, modem_fd < 0 ? strerror(errno) : "ok");
        wait_online("modem", 60);
    }

    if (boot)
        klog("board data: %d", send_board_data(tooldir));

    if (wlan) {
        struct stat st;
        for (int i = 0; i < 4 && !stop; i++) {
            int r = write_file("/dev/wlan", "ON");
            klog("wlan: /dev/wlan ON (try %d) -> %d", i + 1, r);
            if (r == 0 || stat("/sys/class/net/wlan0", &st) == 0)
                break;
            sleep(5);
        }
        for (int i = 0; i < 20 && !stop; i++) {
            if (stat("/sys/class/net/wlan0", &st) == 0) {
                klog("wlan0 exists after %d s", i);
                break;
            }
            sleep(1);
        }
    }

    subsys_report("after");

    if (boot) {
        struct stat st;
        if (stat("/sys/class/net/wlan0", &st) == 0)
            start_wifi_stack();
        else
            klog("no wlan0, Wi-Fi services left alone");
    }

    // Hold /dev/subsys_modem open and report helpers that exit, until killed.
    while (!stop) {
        int st;
        pid_t p = waitpid(-1, &st, WNOHANG);
        if (p > 0)
            klog("pid %d exited (status 0x%x)", (int)p, st);
        sleep(2);
    }

    for (int i = 0; i < n_children; i++)
        kill(children[i], SIGTERM);
    if (modem_fd >= 0)
        close(modem_fd);
    klog("stopped");
    return 0;
}
