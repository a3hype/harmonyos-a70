/* Forced include for the OpenHarmony SDK sysroot: its linux/socket.h defines
 * struct sockaddr_storage, which clashes with sys/socket.h. Keep only the type
 * that linux/qrtr.h needs and skip that header. */
#ifndef A70_COMPAT_H
#define A70_COMPAT_H
#define _UAPI_LINUX_SOCKET_H
typedef unsigned short __kernel_sa_family_t;
/* glibc-style macro the sources rely on. Without it, `struct x {...} __packed;` silently
 * declares a variable named __packed and leaves wire-format structs unpacked. */
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#endif
