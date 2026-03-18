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
 * All network calls use tlsdial() directly.
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

#define CLIENT_ID  "Iv1.b507a08c87ecfe98"
#define SCOPE      "read:user"

#define GITHUB_HOST     "github.com"
#define APIGITHUB_HOST  "api.github.com"
#define COPILOT_HOST    "api.individual.githubcopilot.com"

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
 * Returns freshly allocated body string (caller must free), or nil.
 */
static char *
postjson(char *host, char *path,
         HTTPHdr *hdrs, int nhdrs,
         char *body,
         jsmntok_t *toks, int maxtok, int *ntoks_out)
{
	HTTPConn *c;
	HTTPResp *r;
	jsmn_parser p;
	char *js;
	int n;

	c = tlsdial(host, "443");
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

/*
 * getjson — GET host/path, return parsed response body.
 */
static char *
getjson(char *host, char *path,
        HTTPHdr *hdrs, int nhdrs,
        jsmntok_t *toks, int maxtok, int *ntoks_out)
{
	HTTPConn *c;
	HTTPResp *r;
	jsmn_parser p;
	char *js;
	int n;

	c = tlsdial(host, "443");
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

typedef OAuthDeviceCode DeviceCode;

static int
startdeviceflow(DeviceCode *dc)
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
	hdrs[nhdrs].name  = "Accept";        hdrs[nhdrs].value = "application/json";         nhdrs++;
	hdrs[nhdrs].name  = "Content-Type";  hdrs[nhdrs].value = "application/json";         nhdrs++;
	hdrs[nhdrs].name  = "User-Agent";    hdrs[nhdrs].value = "GitHubCopilotChat/0.35.0"; nhdrs++;

	js = postjson(GITHUB_HOST, "/login/device/code",
	              hdrs, nhdrs, body, toks, MAXTOK, &ntoks);
	if(js == nil)
		return -1;

	vi = jsonget(js, toks, ntoks, 0, "device_code");
	if(vi < 0) { werrstr("oauth: no device_code"); free(js); return -1; }
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

static char *
pollaccesstoken(DeviceCode *dc)
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
	hdrs[nhdrs].name  = "Accept";        hdrs[nhdrs].value = "application/json";         nhdrs++;
	hdrs[nhdrs].name  = "Content-Type";  hdrs[nhdrs].value = "application/json";         nhdrs++;
	hdrs[nhdrs].name  = "User-Agent";    hdrs[nhdrs].value = "GitHubCopilotChat/0.35.0"; nhdrs++;

	snprint(body, sizeof body,
		"{\"client_id\":\"%s\","
		"\"device_code\":\"%s\","
		"\"grant_type\":\"urn:ietf:params:oauth:grant-type:device_code\"}",
		CLIENT_ID, dc->device_code);

	while(time(0) < deadline) {
		sleep(interval_ms / 1000);

		js = postjson(GITHUB_HOST, "/login/oauth/access_token",
		              hdrs, nhdrs, body, toks, MAXTOK, &ntoks);
		if(js == nil)
			return nil;

		vi = jsonget(js, toks, ntoks, 0, "access_token");
		if(vi >= 0) {
			char *tok = mallocz(toks[vi].end - toks[vi].start + 1, 1);
			if(tok == nil) { free(js); return nil; }
			jsonstr(js, &toks[vi], tok, toks[vi].end - toks[vi].start + 1);
			free(js);
			return tok;
		}

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

static int
writetoken(char *tokpath, char *token)
{
	int  fd;
	long n;

	fd = create(tokpath, OWRITE, 0600);
	if(fd < 0)
		return -1;
	n = strlen(token);
	if(write(fd, token, n) != n) {
		close(fd);
		return -1;
	}
	write(fd, "\n", 1);
	close(fd);
	return 0;
}

/* ── Public API ───────────────────────────────────────────────────────── */

int
oauthlogin(char *tokpath)
{
	DeviceCode dc;
	char *token;

	if(startdeviceflow(&dc) < 0)
		return -1;

	print("Open this URL in your browser:\n\n  %s\n\n", dc.verification_uri);
	print("Enter code: %s\n\n", dc.user_code);
	print("Waiting for authorization");
	{
		Biobuf *b = Bfdopen(1, OWRITE);
		if(b != nil) { Bflush(b); Bterm(b); }
	}

	token = pollaccesstoken(&dc);
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

OAuthToken *
oauthsession(char *refresh)
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
	hdrs[nhdrs].name  = "Accept";         hdrs[nhdrs].value = "application/json"; nhdrs++;
	hdrs[nhdrs].name  = "Authorization";  hdrs[nhdrs].value = authbuf;            nhdrs++;
	{
		int i;
		for(i = 0; i < NCOPILOT_HDRS && nhdrs < 8; i++)
			hdrs[nhdrs++] = copilot_hdrs[i];
	}

	js = getjson(APIGITHUB_HOST, "/copilot_internal/v2/token",
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
	if(vi >= 0) ot->expires_at = jsonint(js, &toks[vi]);

	vi = jsonget(js, toks, ntoks, 0, "refresh_in");
	if(vi >= 0) ot->refresh_in = jsonint(js, &toks[vi]);

	free(js);
	return ot;
}

void
oauthtokenfree(OAuthToken *t)
{
	if(t == nil) return;
	free(t->token);
	free(t);
}

int
oauthtokenexists(char *tokpath)
{
	int  fd;
	char buf[4];
	int  n;

	fd = open(tokpath, OREAD);
	if(fd < 0) return 0;
	n = read(fd, buf, sizeof buf);
	close(fd);
	return n > 0;
}

OAuthDeviceCode *
oauthdevicestart(void)
{
	OAuthDeviceCode *dc;

	dc = mallocz(sizeof *dc, 1);
	if(dc == nil) return nil;
	if(startdeviceflow(dc) < 0) {
		free(dc);
		return nil;
	}
	return dc;
}

int
oauthdevicepoll(OAuthDeviceCode *dc, char *tokpath, int *done)
{
	enum { MAXTOK = 64 };
	jsmntok_t toks[MAXTOK];
	char body[512];
	char errbuf[64];
	char *js;
	int ntoks, vi;
	HTTPHdr hdrs[4];
	int nhdrs;

	*done = 0;

	if(dc->expires_in > 0) {
		dc->expires_in -= dc->interval;
		if(dc->expires_in <= 0) {
			werrstr("oauth: device flow timed out");
			*done = 1;
			return -1;
		}
	}

	nhdrs = 0;
	hdrs[nhdrs].name  = "Accept";        hdrs[nhdrs].value = "application/json";         nhdrs++;
	hdrs[nhdrs].name  = "Content-Type";  hdrs[nhdrs].value = "application/json";         nhdrs++;
	hdrs[nhdrs].name  = "User-Agent";    hdrs[nhdrs].value = "GitHubCopilotChat/0.35.0"; nhdrs++;

	snprint(body, sizeof body,
		"{\"client_id\":\"%s\","
		"\"device_code\":\"%s\","
		"\"grant_type\":\"urn:ietf:params:oauth:grant-type:device_code\"}",
		CLIENT_ID, dc->device_code);

	js = postjson(GITHUB_HOST, "/login/oauth/access_token",
	              hdrs, nhdrs, body, toks, MAXTOK, &ntoks);
	if(js == nil) { *done = 1; return -1; }

	vi = jsonget(js, toks, ntoks, 0, "access_token");
	if(vi >= 0) {
		int   tlen = toks[vi].end - toks[vi].start;
		char *tok  = mallocz(tlen + 1, 1);
		if(tok == nil) { free(js); *done = 1; werrstr("malloc"); return -1; }
		jsonstr(js, &toks[vi], tok, tlen + 1);
		free(js);
		if(writetoken(tokpath, tok) < 0) {
			werrstr("oauth: write token: %r");
			free(tok);
			*done = 1;
			return -1;
		}
		free(tok);
		*done = 1;
		return 0;
	}

	vi = jsonget(js, toks, ntoks, 0, "error");
	if(vi >= 0) {
		jsonstr(js, &toks[vi], errbuf, sizeof errbuf);
		free(js);
		if(strcmp(errbuf, "authorization_pending") == 0)
			return 0;
		if(strcmp(errbuf, "slow_down") == 0) {
			dc->interval += 5;
			return 0;
		}
		werrstr("oauth: device flow failed: %s", errbuf);
		*done = 1;
		return -1;
	}

	free(js);
	return 0;
}

void
oauthdevcodefree(OAuthDeviceCode *dc)
{
	free(dc);
}

/*
 * oauthenablemodels — POST /models/<id>/policy {"state":"enabled"} for
 * every model in the Copilot /models list.
 *
 * Required after first login to unlock Claude and Grok models.
 * Errors are silently ignored — best-effort only.
 */
void
oauthenablemodels(char *session)
{
	HTTPConn *c;
	HTTPResp *r;
	char authbuf[1100];
	HTTPHdr hdrs[8];
	int nhdrs;
	enum { MAXTOK = 6000 };
	jsmntok_t *toks;
	jsmn_parser p;
	char *js;
	int ntoks;

	snprint(authbuf, sizeof authbuf, "Bearer %s", session);

	nhdrs = 0;
	hdrs[nhdrs].name  = "Authorization";          hdrs[nhdrs].value = authbuf;                    nhdrs++;
	hdrs[nhdrs].name  = "Content-Type";           hdrs[nhdrs].value = "application/json";          nhdrs++;
	hdrs[nhdrs].name  = "User-Agent";             hdrs[nhdrs].value = "GitHubCopilotChat/0.35.0";  nhdrs++;
	hdrs[nhdrs].name  = "Editor-Version";         hdrs[nhdrs].value = "vscode/1.107.0";            nhdrs++;
	hdrs[nhdrs].name  = "Editor-Plugin-Version";  hdrs[nhdrs].value = "copilot-chat/0.35.0";       nhdrs++;
	hdrs[nhdrs].name  = "Copilot-Integration-Id"; hdrs[nhdrs].value = "vscode-chat";               nhdrs++;

	c = tlsdial(COPILOT_HOST, "443");
	if(c == nil) return;
	r = httpget(c, "/models", COPILOT_HOST, hdrs, nhdrs);
	if(r == nil || r->code != 200) {
		if(r != nil) httprespfree(r);
		httpclose(c);
		return;
	}
	if(httpreadbody(r) < 0) {
		httprespfree(r);
		httpclose(c);
		return;
	}
	httpclose(c);
	js = r->body;
	r->body = nil;
	httprespfree(r);

	toks = mallocz(MAXTOK * sizeof(jsmntok_t), 1);
	if(toks == nil) { free(js); return; }
	jsmn_init(&p);
	ntoks = jsmn_parse(&p, js, strlen(js), toks, MAXTOK);
	if(ntoks < 0) { free(toks); free(js); return; }

	{
		int data_arr = jsonget(js, toks, ntoks, 0, "data");
		if(data_arr < 0 || toks[data_arr].type != JSMN_ARRAY) {
			free(toks); free(js); return;
		}
		int arr_size = toks[data_arr].size;
		int elem = data_arr + 1;
		int ei;

		for(ei = 0; ei < arr_size && elem < ntoks; ei++) {
			int id_vi = jsonget(js, toks, ntoks, elem, "id");
			if(id_vi >= 0) {
				char id_buf[128];
				char path[256];
				char body2[32];
				HTTPConn *c2;
				HTTPResp *r2;
				HTTPHdr hdrs2[8];
				int nh2;

				jsonstr(js, &toks[id_vi], id_buf, sizeof id_buf);
				snprint(path,  sizeof path,  "/models/%s/policy", id_buf);
				snprint(body2, sizeof body2, "{\"state\":\"enabled\"}");

				nh2 = 0;
				hdrs2[nh2].name  = "Authorization";          hdrs2[nh2].value = authbuf;                    nh2++;
				hdrs2[nh2].name  = "Content-Type";           hdrs2[nh2].value = "application/json";          nh2++;
				hdrs2[nh2].name  = "User-Agent";             hdrs2[nh2].value = "GitHubCopilotChat/0.35.0";  nh2++;
				hdrs2[nh2].name  = "Editor-Version";         hdrs2[nh2].value = "vscode/1.107.0";            nh2++;
				hdrs2[nh2].name  = "Editor-Plugin-Version";  hdrs2[nh2].value = "copilot-chat/0.35.0";       nh2++;
				hdrs2[nh2].name  = "Copilot-Integration-Id"; hdrs2[nh2].value = "vscode-chat";               nh2++;
				hdrs2[nh2].name  = "openai-intent";          hdrs2[nh2].value = "chat-policy";               nh2++;

				c2 = tlsdial(COPILOT_HOST, "443");
				if(c2 != nil) {
					r2 = httppost(c2, path, COPILOT_HOST,
					             hdrs2, nh2, body2, strlen(body2));
					if(r2 != nil) httprespfree(r2);
					httpclose(c2);
				}
			}
			/* advance past this array element and all its children */
			if(toks[elem].type == JSMN_OBJECT || toks[elem].type == JSMN_ARRAY) {
				int skip = elem + 1, depth = 1;
				while(skip < ntoks && depth > 0) {
					if(toks[skip].type == JSMN_OBJECT || toks[skip].type == JSMN_ARRAY)
						depth += toks[skip].size;
					else
						depth--;
					skip++;
				}
				elem = skip;
			} else {
				elem++;
			}
		}
	}

	free(toks);
	free(js);
}
