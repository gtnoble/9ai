/*
 * main.c — 9ai entry point
 *
 * Reads configuration from environment / command-line flags,
 * initialises global state via aiinit(), and hands off to aimain()
 * which posts the 9P service and never returns.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#include "9ai.h"
#include "http.h"
#include "json.h"
#include "oauth.h"
#include "models.h"
#include "sse.h"
#include "oai.h"
#include "exec.h"
#include "agent.h"
#include "fs.h"

static void
usage(void)
{
	fprint(2, "usage: 9ai [-m mtpt] [-s srvname] [-S sockpath] [-t tokpath] [-M model]\n");
	threadexitsall("usage");
}

void
threadmain(int argc, char *argv[])
{
	char *mtpt    = nil;
	char *srvname = "9ai";
	char *sockpath = nil;
	char *tokpath  = nil;
	char *model    = "gpt-4o";

	ARGBEGIN{
	case 'm':
		mtpt = ARGF();
		break;
	case 's':
		srvname = ARGF();
		break;
	case 'S':
		sockpath = ARGF();
		break;
	case 't':
		tokpath = ARGF();
		break;
	case 'M':
		model = ARGF();
		break;
	default:
		usage();
	}ARGEND

	if(sockpath == nil) {
		char *h = homedir();
		sockpath = smprint("%s/.cache/9ai/proxy.sock", h);
		free(h);
	}
	if(tokpath == nil) {
		char *h = homedir();
		tokpath = smprint("%s/.cache/9ai/token", h);
		free(h);
	}

	AiState *ai = aiinit(model, sockpath, tokpath);
	aimain(ai, srvname, mtpt);

	/* aimain returns after posting; block here so the process stays alive */
	for(;;)
		sleep(3600);

	/* not reached */
	threadexitsall(nil);
}
