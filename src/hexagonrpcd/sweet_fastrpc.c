/* SPDX-License-Identifier: GPL-3.0-or-later (linked into hexagonrpcd)
 *
 * Upstream fastrpc ioctls -> downstream msm "adsprpc" ioctls (msm-4.14).
 * Only what hexagonrpcd needs to serve files to the ADSP sensors PD:
 * attach (root or sensors PD), invoke, and kernel-allocated mmap.
 */
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#undef ioctl
#include "misc/fastrpc.h"
#undef ioctl

/* downstream UAPI (drivers/char/adsprpc_shared.h) */
struct ds_remote_buf {
	void *pv;
	size_t len;
};
struct ds_invoke {
	uint32_t handle;
	uint32_t sc;
	struct ds_remote_buf *pra;
};
struct ds_init {
	uint32_t flags;
	uintptr_t file;
	uint32_t filelen;
	int32_t filefd;
	uintptr_t mem;
	uint32_t memlen;
	int32_t memfd;
};
struct ds_init_attrs {
	struct ds_init init;
	int attrs;
	unsigned int siglen;
};
struct ds_mmap {
	int fd;
	uint32_t flags;
	uintptr_t vaddrin;
	size_t size;
	uintptr_t vaddrout;
};
#define DS_IOCTL_INVOKE		_IOWR('R', 1, struct ds_invoke)
#define DS_IOCTL_MMAP		_IOWR('R', 2, struct ds_mmap)
#define DS_IOCTL_GETINFO	_IOWR('R', 8, uint32_t)
#define DS_IOCTL_INIT_ATTRS	_IOWR('R', 10, struct ds_init_attrs)
#define DS_INIT_ATTACH		0
#define DS_INIT_ATTACH_SENSORS	3
#define ADSP_DOMAIN_ID		0

static int attach(int fd, uint32_t flags)
{
	uint32_t info = ADSP_DOMAIN_ID;
	struct ds_init_attrs ia;

	if (ioctl(fd, DS_IOCTL_GETINFO, &info) < 0) {
		perror("adsprpc GETINFO");
		return -1;
	}
	memset(&ia, 0, sizeof(ia));
	ia.init.flags = flags;
	ia.init.filefd = -1;
	ia.init.memfd = -1;
	return ioctl(fd, DS_IOCTL_INIT_ATTRS, &ia);
}

static int invoke(int fd, struct fastrpc_invoke *inv)
{
	struct fastrpc_invoke_args *args = (struct fastrpc_invoke_args *)(uintptr_t)inv->args;
	uint32_t sc = inv->sc;
	unsigned n = ((sc >> 16) & 0xff) + ((sc >> 8) & 0xff) + ((sc >> 4) & 0xf) + (sc & 0xf);
	struct ds_remote_buf ra[256];
	struct ds_invoke di;

	for (unsigned i = 0; i < n && i < 256; i++) {
		ra[i].pv = (void *)(uintptr_t)args[i].ptr;
		ra[i].len = (size_t)args[i].length;
	}
	di.handle = inv->handle;
	di.sc = sc;
	di.pra = ra;
	return ioctl(fd, DS_IOCTL_INVOKE, &di);
}

static int map(int fd, struct fastrpc_req_mmap *m)
{
	struct ds_mmap dm;
	int r;

	memset(&dm, 0, sizeof(dm));
	dm.fd = m->fd;
	dm.flags = m->flags;
	dm.vaddrin = (uintptr_t)m->vaddrin;
	dm.size = (size_t)m->size;
	r = ioctl(fd, DS_IOCTL_MMAP, &dm);
	if (r == 0)
		m->vaddrout = dm.vaddrout;
	return r;
}

int sweet_fastrpc_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);

	switch (req) {
	case FASTRPC_IOCTL_INIT_ATTACH:
		return attach(fd, DS_INIT_ATTACH);
	case FASTRPC_IOCTL_INIT_ATTACH_SNS:
		return attach(fd, DS_INIT_ATTACH_SENSORS);
	case FASTRPC_IOCTL_INVOKE:
		return invoke(fd, arg);
	case FASTRPC_IOCTL_MMAP:
		return map(fd, arg);
	case FASTRPC_IOCTL_ALLOC_DMA_BUFF:
	case FASTRPC_IOCTL_INIT_CREATE:
		errno = EOPNOTSUPP;
		return -1;
	default:
		return ioctl(fd, req, arg);
	}
}
