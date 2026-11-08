/*
 * hexagonrpc on a downstream msm-4.14 kernel (sweet): the upstream fastrpc
 * UAPI that hexagonrpcd is written for, translated to the downstream
 * "adsprpc" driver (/dev/adsprpc-smd-secure) by sweet_fastrpc_ioctl().
 * This header replaces <misc/fastrpc.h>.
 */
#ifndef SWEET_FASTRPC_COMPAT_H
#define SWEET_FASTRPC_COMPAT_H
#include <stdint.h>
#include <sys/ioctl.h>
#include <linux/types.h>

struct fastrpc_invoke_args {
	__u64 ptr;
	__u64 length;
	__s32 fd;
	__u32 attr;
};

struct fastrpc_invoke {
	__u32 handle;
	__u32 sc;
	__u64 args;
};

struct fastrpc_init_create {
	__u32 filelen;
	__s32 filefd;
	__u32 attrs;
	__u32 siglen;
	__u64 file;
};

struct fastrpc_alloc_dma_buf {
	__s32 fd;
	__u32 flags;
	__u64 size;
};

struct fastrpc_req_mmap {
	__s32 fd;
	__u32 flags;
	__u64 vaddrin;
	__u64 size;
	__u64 vaddrout;
};

/* request codes understood by sweet_fastrpc_ioctl() only */
#define FASTRPC_IOCTL_ALLOC_DMA_BUFF	0x5201
#define FASTRPC_IOCTL_INVOKE		0x5203
#define FASTRPC_IOCTL_INIT_ATTACH	0x5204
#define FASTRPC_IOCTL_INIT_CREATE	0x5205
#define FASTRPC_IOCTL_MMAP		0x5206
#define FASTRPC_IOCTL_INIT_ATTACH_SNS	0x5208

int sweet_fastrpc_ioctl(int fd, unsigned long req, ...);
#define ioctl sweet_fastrpc_ioctl

#endif
