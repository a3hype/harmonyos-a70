// a70touch: bridges the Galaxy A70's sec_touchscreen to a virtual touchscreen.
//
// The Samsung driver reports only multi-touch axes: no BTN_TOUCH and no ABS_X/ABS_Y.
// This creates a uinput device that adds them, for input stacks that need them.
//
// Usage: a70touch [-g] [-v]
//   -g  grab the source device exclusively (also hides it from kernel handlers)
//   -v  log each finger down/up to stderr
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_SLOTS 10

struct slot {
    int id; // tracking id, -1 when no finger
    int x;
    int y;
};

static struct slot slots[MAX_SLOTS];
static volatile sig_atomic_t stop;
static int uinput_fd = -1;

static void on_signal(int s)
{
    (void)s;
    stop = 1;
}

static void emit_event(unsigned short type, unsigned short code, int value)
{
    struct input_event e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.code = code;
    e.value = value;
    if (write(uinput_fd, &e, sizeof(e)) != (ssize_t)sizeof(e))
        fprintf(stderr, "a70touch: uinput write: %s\n", strerror(errno));
}

// Writes to stderr and the kernel log, since a boot service has no visible stderr.
static void logmsg(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    int k = open("/dev/kmsg", O_WRONLY);
    if (k >= 0) {
        dprintf(k, "%s\n", msg);
        close(k);
    }
}

static void die(const char *what)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "a70touch: %s: %s", what, strerror(errno));
    logmsg(buf);
    exit(1);
}

// Finds the touchscreen by name, so a change in event numbering can't break it.
static int find_source(char *path, size_t path_size)
{
    for (int i = 0; i < 32; i++) {
        snprintf(path, path_size, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        char name[64] = "";
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 &&
            !strcmp(name, "sec_touchscreen"))
            return fd;
        close(fd);
    }
    return -1;
}

static void set_abs(struct uinput_user_dev *d, int code, int min, int max)
{
    d->absmin[code] = min;
    d->absmax[code] = max;
    if (ioctl(uinput_fd, UI_SET_ABSBIT, code) < 0)
        die("UI_SET_ABSBIT");
}

int main(int argc, char **argv)
{
    char path[32] = "";
    int grab = 0, verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-g"))
            grab = 1;
        else if (!strcmp(argv[i], "-v"))
            verbose = 1;
    }

    // At boot the device nodes may not exist yet, so keep looking for up to 2 minutes.
    int src = -1;
    for (int tries = 0; src < 0 && tries < 120; tries++) {
        src = find_source(path, sizeof(path));
        if (src < 0)
            sleep(1);
    }
    if (src < 0) {
        logmsg("a70touch: sec_touchscreen not found");
        return 2;
    }

    struct input_absinfo ax, ay, am;
    if (ioctl(src, EVIOCGABS(ABS_MT_POSITION_X), &ax) < 0 ||
        ioctl(src, EVIOCGABS(ABS_MT_POSITION_Y), &ay) < 0)
        die("EVIOCGABS position");
    int have_major = ioctl(src, EVIOCGABS(ABS_MT_TOUCH_MAJOR), &am) == 0;

    for (int tries = 0; uinput_fd < 0 && tries < 120; tries++) {
        uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (uinput_fd < 0)
            sleep(1);
    }
    if (uinput_fd < 0)
        die("open /dev/uinput");

    if (ioctl(uinput_fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0 ||
        ioctl(uinput_fd, UI_SET_EVBIT, EV_SYN) < 0 ||
        ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(uinput_fd, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(uinput_fd, UI_SET_KEYBIT, BTN_TOUCH) < 0 ||
        ioctl(uinput_fd, UI_SET_KEYBIT, BTN_TOOL_FINGER) < 0)
        die("uinput capabilities");

    struct uinput_user_dev dev;
    memset(&dev, 0, sizeof(dev));
    snprintf(dev.name, sizeof(dev.name), "a70_touchscreen");
    dev.id.bustype = BUS_VIRTUAL;
    dev.id.vendor = 0x04e8;
    dev.id.product = 0xa705;
    dev.id.version = 1;
    set_abs(&dev, ABS_X, ax.minimum, ax.maximum);
    set_abs(&dev, ABS_Y, ay.minimum, ay.maximum);
    set_abs(&dev, ABS_MT_SLOT, 0, MAX_SLOTS - 1);
    set_abs(&dev, ABS_MT_TRACKING_ID, 0, 65535);
    set_abs(&dev, ABS_MT_POSITION_X, ax.minimum, ax.maximum);
    set_abs(&dev, ABS_MT_POSITION_Y, ay.minimum, ay.maximum);
    if (have_major)
        set_abs(&dev, ABS_MT_TOUCH_MAJOR, am.minimum, am.maximum);

    if (write(uinput_fd, &dev, sizeof(dev)) != (ssize_t)sizeof(dev))
        die("write uinput_user_dev");
    if (ioctl(uinput_fd, UI_DEV_CREATE) < 0)
        die("UI_DEV_CREATE");

    if (grab && ioctl(src, EVIOCGRAB, 1) < 0)
        die("EVIOCGRAB");

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    for (int i = 0; i < MAX_SLOTS; i++)
        slots[i].id = -1;

    char started[128];
    snprintf(started, sizeof(started), "a70touch: bridging %s (x %d-%d, y %d-%d) grab=%d",
             path, ax.minimum, ax.maximum, ay.minimum, ay.maximum, grab);
    logmsg(started);

    int cur = 0;
    int was_down = 0;
    unsigned long downs = 0;
    struct input_event ev;

    while (!stop) {
        ssize_t n = read(src, &ev, sizeof(ev));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            die("read source");
        }
        if (n != (ssize_t)sizeof(ev))
            continue;

        if (ev.type == EV_ABS) {
            switch (ev.code) {
            case ABS_MT_SLOT:
                cur = ev.value;
                if (cur >= 0 && cur < MAX_SLOTS)
                    emit_event(EV_ABS, ABS_MT_SLOT, cur);
                break;
            case ABS_MT_TRACKING_ID:
                if (cur >= 0 && cur < MAX_SLOTS) {
                    slots[cur].id = ev.value;
                    emit_event(EV_ABS, ABS_MT_TRACKING_ID, ev.value);
                }
                break;
            case ABS_MT_POSITION_X:
                if (cur >= 0 && cur < MAX_SLOTS) {
                    slots[cur].x = ev.value;
                    emit_event(EV_ABS, ABS_MT_POSITION_X, ev.value);
                }
                break;
            case ABS_MT_POSITION_Y:
                if (cur >= 0 && cur < MAX_SLOTS) {
                    slots[cur].y = ev.value;
                    emit_event(EV_ABS, ABS_MT_POSITION_Y, ev.value);
                }
                break;
            case ABS_MT_TOUCH_MAJOR:
                if (have_major && cur >= 0 && cur < MAX_SLOTS)
                    emit_event(EV_ABS, ABS_MT_TOUCH_MAJOR, ev.value);
                break;
            default:
                break; // pressure and vendor axes are not forwarded
            }
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            int first = -1;
            for (int i = 0; i < MAX_SLOTS; i++) {
                if (slots[i].id >= 0) {
                    first = i;
                    break;
                }
            }
            int down = first >= 0;
            if (down) {
                emit_event(EV_ABS, ABS_X, slots[first].x);
                emit_event(EV_ABS, ABS_Y, slots[first].y);
            }
            if (down != was_down) {
                emit_event(EV_KEY, BTN_TOUCH, down);
                emit_event(EV_KEY, BTN_TOOL_FINGER, down);
                if (down)
                    downs++;
                if (verbose)
                    fprintf(stderr, "a70touch: %s #%lu x=%d y=%d\n", down ? "down" : "up",
                            downs, down ? slots[first].x : 0, down ? slots[first].y : 0);
                was_down = down;
            }
            emit_event(EV_SYN, SYN_REPORT, 0);
        }
    }

    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    if (grab)
        ioctl(src, EVIOCGRAB, 0);
    close(src);
    fprintf(stderr, "a70touch: stopped after %lu touches\n", downs);
    return 0;
}
