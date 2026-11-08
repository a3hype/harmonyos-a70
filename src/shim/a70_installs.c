/*
 * Starts the installs service with the code-signing shim preloaded.
 * Every argument is passed on unchanged, including the extra one init appends
 * when samgr loads the service on demand. Only argv[0] changes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef WRAP_TEST /* harmless test build: tools/wrap_test.sh */
#define TARGET "/system/bin/sh"
#define SHIM "/data/local/tmp/liba70noverity.so"
#else
#define TARGET "/system/bin/sa_main"
#define SHIM "/system/lib64/liba70noverity.so"
#endif

int main(int argc, char **argv)
{
	(void)argc;
	setenv("LD_PRELOAD", SHIM, 1);
	argv[0] = (char *)TARGET;
	execv(TARGET, argv);
	perror("a70-installs: execv");
	return 127;
}
