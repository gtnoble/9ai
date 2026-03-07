/*
 * httptest.c — Stage 2 test: GET /models from the Copilot API via 9aitls
 *
 * Requires:
 *   - 9aitls running:  ./9aitls &
 *   - token file:      ~/lib/9ai/token  (or -t path)
 *   - proxy socket:    ~/lib/9ai/proxy.sock  (or -s path)
 *
 * Usage:
 *   mk o.httptest && ./o.httptest [-t tokenfile] [-s sockpath]
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>

#include "9ai.h"
#include "http.h"

#define COPILOT_HOST  "api.individual.githubcopilot.com"
#define DEFAULT_SOCK  "proxy.sock"   /* relative to configpath dir */

static char *
readfile(char *path)
{
	char buf[4096];
	int  fd;
	long n;

	fd = open(path, OREAD);
	if(fd < 0)
		sysfatal("open %s: %r", path);
	n = readn(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		sysfatal("empty file: %s", path);
	buf[n] = '\0';
	while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
		buf[--n] = '\0';
	return strdup(buf);
}

void
threadmain(int argc, char *argv[])
{
	char     *tokfile, *sockfile;
	char     *token;
	char      auth[4096];
	HTTPConn *c;
	HTTPResp *r;
	HTTPHdr   hdrs[8];
	int       nhdrs;

	tokfile  = nil;
	sockfile = nil;

	ARGBEGIN{
	case 't':
		tokfile = ARGF();
		break;
	case 's':
		sockfile = ARGF();
		break;
	}ARGEND

	USED(argc); USED(argv);

	if(tokfile == nil)
		tokfile = configpath("token");
	if(sockfile == nil)
		sockfile = configpath("proxy.sock");

	token = readfile(tokfile);
	snprint(auth, sizeof auth, "Bearer %s", token);
	free(token);

	print("dialing proxy %s...\n", sockfile);
	c = httpdial(sockfile);
	if(c == nil)
		sysfatal("httpdial: %r");
	print("connected\n");

	nhdrs = 0;
	hdrs[nhdrs].name  = "Authorization";
	hdrs[nhdrs].value = auth;                        nhdrs++;
	hdrs[nhdrs].name  = "Content-Type";
	hdrs[nhdrs].value = "application/json";          nhdrs++;
	hdrs[nhdrs].name  = "User-Agent";
	hdrs[nhdrs].value = "GitHubCopilotChat/0.35.0";  nhdrs++;
	hdrs[nhdrs].name  = "Editor-Version";
	hdrs[nhdrs].value = "vscode/1.107.0";            nhdrs++;
	hdrs[nhdrs].name  = "Editor-Plugin-Version";
	hdrs[nhdrs].value = "copilot-chat/0.35.0";       nhdrs++;
	hdrs[nhdrs].name  = "Copilot-Integration-Id";
	hdrs[nhdrs].value = "vscode-chat";               nhdrs++;

	print("GET /models...\n");
	r = httpget(c, "/models", COPILOT_HOST, hdrs, nhdrs);
	if(r == nil)
		sysfatal("httpget: %r");
	print("HTTP %d\n", r->code);

	if(httpreadbody(r) < 0)
		sysfatal("httpreadbody: %r");

	write(1, r->body, r->bodylen);
	write(1, "\n", 1);

	httprespfree(r);
	httpclose(c);
	threadexitsall(nil);
}
