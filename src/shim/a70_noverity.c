/*
 * Preloaded into the installs service only.
 *
 * This kernel has no HarmonyOS code-signing ioctl (0x408066c8), so enabling it on
 * an installed file fails with ENOTTY and the whole install is rolled back. The
 * kernel does not enforce code signing here either (no /dev/xpm, no /dev/code_sign).
 *
 * Two things are faked, and only for that case:
 *  - the enable ioctl reports success when the kernel answers ENOTTY;
 *  - FS_IOC_GETFLAGS reports FS_VERITY_FL for files whose enable was faked.
 * Everything else passes through. This is a bring-up stub: it removes a protection,
 * so it must not be used on anything but a test device.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define IOC_TYPE(req) (((unsigned)(req) >> 8) & 0xff)
#define IOC_NR(req) ((unsigned)(req) & 0xff)
#define IOC_READ_DIR(req) ((((unsigned)(req) >> 30) & 3) == 2)
#define NR_GETFLAGS 1
#define FS_VERITY_FL 0x00100000
#define TABLE 512

static struct {
	unsigned long dev, ino;
} faked[TABLE];
static unsigned next_slot;
static int table_lock;
static int unknown_logged;

static void lock(void)
{
	while (__atomic_exchange_n(&table_lock, 1, __ATOMIC_ACQUIRE))
		;
}

static void unlock(void)
{
	__atomic_store_n(&table_lock, 0, __ATOMIC_RELEASE);
}

static void note(const char *msg)
{
	int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {
		write(fd, msg, strlen(msg));
		close(fd);
	}
}

__attribute__((constructor)) static void loaded(void)
{
	note("a70-noverity: loaded v2\n");
}

static void remember(int fd)
{
	struct stat st;
	if (fstat(fd, &st))
		return;
	lock();
	faked[next_slot].dev = st.st_dev;
	faked[next_slot].ino = st.st_ino;
	next_slot = (next_slot + 1) % TABLE;
	unlock();
}

static int was_faked(int fd)
{
	struct stat st;
	int found = 0;
	if (fstat(fd, &st))
		return 0;
	lock();
	for (int i = 0; i < TABLE && !found; i++)
		found = faked[i].ino == st.st_ino && faked[i].dev == st.st_dev;
	unlock();
	return found;
}

int ioctl(int fd, int req, ...)
{
	va_list ap;
	void *arg;
	long r;
	int saved;
	char line[64];

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);

	r = syscall(SYS_ioctl, fd, (unsigned long)(unsigned)req, arg);
	if (IOC_TYPE(req) != 'f')
		return (int)r;
	saved = errno;

	if (r == -1 && saved == ENOTTY) {
		remember(fd);
		snprintf(line, sizeof(line), "a70-noverity: stub ioctl %#x\n", (unsigned)req);
		note(line);
		errno = 0;
		return 0;
	}
	if (r == 0 && IOC_NR(req) == NR_GETFLAGS && IOC_READ_DIR(req) && arg && was_faked(fd)) {
		*(int *)arg |= FS_VERITY_FL;
		note("a70-noverity: verity flag reported\n");
		return 0;
	}
	if (__atomic_fetch_add(&unknown_logged, 1, __ATOMIC_RELAXED) < 16) {
		snprintf(line, sizeof(line), "a70-noverity: pass ioctl %#x r=%ld\n", (unsigned)req, r);
		note(line);
	}
	errno = saved;
	return (int)r;
}
