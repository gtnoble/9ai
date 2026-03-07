/*
 * main.c — 9ai entry point (stub)
 *
 * Stage 1: verify plan9port build, port.c helpers only.
 * threadmain is required by libthread; real logic added in later stages.
 */

#include <u.h>
#include <libc.h>
#include <thread.h>

#include "9ai.h"

static void
usage(void)
{
	fprint(2, "usage: 9ai [-m mtpt] [-s srvname]\n");
	threadexitsall("usage");
}

void
threadmain(int argc, char *argv[])
{
	char *mtpt = nil;
	char *srvname = "9ai";
	char *p;

	ARGBEGIN{
	case 'm':
		mtpt = ARGF();
		break;
	case 's':
		srvname = ARGF();
		break;
	default:
		usage();
	}ARGEND

	USED(mtpt);
	USED(srvname);

	/* Stage 1 smoke test: exercise port.c helpers */
	p = homedir();
	print("homedir: %s\n", p);
	free(p);

	p = configpath("token");
	print("configpath(token): %s\n", p);
	free(p);

	p = configpath("sessions/");
	print("configpath(sessions/): %s\n", p);
	free(p);

	threadexitsall(nil);
}
