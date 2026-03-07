/*
 * oauth.c — GitHub Copilot OAuth device flow
 *
 * See oauth.h for the interface.
 *
 * Endpoints used:
 *   github.com         POST /login/device/code
 *   github.com         POST /login/oauth/access_token
 *   api.github.com     GET  /copilot_internal/v2/token
 *
 * All three go through the 9aitls Unix-socket TLS proxy.
 * The proxy routes to the correct upstream via the Host: header.
 *
 * Client ID is the standard GitHub Copilot VS Code client.
 * Scope: "read:user" (minimum required for Copilot).
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>

#include "9ai.h"
#include "http.h"
#include "json.h"
#include "oauth.h"

/* GitHub Copilot VS Code client ID (base64 of "Iv1.b507a08c87ecfe98") */
#define CLIENT_ID  "Iv1.b507a08c87ecfe98"
#define SCOPE      "read:user"

#define GITHUB_HOST     "github.com"
#define APIGITHUB_HOST  "api.github.com"

/* Standard Copilot headers required by the API */
static HTTPHdr copilot_hdrs[] = {
	{ "User-Agent",             "GitHubCopilotChat/0.35.0" },
	{ "Editor-Version",         "vscode/1.107.0"           },
	{ "Editor-Plugin-Version",  "copilot-chat/0.35.0"      },
	{ "Copilot-Integration-Id", "vscode-chat"              },
};
enum { NCOPILOT_HDRS = nelem(copilot_hdrs) };

/* ── Internal helpers ─────────────────────────────────────────────────── */

/*
 * postjson — POST a JSON body to host/path, return parsed response.
 * toks and ntoks are output: caller provides the token array.
 * Returns freshly allocated body string (caller must free), or nil.
 */
static char *
postjson(char *sockpath, char *host, char *path,
         HTTPHdr *hdrs, int nhdrs,
         char *body,
         jsmntok_t *toks, int maxtok, int *ntoks_out)
{
	HTTPConn *c;
	HTTPResp *r;
	jsmn_parser p;
	char *js;
	int n;

	c = portdial(host, "443", sockpath);
	if(c == nil)
		return nil;

	r = httppost(c, path, host, hdrs, nhdrs, body, strlen(body));
	if(r == nil) {
		httpclose(c);
		return nil;
	}
	if(httpreadbody(r) < 0) {
		httprespfree(r);
		httpclose(c);
		return nil;
	}
	js = r->body;
	r->body = nil;   /* take ownership */
	httprespfree(r);
	httpclose(c);

	jsmn_init(&p);
	n = jsmn_parse(&p, js, strlen(js), toks, maxtok);
	if(n < 0) {
		werrstr("oauth: JSON parse error %d: %.80s", n, js);
		free(js);
		return nil;
	}
	*ntoks_out = n;
	return js;
}

/*
 * getjson — GET host/path, return parsed response body.
 * Same ownership semantics as postjson.
 */
static char *
getjson(char *sockpath, char *host, char *path,
        HTTPHdr *hdrs, int nhdrs,
        jsmntok_t *toks, int maxtok, int *ntoks_out)
{
	HTTPConn *c;
	HTTPResp *r;
	jsmn_parser p;
	char *js;
	int n;

	c = portdial(host, "443", sockpath);
	if(c == nil)
		return nil;

	r = httpget(c, path, host, hdrs, nhdrs);
	if(r == nil) {
		httpclose(c);
		return nil;
	}
	if(httpreadbody(r) < 0) {
		httprespfree(r);
		httpclose(c);
		return nil;
	}
	js = r->body;
	r->body = nil;
	httprespfree(r);
	httpclose(c);

	jsmn_init(&p);
	n = jsmn_parse(&p, js, strlen(js), toks, maxtok);
	if(n < 0) {
		werrstr("oauth: JSON parse error %d: %.80s", n, js);
		free(js);
		return nil;
	}
	*ntoks_out = n;
	return js;
}

/* ── Phase 1: device code flow ────────────────────────────────────────── */

typedef struct DeviceCode DeviceCode;
struct DeviceCode {
	char device_code[256];
	char user_code[64];
	char verification_uri[256];
	long interval;
	long expires_in;
};

static int
startdeviceflow(char *sockpath, DeviceCode *dc)
{
	enum { MAXTOK = 64 };
	jsmntok_t toks[MAXTOK];
	int ntoks, vi;
	char body[256];
	char *js;
	HTTPHdr hdrs[4];
	int nhdrs;

	snprint(body, sizeof body,
		"{\"client_id\":\"%s\",\"scope\":\"%s\"}",
		CLIENT_ID, SCOPE);

	nhdrs = 0;
	hdrs[nhdrs].name  = "Accept";
	hdrs[nhdrs].value = "application/json";       nhdrs++;
	hdrs[nhdrs].name  = "Content-Type";
	hdrs[nhdrs].value = "application/json";       nhdrs++;
	hdrs[nhdrs].name  = "User-Agent";
	hdrs[nhdrs].value = "GitHubCopilotChat/0.35.0"; nhdrs++;

	js = postjson(sockpath, GITHUB_HOST, "/login/device/code",
	              hdrs, nhdrs, body, toks, MAXTOK, &ntoks);
	if(js == nil)
		return -1;

	vi = jsonget(js, toks, ntoks, 0, "device_code");
	if(vi < 0) { werrstr("oauth: no device_code in response"); free(js); return -1; }
	jsonstr(js, &toks[vi], dc->device_code, sizeof dc->device_code);

	vi = jsonget(js, toks, ntoks, 0, "user_code");
	if(vi < 0) { werrstr("oauth: no user_code"); free(js); return -1; }
	jsonstr(js, &toks[vi], dc->user_code, sizeof dc->user_code);

	vi = jsonget(js, toks, ntoks, 0, "verification_uri");
	if(vi < 0) { werrstr("oauth: no verification_uri"); free(js); return -1; }
	jsonstr(js, &toks[vi], dc->verification_uri, sizeof dc->verification_uri);

	vi = jsonget(js, toks, ntoks, 0, "interval");
	if(vi < 0) { werrstr("oauth: no interval"); free(js); return -1; }
	dc->interval = jsonint(js, &toks[vi]);

	vi = jsonget(js, toks, ntoks, 0, "expires_in");
	if(vi < 0) { werrstr("oauth: no expires_in"); free(js); return -1; }
	dc->expires_in = jsonint(js, &toks[vi]);

	free(js);
	return 0;
}

/*
 * pollaccesstoken — poll until GitHub grants the token.
 * Returns allocated token string on success, nil on error/timeout.
 */
static char *
pollaccesstoken(char *sockpath, DeviceCode *dc)
{
	enum { MAXTOK = 64 };
	jsmntok_t toks[MAXTOK];
	char body[512];
	char errbuf[64];
	char *js;
	int ntoks, vi;
	long interval_ms, deadline;
	HTTPHdr hdrs[4];
	int nhdrs;

	interval_ms = dc->interval > 0 ? dc->interval * 1000 : 5000;
	deadline = time(0) + dc->expires_in;

	nhdrs = 0;
	hdrs[nhdrs].name  = "Accept";
	hdrs[nhdrs].value = "application/json";         nhdrs++;
	hdrs[nhdrs].name  = "Content-Type";
	hdrs[nhdrs].value = "application/json";         nhdrs++;
	hdrs[nhdrs].name  = "User-Agent";
	hdrs[nhdrs].value = "GitHubCopilotChat/0.35.0"; nhdrs++;

	snprint(body, sizeof body,
		"{\"client_id\":\"%s\","
		"\"device_code\":\"%s\","
		"\"grant_type\":\"urn:ietf:params:oauth:grant-type:device_code\"}",
		CLIENT_ID, dc->device_code);

	while(time(0) < deadline) {
		sleep(interval_ms / 1000);

		js = postjson(sockpath, GITHUB_HOST,
		              "/login/oauth/access_token",
		              hdrs, nhdrs, body, toks, MAXTOK, &ntoks);
		if(js == nil)
			return nil;

		/* success: access_token present */
		vi = jsonget(js, toks, ntoks, 0, "access_token");
		if(vi >= 0) {
			char *tok = mallocz(toks[vi].end - toks[vi].start + 1, 1);
			if(tok == nil) { free(js); return nil; }
			jsonstr(js, &toks[vi], tok, toks[vi].end - toks[vi].start + 1);
			free(js);
			return tok;
		}

		/* error field */
		vi = jsonget(js, toks, ntoks, 0, "error");
		if(vi >= 0) {
			jsonstr(js, &toks[vi], errbuf, sizeof errbuf);
			free(js);

			if(strcmp(errbuf, "authorization_pending") == 0)
				continue;

			if(strcmp(errbuf, "slow_down") == 0) {
				interval_ms += 5000;
				continue;
			}

			werrstr("oauth: device flow failed: %s", errbuf);
			return nil;
		}

		free(js);
	}

	werrstr("oauth: device flow timed out");
	return nil;
}

/* ── writetoken — persist token to file ────────────────────────────────── */

static int
writetoken(char *tokpath, char *token)
{
	int fd;
	long n;

	/* Create with mode 0600 */
	fd = create(tokpath, OWRITE, 0600);
	if(fd < 0)
		return -1;
	n = strlen(token);
	if(write(fd, token, n) != n) {
		close(fd);
		return -1;
	}
	/* trailing newline for readability */
	write(fd, "\n", 1);
	close(fd);
	return 0;
}

/* ── oauthlogin ──────────────────────────────────────────────────────── */

int
oauthlogin(char *sockpath, char *tokpath)
{
	DeviceCode dc;
	char *token;

	if(startdeviceflow(sockpath, &dc) < 0)
		return -1;

	print("Open this URL in your browser:\n\n  %s\n\n", dc.verification_uri);
	print("Enter code: %s\n\n", dc.user_code);
	print("Waiting for authorization");
	/* flush stdout so the message appears before we block in the poll loop */
	{
		Biobuf *b = Bfdopen(1, OWRITE);
		if(b != nil) { Bflush(b); Bterm(b); }
	}

	token = pollaccesstoken(sockpath, &dc);
	if(token == nil) {
		print("\n");
		return -1;
	}
	print("\nAuthorized.\n");

	if(writetoken(tokpath, token) < 0) {
		werrstr("oauth: write token to %s: %r", tokpath);
		free(token);
		return -1;
	}
	print("Token saved to %s\n", tokpath);
	free(token);
	return 0;
}

/* ── oauthsession ─────────────────────────────────────────────────────── */

OAuthToken *
oauthsession(char *refresh, char *sockpath)
{
	enum { MAXTOK = 128 };
	jsmntok_t toks[MAXTOK];
	char *js;
	int ntoks, vi;
	char authbuf[512];
	char tokbuf[1024];
	OAuthToken *ot;
	HTTPHdr hdrs[8];
	int nhdrs;

	snprint(authbuf, sizeof authbuf, "Bearer %s", refresh);

	nhdrs = 0;
	hdrs[nhdrs].name  = "Accept";
	hdrs[nhdrs].value = "application/json";  nhdrs++;
	hdrs[nhdrs].name  = "Authorization";
	hdrs[nhdrs].value = authbuf;             nhdrs++;
	/* append standard Copilot headers */
	{
		int i;
		for(i = 0; i < NCOPILOT_HDRS && nhdrs < 8; i++)
			hdrs[nhdrs++] = copilot_hdrs[i];
	}

	js = getjson(sockpath, APIGITHUB_HOST,
	             "/copilot_internal/v2/token",
	             hdrs, nhdrs, toks, MAXTOK, &ntoks);
	if(js == nil)
		return nil;

	vi = jsonget(js, toks, ntoks, 0, "token");
	if(vi < 0) {
		werrstr("oauthsession: no token field in response");
		free(js);
		return nil;
	}
	jsonstr(js, &toks[vi], tokbuf, sizeof tokbuf);

	ot = mallocz(sizeof *ot, 1);
	if(ot == nil) { free(js); return nil; }

	ot->token = strdup(tokbuf);

	vi = jsonget(js, toks, ntoks, 0, "expires_at");
	if(vi >= 0)
		ot->expires_at = jsonint(js, &toks[vi]);

	vi = jsonget(js, toks, ntoks, 0, "refresh_in");
	if(vi >= 0)
		ot->refresh_in = jsonint(js, &toks[vi]);

	free(js);
	return ot;
}

/* ── oauthtokenfree ───────────────────────────────────────────────────── */

void
oauthtokenfree(OAuthToken *t)
{
	if(t == nil)
		return;
	free(t->token);
	free(t);
}
