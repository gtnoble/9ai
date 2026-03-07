/*
 * oauthtest.c — Stage 4 test: exercise oauthsession and optionally oauthlogin
 *
 * Usage:
 *   mk o.oauthtest
 *
 *   # Test session token exchange (requires existing ~/lib/9ai/token):
 *   ./o.oauthtest -s ~/.cache/9ai/proxy.sock [-t tokenfile]
 *
 *   # Test full device flow login (interactive, writes new token):
 *   ./o.oauthtest -s ~/.cache/9ai/proxy.sock -l [-t tokenfile]
 *
 * The session test prints the session token fields and verifies that a
 * subsequent GET /models with the session token returns HTTP 200.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>

#include "9ai.h"
#include "http.h"
#include "json.h"
#include "oauth.h"

#define COPILOT_HOST "api.individual.githubcopilot.com"

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
		sysfatal("empty: %s", path);
	buf[n] = '\0';
	while(n > 0 && (buf[n-1]=='\n'||buf[n-1]=='\r'||buf[n-1]==' '))
		buf[--n] = '\0';
	return strdup(buf);
}

static void
test_session(char *sockpath, char *tokpath)
{
	OAuthToken *ot;
	char *refresh;
	char auth[1200];
	HTTPConn *c;
	HTTPResp *r;
	HTTPHdr hdrs[8];
	int nhdrs;

	print("=== oauthsession test ===\n");

	refresh = readfile(tokpath);
	print("refresh token: %.20s...\n", refresh);

	ot = oauthsession(refresh, sockpath);
	if(ot == nil)
		sysfatal("oauthsession: %r");

	print("session token: %.40s...\n", ot->token);
	print("expires_at:    %ld\n", ot->expires_at);
	print("refresh_in:    %ld\n", ot->refresh_in);

	/* Verify: use session token to GET /models */
	snprint(auth, sizeof auth, "Bearer %s", ot->token);
	nhdrs = 0;
	hdrs[nhdrs].name  = "Authorization";       hdrs[nhdrs].value = auth;                        nhdrs++;
	hdrs[nhdrs].name  = "Content-Type";        hdrs[nhdrs].value = "application/json";          nhdrs++;
	hdrs[nhdrs].name  = "User-Agent";          hdrs[nhdrs].value = "GitHubCopilotChat/0.35.0";  nhdrs++;
	hdrs[nhdrs].name  = "Editor-Version";      hdrs[nhdrs].value = "vscode/1.107.0";            nhdrs++;
	hdrs[nhdrs].name  = "Editor-Plugin-Version"; hdrs[nhdrs].value = "copilot-chat/0.35.0";     nhdrs++;
	hdrs[nhdrs].name  = "Copilot-Integration-Id"; hdrs[nhdrs].value = "vscode-chat";            nhdrs++;

	c = portdial("api.individual.githubcopilot.com", "443", sockpath);
	if(c == nil)
		sysfatal("httpdial: %r");

	r = httpget(c, "/models", COPILOT_HOST, hdrs, nhdrs);
	if(r == nil)
		sysfatal("httpget /models: %r");
	print("GET /models → HTTP %d\n", r->code);
	if(r->code != 200)
		sysfatal("expected 200, got %d", r->code);

	/* just confirm it's valid JSON with a "data" array */
	if(httpreadbody(r) < 0)
		sysfatal("httpreadbody: %r");
	if(strstr(r->body, "\"data\"") == nil)
		sysfatal("no 'data' key in /models response");
	print("response contains 'data' array: ok\n");

	httprespfree(r);
	httpclose(c);
	oauthtokenfree(ot);
	free(refresh);
	print("session test passed\n\n");
}

void
threadmain(int argc, char *argv[])
{
	char *sockpath, *tokpath;
	int   do_login;

	sockpath  = nil;
	tokpath   = nil;
	do_login  = 0;

	ARGBEGIN{
	case 's':
		sockpath = ARGF();
		break;
	case 't':
		tokpath = ARGF();
		break;
	case 'l':
		do_login = 1;
		break;
	}ARGEND

	USED(argv);

	if(sockpath == nil)
		sockpath = configpath("proxy.sock");
	if(tokpath == nil)
		tokpath = configpath("token");

	if(do_login) {
		print("=== oauthlogin test ===\n");
		if(oauthlogin(sockpath, tokpath) < 0)
			sysfatal("oauthlogin: %r");
		print("login test passed\n\n");
	}

	test_session(sockpath, tokpath);
	threadexitsall(nil);
}
