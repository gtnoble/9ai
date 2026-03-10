/*
 * oaitest.c — unit tests for the OAI Completions request builder and
 *             SSE delta parser (oai.c)
 *
 * Part 1: Unit tests (no network).
 *   1.1  Request JSON serialisation — user/assistant/tool messages
 *   1.2  oaireqhdrs — X-Initiator, Authorization
 *   1.3  Delta parser driven from captured OAI text fixture
 *   1.4  Delta parser driven from captured OAI tool fixture
 *
 * Part 2: Live integration test (requires -s <sockpath> -t <tokenpath>).
 *   2.1  POST gpt-4o with user message "say hello world and nothing else"
 *        Verify: OAIDText deltas received, assembled text contains "hello"
 *        and "world".  OAIDStop with stop_reason "stop".
 *   2.2  POST gpt-4o with exec tool request "list files in /tmp"
 *        Verify: OAIDTool delta received with name "exec",
 *        OAIDToolArg chunks received, OAIDStop with "tool_calls".
 *
 * Fixtures captured from the Copilot API (same as ssetest.c).
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>

#include "9ai.h"
#include "http.h"
#include "json.h"
#include "oauth.h"
#include "sse.h"
#include "oai.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
	if(!(cond)) { \
		fprint(2, "FAIL: %s\n", msg); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

#define CHECKEQ(a, b, msg) do { \
	if((long)(a) != (long)(b)) { \
		fprint(2, "FAIL: %s: got %ld want %ld\n", \
		       msg, (long)(a), (long)(b)); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

#define CHECKSTR(a, b, msg) do { \
	const char *_a = (a), *_b = (b); \
	if(_a == nil || strcmp(_a, _b) != 0) { \
		fprint(2, "FAIL: %s: got \"%s\" want \"%s\"\n", \
		       msg, _a ? _a : "(nil)", _b); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

#define CHECKNIL(a, msg) do { \
	if((a) != nil) { \
		fprint(2, "FAIL: %s: got non-nil\n", msg); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

#define CHECKPREFIX(s, pfx, msg) do { \
	if((s) == nil || strncmp((s),(pfx),strlen(pfx)) != 0) { \
		fprint(2, "FAIL: %s: \"%s\" does not start with \"%s\"\n", \
		       msg, (s) ? (s) : "(nil)", (pfx)); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

#define CHECKCONTAINS(s, sub, msg) do { \
	if((s) == nil || strstr((s),(sub)) == nil) { \
		fprint(2, "FAIL: %s: \"%s\" does not contain \"%s\"\n", \
		       msg, (s) ? (s) : "(nil)", (sub)); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

/* ── Fixtures ────────────────────────────────────────────────────────── */
/* Captured OAI /chat/completions SSE streams (text and tool). */

static const char oai_text_sse[] =
    "data: {\"choices\":[],\"created\":0,\"id\":\"\",\"prompt_filter_results\":[{\"co"
    "ntent_filter_results\":{\"hate\":{\"filtered\":false,\"severity\":\"safe\"},\"se"
    "lf_harm\":{\"filtered\":false,\"severity\":\"safe\"},\"sexual\":{\"filtered\":fa"
    "lse,\"severity\":\"safe\"},\"violence\":{\"filtered\":false,\"severity\":\"safe\""
    "}},\"prompt_index\":0}]}\n\ndata: {\"choices\":[{\"index\":0,\"content_filter_re"
    "sults\":{},\"delta\":{\"content\":\"\",\"role\":\"assistant\"}}],\"created\":177"
    "2850501,\"id\":\"chatcmpl-DGbgrMVyCmGwllhL7hNsBsf5AoP4U\",\"model\":\"gpt-4o-202"
    "4-11-20\",\"system_fingerprint\":\"fp_569604b1f8\"}\n\ndata: {\"choices\":[{\"in"
    "dex\":0,\"content_filter_results\":{\"hate\":{\"filtered\":false,\"severity\":\"s"
    "afe\"},\"self_harm\":{\"filtered\":false,\"severity\":\"safe\"},\"sexual\":{\"fil"
    "tered\":false,\"severity\":\"safe\"},\"violence\":{\"filtered\":false,\"severity\""
    ":\"safe\"}},\"delta\":{\"content\":\"hello\"}}],\"created\":1772850501,\"id\":\"c"
    "hatcmpl-DGbgrMVyCmGwllhL7hNsBsf5AoP4U\",\"model\":\"gpt-4o-2024-11-20\",\"system"
    "_fingerprint\":\"fp_569604b1f8\"}\n\ndata: {\"choices\":[{\"index\":0,\"content_"
    "filter_results\":{\"hate\":{\"filtered\":false,\"severity\":\"safe\"},\"self_harm"
    "\":{\"filtered\":false,\"severity\":\"safe\"},\"sexual\":{\"filtered\":false,\"se"
    "verity\":\"safe\"},\"violence\":{\"filtered\":false,\"severity\":\"safe\"}},\"del"
    "ta\":{\"content\":\" world\"}}],\"created\":1772850501,\"id\":\"chatcmpl-DGbgrMVy"
    "CmGwllhL7hNsBsf5AoP4U\",\"model\":\"gpt-4o-2024-11-20\",\"system_fingerprint\":\""
    "fp_569604b1f8\"}\n\ndata: {\"choices\":[{\"finish_reason\":\"stop\",\"index\":0,"
    "\"content_filter_results\":{},\"delta\":{\"content\":null}}],\"created\":17728505"
    "01,\"id\":\"chatcmpl-DGbgrMVyCmGwllhL7hNsBsf5AoP4U\",\"usage\":{\"completion_tok"
    "ens\":3,\"completion_tokens_details\":{\"accepted_prediction_tokens\":0,\"rejecte"
    "d_prediction_tokens\":0},\"prompt_tokens\":13,\"prompt_tokens_details\":{\"cached"
    "_tokens\":0},\"total_tokens\":16,\"reasoning_tokens\":0},\"model\":\"gpt-4o-2024-"
    "11-20\",\"system_fingerprint\":\"fp_569604b1f8\"}\n\ndata: [DONE]\n\n"
    ;

static const char oai_tool_sse[] =
    "data: {\"choices\":[],\"created\":0,\"id\":\"\",\"prompt_filter_results\":[{\"co"
    "ntent_filter_results\":{\"hate\":{\"filtered\":false,\"severity\":\"safe\"},\"se"
    "lf_harm\":{\"filtered\":false,\"severity\":\"safe\"},\"sexual\":{\"filtered\":fa"
    "lse,\"severity\":\"safe\"},\"violence\":{\"filtered\":false,\"severity\":\"safe\""
    "}},\"prompt_index\":0}]}\n\ndata: {\"choices\":[{\"index\":0,\"content_filter_re"
    "sults\":{},\"delta\":{\"content\":null,\"role\":\"assistant\",\"tool_calls\":[{\""
    "function\":{\"arguments\":\"\",\"name\":\"exec\"},\"id\":\"call_krIGv9ti5vSLo5Qx"
    "84dxoeJ9\",\"index\":0,\"type\":\"function\"}]}}],\"created\":1772850512,\"id\":"
    "\"chatcmpl-DGbh2E2b2l7VDvDB72znvNL18CX0u\",\"model\":\"gpt-4o-2024-11-20\",\"sy"
    "stem_fingerprint\":\"fp_569604b1f8\"}\n\ndata: {\"choices\":[{\"index\":0,\"cont"
    "ent_filter_results\":{},\"delta\":{\"content\":null,\"tool_calls\":[{\"function\""
    ":{\"arguments\":\"{\\\"\"},\"index\":0}]}}],\"created\":1772850512,\"id\":\"chatc"
    "mpl-DGbh2E2b2l7VDvDB72znvNL18CX0u\",\"model\":\"gpt-4o-2024-11-20\",\"system_fi"
    "ngerprint\":\"fp_569604b1f8\"}\n\ndata: {\"choices\":[{\"index\":0,\"content_fil"
    "ter_results\":{},\"delta\":{\"content\":null,\"tool_calls\":[{\"function\":{\"ar"
    "guments\":\"argv\"},\"index\":0}]}}],\"created\":1772850512,\"id\":\"chatcmpl-DG"
    "bh2E2b2l7VDvDB72znvNL18CX0u\",\"model\":\"gpt-4o-2024-11-20\",\"system_fingerpri"
    "nt\":\"fp_569604b1f8\"}\n\ndata: {\"choices\":[{\"index\":0,\"content_filter_res"
    "ults\":{},\"delta\":{\"content\":null,\"tool_calls\":[{\"function\":{\"arguments"
    "\":\"\\\":[\\\"\"},\"index\":0}]}}],\"created\":1772850512,\"id\":\"chatcmpl-DGbh"
    "2E2b2l7VDvDB72znvNL18CX0u\",\"model\":\"gpt-4o-2024-11-20\",\"system_fingerprint"
    "\":\"fp_569604b1f8\"}\n\ndata: {\"choices\":[{\"index\":0,\"content_filter_resul"
    "ts\":{},\"delta\":{\"content\":null,\"tool_calls\":[{\"function\":{\"arguments\":"
    "\"echo\"},\"index\":0}]}}],\"created\":1772850512,\"id\":\"chatcmpl-DGbh2E2b2l7V"
    "DvDB72znvNL18CX0u\",\"model\":\"gpt-4o-2024-11-20\",\"system_fingerprint\":\"fp_"
    "569604b1f8\"}\n\ndata: {\"choices\":[{\"index\":0,\"content_filter_results\":{}"
    ",\"delta\":{\"content\":null,\"tool_calls\":[{\"function\":{\"arguments\":\"\\\","
    "\\\"\"},\"index\":0}]}}],\"created\":1772850512,\"id\":\"chatcmpl-DGbh2E2b2l7VDv"
    "DB72znvNL18CX0u\",\"model\":\"gpt-4o-2024-11-20\",\"system_fingerprint\":\"fp_56"
    "9604b1f8\"}\n\ndata: {\"choices\":[{\"index\":0,\"content_filter_results\":{},\""
    "delta\":{\"content\":null,\"tool_calls\":[{\"function\":{\"arguments\":\"hello\""
    "},\"index\":0}]}}],\"created\":1772850512,\"id\":\"chatcmpl-DGbh2E2b2l7VDvDB72zn"
    "vNL18CX0u\",\"model\":\"gpt-4o-2024-11-20\",\"system_fingerprint\":\"fp_569604b1"
    "f8\"}\n\ndata: {\"choices\":[{\"index\":0,\"content_filter_results\":{},\"delta\""
    ":{\"content\":null,\"tool_calls\":[{\"function\":{\"arguments\":\"\\\"]\"},\"inde"
    "x\":0}]}}],\"created\":1772850512,\"id\":\"chatcmpl-DGbh2E2b2l7VDvDB72znvNL18CX0"
    "u\",\"model\":\"gpt-4o-2024-11-20\",\"system_fingerprint\":\"fp_569604b1f8\"}\n\n"
    "data: {\"choices\":[{\"index\":0,\"content_filter_results\":{},\"delta\":{\"cont"
    "ent\":null,\"tool_calls\":[{\"function\":{\"arguments\":\"}\"},\"index\":0}]}}],"
    "\"created\":1772850512,\"id\":\"chatcmpl-DGbh2E2b2l7VDvDB72znvNL18CX0u\",\"mode"
    "l\":\"gpt-4o-2024-11-20\",\"system_fingerprint\":\"fp_569604b1f8\"}\n\ndata: {\"c"
    "hoices\":[{\"finish_reason\":\"tool_calls\",\"index\":0,\"content_filter_results\""
    ":{},\"delta\":{\"content\":null}}],\"created\":1772850512,\"id\":\"chatcmpl-DGbh2"
    "E2b2l7VDvDB72znvNL18CX0u\",\"usage\":{\"completion_tokens\":17,\"completion_token"
    "s_details\":{\"accepted_prediction_tokens\":0,\"rejected_prediction_tokens\":0},\""
    "prompt_tokens\":51,\"prompt_tokens_details\":{\"cached_tokens\":0},\"total_tokens"
    "\":68,\"reasoning_tokens\":0},\"model\":\"gpt-4o-2024-11-20\",\"system_fingerprin"
    "t\":\"fp_569604b1f8\"}\n\ndata: [DONE]\n\n"
    ;

/* ── Fixture driver ──────────────────────────────────────────────────── */
/*
 * Make an HTTPResp that reads from a static byte array via a pipe.
 * The caller must close the write end before reading; we close it here
 * after writing.
 */
static HTTPResp *
resp_from_fixture(const char *data, long len)
{
	int pfd[2];
	HTTPConn *c;
	HTTPResp *r;

	if(pipe(pfd) < 0)
		sysfatal("pipe: %r");

	/* Write fixture into the write end, then close it */
	if(write(pfd[1], data, len) != len)
		sysfatal("write fixture: %r");
	close(pfd[1]);

	c = mallocz(sizeof *c, 1);
	if(c == nil) sysfatal("mallocz: %r");
	c->fd   = pfd[0];
	c->bio  = mallocz(sizeof(Biobuf), 1);
	if(c->bio == nil) sysfatal("mallocz: %r");
	Binit(c->bio, pfd[0], OREAD);

	r = mallocz(sizeof *r, 1);
	if(r == nil) sysfatal("mallocz: %r");
	r->code = 200;
	r->conn = c;
	return r;
}

static void
resp_free(HTTPResp *r)
{
	if(r == nil) return;
	if(r->conn != nil) {
		if(r->conn->bio != nil) {
			Bterm(r->conn->bio);
			free(r->conn->bio);
		}
		close(r->conn->fd);
		free(r->conn->host);
		free(r->conn);
	}
	free(r->body);
	free(r);
}

/* ── Part 1.1: Request JSON serialisation ─────────────────────────────── */

static void
test_req_user_only(void)
{
	OAIReq *req;
	char   *body;
	long    len;

	print("\n-- 1.1 request JSON: user-only message\n");

	req = oaireqnew("gpt-4o");
	oaireqaddmsg(req, oaimsguser("hello world"));
	body = oaireqjson(req, nil, &len);

	CHECK(body != nil, "oaireqjson returns non-nil");
	CHECK(len > 0,     "body length > 0");
	CHECKCONTAINS(body, "\"model\":\"gpt-4o\"", "model field");
	CHECKCONTAINS(body, "\"stream\":true",       "stream field");
	CHECKCONTAINS(body, "max_completion_tokens", "max_completion_tokens field");
	CHECKCONTAINS(body, "\"role\":\"user\"",     "user role");
	CHECKCONTAINS(body, "hello world",           "user text content");
	CHECKCONTAINS(body, "\"name\":\"exec\"",     "exec tool present");

	free(body);
	oaireqfree(req);
}

static void
test_req_with_system(void)
{
	OAIReq *req;
	char   *body;
	long    len;

	print("\n-- 1.1 request JSON: system prompt\n");

	req = oaireqnew("gpt-4o");
	oaireqaddmsg(req, oaimsguser("ping"));
	body = oaireqjson(req, "you are a test assistant", &len);

	CHECK(body != nil, "oaireqjson with system returns non-nil");
	CHECKCONTAINS(body, "\"role\":\"system\"",           "system role present");
	CHECKCONTAINS(body, "you are a test assistant",       "system prompt text");

	free(body);
	oaireqfree(req);
}

static void
test_req_tool_round_trip(void)
{
	OAIReq *req;
	char   *body;
	long    len;

	print("\n-- 1.1 request JSON: tool call + result round-trip\n");

	req = oaireqnew("gpt-4o");
	oaireqaddmsg(req, oaimsguser("list files"));
	/* assistant with tool call */
	oaireqaddmsg(req, oaimsgtoolcall(
	    "",
	    "call_abc123",
	    "exec",
	    "{\"argv\":[\"ls\",\"-l\"]}"));
	/* tool result */
	oaireqaddmsg(req, oaimsgtoolresult(
	    "call_abc123",
	    "total 8\n-rw-r--r-- 1 user group 42 Jan 1 main.c\n",
	    0));

	body = oaireqjson(req, nil, &len);
	CHECK(body != nil, "oaireqjson tool round-trip returns non-nil");
	CHECKCONTAINS(body, "\"tool_calls\"",    "tool_calls array present");
	CHECKCONTAINS(body, "call_abc123",       "tool call id present");
	CHECKCONTAINS(body, "\"role\":\"tool\"", "tool result role present");
	CHECKCONTAINS(body, "total 8",           "tool result content present");

	free(body);
	oaireqfree(req);
}

static void
test_req_no_store(void)
{
	OAIReq *req;
	char   *body;
	long    len;

	print("\n-- 1.1 request JSON: no 'store' field (Copilot compat)\n");

	req = oaireqnew("gpt-4o");
	oaireqaddmsg(req, oaimsguser("test"));
	body = oaireqjson(req, nil, &len);

	/* "store" must not appear — Copilot compat */
	CHECK(body != nil && strstr(body, "\"store\"") == nil,
	      "no 'store' field in request");

	free(body);
	oaireqfree(req);
}

static void
test_req_assistant_content_string(void)
{
	OAIReq *req;
	char   *body;
	long    len;

	print("\n-- 1.1 request JSON: assistant content is string not array\n");

	req = oaireqnew("gpt-4o");
	oaireqaddmsg(req, oaimsguser("hello"));
	oaireqaddmsg(req, oaimsgassistant("I am here"));
	oaireqaddmsg(req, oaimsguser("ok"));
	body = oaireqjson(req, nil, &len);

	CHECK(body != nil, "body non-nil");
	CHECKCONTAINS(body, "I am here", "assistant text present");
	/* Copilot compat: content is a plain string, not [{type:text,...}] */
	CHECK(strstr(body, "\"type\":\"text\"") == nil,
	      "assistant content not an array of typed blocks");

	free(body);
	oaireqfree(req);
}

/* ── Part 1.2: oaireqhdrs ─────────────────────────────────────────────── */

static void
test_hdrs_user_initiator(void)
{
	OAIReq   *req;
	HTTPHdr   hdrs[16];
	int       n, i;
	int found_auth = 0, found_initiator_user = 0;

	print("\n-- 1.2 headers: X-Initiator=user when last msg is user\n");

	req = oaireqnew("gpt-4o");
	oaireqaddmsg(req, oaimsguser("hello"));
	n = oaireqhdrs(req, "tok_abc", hdrs, 16);

	CHECK(n > 0, "at least one header returned");
	for(i = 0; i < n; i++) {
		if(strcmp(hdrs[i].name, "Authorization") == 0) {
			found_auth = 1;
			CHECKPREFIX(hdrs[i].value, "Bearer ", "Authorization starts with Bearer");
			CHECKCONTAINS(hdrs[i].value, "tok_abc", "Authorization contains session token");
		}
		if(strcmp(hdrs[i].name, "X-Initiator") == 0 &&
		   strcmp(hdrs[i].value, "user") == 0)
			found_initiator_user = 1;
	}
	CHECK(found_auth, "Authorization header present");
	CHECK(found_initiator_user, "X-Initiator: user when last msg is user");

	oaireqfree(req);
	/* free smprint'd Authorization values */
	for(i = 0; i < n; i++)
		if(strcmp(hdrs[i].name, "Authorization") == 0)
			free(hdrs[i].value);
}

static void
test_hdrs_agent_initiator(void)
{
	OAIReq   *req;
	HTTPHdr   hdrs[16];
	int       n, i;
	int found_initiator_agent = 0;

	print("\n-- 1.2 headers: X-Initiator=agent when last msg is tool result\n");

	req = oaireqnew("gpt-4o");
	oaireqaddmsg(req, oaimsguser("run ls"));
	oaireqaddmsg(req, oaimsgtoolcall("", "call_x", "exec", "{\"argv\":[\"ls\"]}"));
	oaireqaddmsg(req, oaimsgtoolresult("call_x", "file1\nfile2\n", 0));
	n = oaireqhdrs(req, "tok_xyz", hdrs, 16);

	for(i = 0; i < n; i++)
		if(strcmp(hdrs[i].name, "X-Initiator") == 0 &&
		   strcmp(hdrs[i].value, "agent") == 0)
			found_initiator_agent = 1;
	CHECK(found_initiator_agent, "X-Initiator: agent after tool result");

	oaireqfree(req);
	for(i = 0; i < n; i++)
		if(strcmp(hdrs[i].name, "Authorization") == 0)
			free(hdrs[i].value);
}

/* ── Part 1.3: Delta parser — text fixture ────────────────────────────── */

static void
test_delta_text_fixture(void)
{
	HTTPResp *resp;
	OAIParser p;
	OAIDelta  d;
	int       rc;
	char      assembled[1024];
	int       ntext = 0, nstop = 0;

	print("\n-- 1.3 delta parser: OAI text fixture\n");

	resp = resp_from_fixture(oai_text_sse, strlen(oai_text_sse));
	oaiinit(&p, resp);

	assembled[0] = '\0';

	while((rc = oaidelta(&p, &d)) == OAI_OK) {
		switch(d.type) {
		case OAIDText:
			ntext++;
			strncat(assembled, d.text, sizeof assembled - strlen(assembled) - 1);
			break;
		case OAIDStop:
			nstop++;
			CHECKSTR(d.stop_reason, "stop", "text fixture stop_reason is 'stop'");
			break;
		case OAIDTool:
		case OAIDToolArg:
			CHECK(0, "no tool deltas expected in text fixture");
			break;
		}
	}

	CHECKEQ(rc, OAI_DONE, "text fixture ends with OAI_DONE");
	CHECK(ntext >= 2,         "at least 2 text deltas (hello + world)");
	CHECKEQ(nstop, 1,         "exactly 1 stop delta");
	CHECKCONTAINS(assembled, "hello", "assembled text contains 'hello'");
	CHECKCONTAINS(assembled, "world", "assembled text contains 'world'");

	oaiterm(&p);
	resp_free(resp);
}

/* ── Part 1.4: Delta parser — tool fixture ────────────────────────────── */

static void
test_delta_tool_fixture(void)
{
	HTTPResp *resp;
	OAIParser p;
	OAIDelta  d;
	int       rc;
	char      args[1024];
	int       ntool = 0, ntoolarg = 0, nstop = 0;
	char      saved_tool_id[128];
	char      saved_tool_name[128];

	print("\n-- 1.4 delta parser: OAI tool fixture\n");

	resp = resp_from_fixture(oai_tool_sse, strlen(oai_tool_sse));
	oaiinit(&p, resp);

	args[0]           = '\0';
	saved_tool_id[0]  = '\0';
	saved_tool_name[0]= '\0';

	while((rc = oaidelta(&p, &d)) == OAI_OK) {
		switch(d.type) {
		case OAIDTool:
			ntool++;
			if(d.tool_id != nil)
				snprint(saved_tool_id, sizeof saved_tool_id, "%s", d.tool_id);
			if(d.tool_name != nil)
				snprint(saved_tool_name, sizeof saved_tool_name, "%s", d.tool_name);
			break;
		case OAIDToolArg:
			ntoolarg++;
			strncat(args, d.text, sizeof args - strlen(args) - 1);
			break;
		case OAIDStop:
			nstop++;
			CHECKSTR(d.stop_reason, "tool_calls",
			         "tool fixture stop_reason is 'tool_calls'");
			break;
		case OAIDText:
			/* some chunks have null content — skip; should not appear here */
			break;
		}
	}

	CHECKEQ(rc, OAI_DONE,     "tool fixture ends with OAI_DONE");
	CHECKEQ(ntool, 1,          "exactly 1 OAIDTool delta");
	CHECK(ntoolarg >= 1,        "at least 1 OAIDToolArg delta");
	CHECKEQ(nstop, 1,           "exactly 1 stop delta");
	CHECKCONTAINS(saved_tool_id,  "call_",  "tool id starts with call_");
	CHECKSTR(saved_tool_name, "exec",       "tool name is 'exec'");
	CHECKCONTAINS(args, "argv",             "accumulated args contain 'argv'");
	CHECKCONTAINS(args, "echo",             "accumulated args contain 'echo'");

	oaiterm(&p);
	resp_free(resp);
}

/* ── Part 1.5: oaireqctxtokens and oaireqtrim ────────────────────────── */

static void
test_ctxtokens_empty(void)
{
	OAIReq *req;

	print("\n-- 1.5 oaireqctxtokens: empty req → 0\n");

	req = oaireqnew("gpt-4o");
	CHECKEQ(oaireqctxtokens(req), 0, "empty req has 0 tokens");
	oaireqfree(req);
}

static void
test_ctxtokens_counts_text(void)
{
	OAIReq *req;
	long    est;

	print("\n-- 1.5 oaireqctxtokens: 8-char user message → 2 tokens\n");

	req = oaireqnew("gpt-4o");
	oaireqaddmsg(req, oaimsguser("12345678"));  /* exactly 8 chars */
	est = oaireqctxtokens(req);
	CHECKEQ(est, 2, "8 chars / 4 = 2 tokens");
	oaireqfree(req);
}

static void
test_ctxtokens_counts_tool_args(void)
{
	OAIReq *req;
	long    est;

	print("\n-- 1.5 oaireqctxtokens: tool args counted\n");

	req = oaireqnew("gpt-4o");
	oaireqaddmsg(req, oaimsguser("go"));
	/* tool call with 8-char args */
	oaireqaddmsg(req, oaimsgtoolcall("", "call_x", "exec", "12345678"));
	est = oaireqctxtokens(req);
	/* "go" = 2 chars + "12345678" = 8 chars → 10 chars → ceil(10/4) = 3 */
	CHECKEQ(est, 3, "user text + tool_args counted: ceil(10/4)=3");
	oaireqfree(req);
}

static void
test_trim_zero_noop(void)
{
	OAIReq *req;
	int     removed;

	print("\n-- 1.5 oaireqtrim: trim 0 is no-op\n");

	req = oaireqnew("gpt-4o");
	oaireqaddmsg(req, oaimsguser("hello"));
	oaireqaddmsg(req, oaimsgassistant("world"));
	removed = oaireqtrim(req, 0);
	CHECKEQ(removed, 0,          "trim 0 removed 0 messages");
	CHECK(req->msgs != nil,      "list non-empty after trim 0");
	oaireqfree(req);
}

static void
test_trim_one_turn(void)
{
	OAIReq *req;
	int     removed;

	print("\n-- 1.5 oaireqtrim: trim 1 removes first user+assistant pair\n");

	req = oaireqnew("gpt-4o");
	/* turn 1 */
	oaireqaddmsg(req, oaimsguser("first question"));
	oaireqaddmsg(req, oaimsgassistant("first answer"));
	/* turn 2 */
	oaireqaddmsg(req, oaimsguser("second question"));
	oaireqaddmsg(req, oaimsgassistant("second answer"));

	removed = oaireqtrim(req, 1);

	CHECK(removed > 0, "trim 1 removed messages");
	/* first user message of turn 2 must now be the head */
	CHECK(req->msgs != nil, "list non-empty after trim");
	CHECKEQ(req->msgs->role, OAIRoleUser, "head is user message");
	{
		/* verify the content is the second turn's user message */
		char *body = oaireqjson(req, nil, nil);
		CHECK(body != nil, "serialised after trim");
		CHECKCONTAINS(body, "second question", "second turn still present");
		CHECK(strstr(body, "first question") == nil,
		      "first turn no longer in JSON");
		free(body);
	}
	oaireqfree(req);
}

static void
test_trim_with_tool_turn(void)
{
	OAIReq *req;
	int     removed;

	print("\n-- 1.5 oaireqtrim: trim 1 removes turn that contains a tool call\n");

	req = oaireqnew("gpt-4o");
	/* turn 1: user → assistant tool call → tool result */
	oaireqaddmsg(req, oaimsguser("list files"));
	oaireqaddmsg(req, oaimsgtoolcall("", "call_1", "exec", "{\"argv\":[\"ls\"]}"));
	oaireqaddmsg(req, oaimsgtoolresult("call_1", "file1\nfile2\n", 0));
	/* turn 2: plain exchange */
	oaireqaddmsg(req, oaimsguser("what did you find?"));
	oaireqaddmsg(req, oaimsgassistant("two files"));

	removed = oaireqtrim(req, 1);

	CHECK(removed >= 3, "at least 3 messages removed (user+toolcall+toolresult)");
	{
		char *body = oaireqjson(req, nil, nil);
		CHECK(body != nil, "serialised after trim");
		CHECKCONTAINS(body, "what did you find?", "turn 2 still present");
		CHECK(strstr(body, "list files") == nil, "turn 1 user gone");
		CHECK(strstr(body, "file1") == nil,      "turn 1 tool result gone");
		free(body);
	}
	oaireqfree(req);
}

static void
test_trim_all(void)
{
	OAIReq *req;
	int     removed;

	print("\n-- 1.5 oaireqtrim: trim N > actual turns removes everything\n");

	req = oaireqnew("gpt-4o");
	oaireqaddmsg(req, oaimsguser("only question"));
	oaireqaddmsg(req, oaimsgassistant("only answer"));

	removed = oaireqtrim(req, 99);

	CHECK(removed > 0,           "messages were removed");
	CHECKNIL(req->msgs,          "msgs is nil after trimming all");
	CHECKNIL(req->msgtail,       "msgtail is nil after trimming all");
	oaireqfree(req);
}

static void
test_trim_msgtail_updated(void)
{
	OAIReq *req;
	char   *body;

	print("\n-- 1.5 oaireqtrim: msgtail updated; append after trim works\n");

	req = oaireqnew("gpt-4o");
	/* turn 1 */
	oaireqaddmsg(req, oaimsguser("alpha"));
	oaireqaddmsg(req, oaimsgassistant("beta"));
	/* turn 2 */
	oaireqaddmsg(req, oaimsguser("gamma"));
	oaireqaddmsg(req, oaimsgassistant("delta"));

	oaireqtrim(req, 1);

	/* append a new message — this crashes if msgtail is stale */
	oaireqaddmsg(req, oaimsguser("epsilon"));

	body = oaireqjson(req, nil, nil);
	CHECK(body != nil, "serialised after trim + append");
	CHECKCONTAINS(body, "gamma",   "turn 2 present");
	CHECKCONTAINS(body, "epsilon", "appended message present");
	CHECK(strstr(body, "alpha") == nil, "turn 1 absent");
	free(body);
	oaireqfree(req);
}

static void
test_trim_empty_noop(void)
{
	OAIReq *req;
	int     removed;

	print("\n-- 1.5 oaireqtrim: trim on empty req → 0\n");

	req = oaireqnew("gpt-4o");
	removed = oaireqtrim(req, 1);
	CHECKEQ(removed, 0, "trim on empty req removes 0");
	oaireqfree(req);
}

/* ── Part 2: Live integration tests ───────────────────────────────────── */

static void
test_live_text(char *sockpath, char *tokpath)
{
	char       refresh[256], *p;
	OAuthToken *tok;
	OAIReq     *req;
	HTTPHdr     hdrs[16];
	int         nhdrs;
	char       *body;
	long        bodylen;
	HTTPConn   *c;
	HTTPResp   *r;
	OAIParser   parser;
	OAIDelta    d;
	int         rc;
	char        assembled[8192];
	int         ntext = 0, nstop = 0;
	int         fd;

	print("\n-- 2.1 live: gpt-4o text response\n");

	/* load refresh token */
	fd = open(tokpath, OREAD);
	if(fd < 0) { fprint(2, "SKIP: cannot open token %s: %r\n", tokpath); return; }
	p = refresh;
	{
		int n = read(fd, refresh, sizeof refresh - 1);
		if(n <= 0) { close(fd); fprint(2, "SKIP: empty token file\n"); return; }
		refresh[n] = '\0';
		/* strip trailing newline */
		while(n > 0 && (refresh[n-1] == '\n' || refresh[n-1] == '\r'))
			refresh[--n] = '\0';
	}
	close(fd);
	USED(p);

	tok = oauthsession(refresh, sockpath);
	if(tok == nil) { fprint(2, "SKIP: oauthsession failed: %r\n"); return; }

	req = oaireqnew("gpt-4o");
	oaireqaddmsg(req, oaimsguser("say hello world and nothing else"));
	nhdrs = oaireqhdrs(req, tok->token, hdrs, 16);

	body = oaireqjson(req, nil, &bodylen);
	CHECK(body != nil, "live: request JSON serialised");

	c = portdial("api.individual.githubcopilot.com", "443", sockpath);
	if(c == nil) {
		fprint(2, "SKIP: httpdial failed: %r\n");
		free(body);
		oaireqfree(req);
		oauthtokenfree(tok);
		return;
	}
	r = httppost(c, "/chat/completions",
	             "api.individual.githubcopilot.com",
	             hdrs, nhdrs, body, bodylen);
	if(r == nil || r->code != 200) {
		fprint(2, "SKIP: POST failed code=%d: %r\n", r ? r->code : -1);
		if(r) httprespfree(r);
		httpclose(c);
		free(body);
		oaireqfree(req);
		oauthtokenfree(tok);
		return;
	}

	oaiinit(&parser, r);
	assembled[0] = '\0';
	while((rc = oaidelta(&parser, &d)) == OAI_OK) {
		if(d.type == OAIDText) {
			ntext++;
			strncat(assembled, d.text, sizeof assembled - strlen(assembled) - 1);
		} else if(d.type == OAIDStop) {
			nstop++;
		}
	}

	{
		/* case-fold for comparison — model may capitalise "Hello" / "World" */
		char lc[sizeof assembled];
		int i;
		for(i = 0; assembled[i]; i++)
			lc[i] = (assembled[i] >= 'A' && assembled[i] <= 'Z')
			         ? assembled[i] + 32 : assembled[i];
		lc[i] = '\0';
		CHECKEQ(rc, OAI_DONE, "live text: ends OAI_DONE");
		CHECK(ntext > 0,       "live text: at least one text delta");
		CHECKEQ(nstop, 1,      "live text: exactly one stop delta");
		CHECKCONTAINS(lc, "hello", "live text: assembled contains 'hello' (case-insensitive)");
		CHECKCONTAINS(lc, "world", "live text: assembled contains 'world' (case-insensitive)");
	}
	print("  assembled: %s\n", assembled);

	oaiterm(&parser);
	httprespfree(r);
	httpclose(c);
	free(body);
	oaireqfree(req);
	oauthtokenfree(tok);
	/* free smprint'd Authorization values */
	{
		int i;
		for(i = 0; i < nhdrs; i++)
			if(strcmp(hdrs[i].name, "Authorization") == 0)
				free(hdrs[i].value);
	}
}

static void
test_live_tool(char *sockpath, char *tokpath)
{
	char       refresh[256];
	OAuthToken *tok;
	OAIReq     *req;
	HTTPHdr     hdrs[16];
	int         nhdrs;
	char       *body;
	long        bodylen;
	HTTPConn   *c;
	HTTPResp   *r;
	OAIParser   parser;
	OAIDelta    d;
	int         rc;
	char        args[4096];
	int         ntool = 0, ntoolarg = 0, nstop = 0;
	char        tool_name[128];
	int         fd;

	print("\n-- 2.2 live: gpt-4o tool call\n");

	fd = open(tokpath, OREAD);
	if(fd < 0) { fprint(2, "SKIP: cannot open token %s: %r\n", tokpath); return; }
	{
		int n = read(fd, refresh, sizeof refresh - 1);
		if(n <= 0) { close(fd); fprint(2, "SKIP: empty token file\n"); return; }
		refresh[n] = '\0';
		while(n > 0 && (refresh[n-1] == '\n' || refresh[n-1] == '\r'))
			refresh[--n] = '\0';
	}
	close(fd);

	tok = oauthsession(refresh, sockpath);
	if(tok == nil) { fprint(2, "SKIP: oauthsession failed: %r\n"); return; }

	req = oaireqnew("gpt-4o");
	oaireqaddmsg(req, oaimsguser(
	    "Use the exec tool to run: echo hello. "
	    "Do not explain, just call the tool immediately."));
	nhdrs = oaireqhdrs(req, tok->token, hdrs, 16);
	body = oaireqjson(req, nil, &bodylen);
	CHECK(body != nil, "live tool: request JSON serialised");

	c = portdial("api.individual.githubcopilot.com", "443", sockpath);
	if(c == nil) {
		fprint(2, "SKIP: httpdial failed: %r\n");
		free(body);
		oaireqfree(req);
		oauthtokenfree(tok);
		return;
	}
	r = httppost(c, "/chat/completions",
	             "api.individual.githubcopilot.com",
	             hdrs, nhdrs, body, bodylen);
	if(r == nil || r->code != 200) {
		fprint(2, "SKIP: POST failed code=%d: %r\n", r ? r->code : -1);
		if(r) httprespfree(r);
		httpclose(c);
		free(body);
		oaireqfree(req);
		oauthtokenfree(tok);
		return;
	}

	oaiinit(&parser, r);
	args[0]     = '\0';
	tool_name[0]= '\0';
	while((rc = oaidelta(&parser, &d)) == OAI_OK) {
		switch(d.type) {
		case OAIDTool:
			ntool++;
			if(d.tool_name != nil)
				snprint(tool_name, sizeof tool_name, "%s", d.tool_name);
			break;
		case OAIDToolArg:
			ntoolarg++;
			strncat(args, d.text, sizeof args - strlen(args) - 1);
			break;
		case OAIDStop:
			nstop++;
			break;
		case OAIDText:
			break;
		}
	}

	CHECKEQ(rc, OAI_DONE,  "live tool: ends OAI_DONE");
	CHECK(ntool >= 1,       "live tool: at least one OAIDTool delta");
	CHECKSTR(tool_name, "exec", "live tool: tool name is 'exec'");
	CHECK(ntoolarg >= 1,    "live tool: at least one OAIDToolArg delta");
	CHECKEQ(nstop, 1,       "live tool: exactly one stop delta");
	CHECKCONTAINS(args, "argv", "live tool: args contain 'argv'");
	print("  accumulated args: %s\n", args);

	oaiterm(&parser);
	httprespfree(r);
	httpclose(c);
	free(body);
	oaireqfree(req);
	oauthtokenfree(tok);
	{
		int i;
		for(i = 0; i < nhdrs; i++)
			if(strcmp(hdrs[i].name, "Authorization") == 0)
				free(hdrs[i].value);
	}
}

/* ── Main ─────────────────────────────────────────────────────────────── */

void
threadmain(int argc, char *argv[])
{
	char *sockpath = nil;
	char *tokpath  = nil;

	ARGBEGIN{
	case 's': sockpath = ARGF(); break;
	case 't': tokpath  = ARGF(); break;
	default:
		fprint(2, "usage: %s [-s sockpath] [-t tokenpath]\n", argv0);
		threadexitsall("usage");
	}ARGEND

	USED(argc);

	print("=== Part 1: unit tests ===\n");
	test_req_user_only();
	test_req_with_system();
	test_req_tool_round_trip();
	test_req_no_store();
	test_req_assistant_content_string();
	test_hdrs_user_initiator();
	test_hdrs_agent_initiator();
	test_delta_text_fixture();
	test_delta_tool_fixture();

	print("\n=== Part 1.5: oaireqctxtokens and oaireqtrim ===\n");
	test_ctxtokens_empty();
	test_ctxtokens_counts_text();
	test_ctxtokens_counts_tool_args();
	test_trim_zero_noop();
	test_trim_one_turn();
	test_trim_with_tool_turn();
	test_trim_all();
	test_trim_msgtail_updated();
	test_trim_empty_noop();

	if(sockpath != nil && tokpath != nil) {
		print("\n=== Part 2: live integration tests ===\n");
		test_live_text(sockpath, tokpath);
		test_live_tool(sockpath, tokpath);
	} else {
		print("\n(skip live tests: pass -s sockpath -t tokenpath to enable)\n");
	}

	if(failures > 0) {
		fprint(2, "\n%d test(s) FAILED\n", failures);
		threadexitsall("FAIL");
	}
	print("\nall tests passed\n");
	threadexitsall(nil);
}
