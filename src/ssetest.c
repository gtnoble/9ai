/*
 * ssetest.c — unit tests for the SSE line parser (sse.c)
 *
 * Tests drive the parser from real captured fixture bytes fed through a
 * pipe, without any network access.  The pipe tricks httpreadline() into
 * reading from our fixture data exactly as it would from a live connection.
 *
 * Fixtures captured from the Copilot API:
 *   oai_text_sse  — OpenAI /chat/completions, text response
 *   ant_text_sse  — Anthropic /v1/messages, text response
 *   oai_tool_sse  — OpenAI /chat/completions, tool_calls response
 *   ant_tool_sse  — Anthropic /v1/messages, tool_use response
 *
 * For each fixture we verify:
 *   - correct number of SSE_OK events before SSE_DONE
 *   - event name nil (OAI) or correct string (ANT) on each step
 *   - data payload starts with expected prefix
 *   - ssestep returns SSE_DONE as the last result
 *   - SSEEvent fields are valid
 *
 * Part 2: live integration test (optional; -s/-t flags).
 *   POSTs to /chat/completions (gpt-4o) and /v1/messages (claude-sonnet-4.5),
 *   drives the SSE parser, and verifies text content is received.
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

static int failures = 0;

#define CHECK(cond, msg) do { \
	if(!(cond)) { \
		fprint(2, "FAIL: %s (line %d)\n", msg, __LINE__); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

#define CHECKEQ(a, b, msg) do { \
	if((long)(a) != (long)(b)) { \
		fprint(2, "FAIL: %s: got %ld want %ld (line %d)\n", \
		       msg, (long)(a), (long)(b), __LINE__); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

#define CHECKSTR(a, b, msg) do { \
	const char *_a = (a), *_b = (b); \
	if(_a == nil || strcmp(_a, _b) != 0) { \
		fprint(2, "FAIL: %s: got %s want %s (line %d)\n", \
		       msg, _a ? _a : "(nil)", _b, __LINE__); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

#define CHECKNIL(a, msg) do { \
	if((a) != nil) { \
		fprint(2, "FAIL: %s: got non-nil (line %d)\n", msg, __LINE__); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

#define CHECKPREFIX(s, pfx, msg) do { \
	if((s) == nil || strncmp((s),(pfx),strlen(pfx)) != 0) { \
		fprint(2, "FAIL: %s: \"%s\" does not start with \"%s\" (line %d)\n", \
		       msg, (s) ? (s) : "(nil)", (pfx), __LINE__); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

/* ── Fixtures ────────────────────────────────────────────────────────── */
/* Real SSE streams captured from the Copilot API. */

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

static const char ant_text_sse[] =
    "event: message_start\ndata: {\"message\":{\"content\":[],\"id\":\"msg_bdrk_01UUX"
    "fm5MgQftiAPL8nLsGkk\",\"model\":\"claude-sonnet-4-5-20250929\",\"role\":\"assist"
    "ant\",\"stop_reason\":null,\"stop_sequence\":null,\"type\":\"message\",\"usage\":"
    "{\"cache_creation\":{\"ephemeral_1h_input_tokens\":0,\"ephemeral_5m_input_tokens\""
    ":0},\"cache_creation_input_tokens\":0,\"cache_read_input_tokens\":0,\"input_token"
    "s\":13,\"output_tokens\":1}},\"type\":\"message_start\"}\n\nevent: content_block_"
    "start\ndata: {\"content_block\":{\"text\":\"\",\"type\":\"text\"},\"index\":0,\""
    "type\":\"content_block_start\"}\n\nevent: content_block_delta\ndata: {\"delta\":{"
    "\"text\":\"hello\",\"type\":\"text_delta\"},\"index\":0,\"type\":\"content_block_"
    "delta\"}\n\nevent: content_block_delta\ndata: {\"delta\":{\"text\":\" world\",\"t"
    "ype\":\"text_delta\"},\"index\":0,\"type\":\"content_block_delta\"}\n\nevent: con"
    "tent_block_stop\ndata: {\"index\":0,\"type\":\"content_block_stop\"}\n\nevent: me"
    "ssage_delta\ndata: {\"copilot_usage\":{\"token_details\":null,\"total_nano_aiu\":0"
    "},\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"type\":\"mess"
    "age_delta\",\"usage\":{\"output_tokens\":5}}\n\nevent: message_stop\ndata: {\"ama"
    "zon-bedrock-invocationMetrics\":{\"firstByteLatency\":1231,\"inputTokenCount\":13"
    ",\"invocationLatency\":1315,\"outputTokenCount\":5},\"type\":\"message_stop\"}\n\n"
    "data: [DONE]\n\n"
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

static const char ant_tool_sse[] =
    "event: message_start\ndata: {\"message\":{\"content\":[],\"id\":\"msg_bdrk_013FW"
    "HkWD3yMBzUMhECeLZb4\",\"model\":\"claude-sonnet-4-5-20250929\",\"role\":\"assist"
    "ant\",\"stop_reason\":null,\"stop_sequence\":null,\"type\":\"message\",\"usage\":"
    "{\"cache_creation\":{\"ephemeral_1h_input_tokens\":0,\"ephemeral_5m_input_tokens\""
    ":0},\"cache_creation_input_tokens\":0,\"cache_read_input_tokens\":0,\"input_token"
    "s\":579,\"output_tokens\":1}},\"type\":\"message_start\"}\n\nevent: content_block"
    "_start\ndata: {\"content_block\":{\"id\":\"toolu_bdrk_011dsXUGnEnR84WXf49XdJ1p\""
    ",\"input\":{},\"name\":\"exec\",\"type\":\"tool_use\"},\"index\":0,\"type\":\"con"
    "tent_block_start\"}\n\nevent: content_block_delta\ndata: {\"delta\":{\"partial_js"
    "on\":\"\",\"type\":\"input_json_delta\"},\"index\":0,\"type\":\"content_block_del"
    "ta\"}\n\nevent: content_block_delta\ndata: {\"delta\":{\"partial_json\":\"{\\\"\"}"
    ",\"type\":\"input_json_delta\"},\"index\":0,\"type\":\"content_block_delta\"}\n\n"
    "event: content_block_delta\ndata: {\"delta\":{\"partial_json\":\"ar\",\"type\":\"i"
    "nput_json_delta\"},\"index\":0,\"type\":\"content_block_delta\"}\n\nevent: content"
    "_block_delta\ndata: {\"delta\":{\"partial_json\":\"gv\\\": [\\\"\",\"type\":\"inpu"
    "t_json_delta\"},\"index\":0,\"type\":\"content_block_delta\"}\n\nevent: content_b"
    "lock_delta\ndata: {\"delta\":{\"partial_json\":\"echo\\\",\\\"hello\",\"type\":\""
    "input_json_delta\"},\"index\":0,\"type\":\"content_block_delta\"}\n\nevent: conte"
    "nt_block_delta\ndata: {\"delta\":{\"partial_json\":\"\\\"]}\"  ,\"type\":\"input_"
    "json_delta\"},\"index\":0,\"type\":\"content_block_delta\"}\n\nevent: content_blo"
    "ck_stop\ndata: {\"index\":0,\"type\":\"content_block_stop\"}\n\nevent: message_de"
    "lta\ndata: {\"copilot_usage\":{\"token_details\":null,\"total_nano_aiu\":0},\"delt"
    "a\":{\"stop_reason\":\"tool_use\",\"stop_sequence\":null},\"type\":\"message_delt"
    "a\",\"usage\":{\"output_tokens\":56}}\n\nevent: message_stop\ndata: {\"amazon-be"
    "drock-invocationMetrics\":{\"firstByteLatency\":1155,\"inputTokenCount\":579,\"in"
    "vocationLatency\":1541,\"outputTokenCount\":56},\"type\":\"message_stop\"}\n\ndat"
    "a: [DONE]\n\n"
    ;

/* ── Pipe-based HTTPResp factory ─────────────────────────────────────── */

/*
 * make_resp — create an HTTPResp whose Biobuf reads from a pipe
 * pre-loaded with the given fixture bytes.
 *
 * Writes fixture into write end of pipe, closes write end, returns
 * the HTTPResp reading from the read end.
 * Caller must free resp and close resp->conn->fd.
 */
static HTTPResp *
make_resp(const char *data, long len)
{
	int p[2];
	HTTPConn *c;
	HTTPResp *r;

	if(pipe(p) < 0)
		sysfatal("pipe: %r");

	/* write all fixture data then close write end */
	if(write(p[1], (char*)data, len) != len)
		sysfatal("write to pipe: %r");
	close(p[1]);

	c = mallocz(sizeof *c, 1);
	c->fd   = p[0];
	c->host = strdup("(fixture)");
	c->bio  = mallocz(sizeof(Biobuf), 1);
	Binit(c->bio, p[0], OREAD);

	r = mallocz(sizeof *r, 1);
	r->code = 200;
	r->conn = c;
	return r;
}

static void
free_resp(HTTPResp *r)
{
	if(r == nil) return;
	Bterm(r->conn->bio);
	free(r->conn->bio);
	close(r->conn->fd);
	free(r->conn->host);
	free(r->conn);
	free(r);
}

/* ── Unit test helpers ───────────────────────────────────────────────── */

typedef struct ExpectedEvent ExpectedEvent;
struct ExpectedEvent {
	char *event;  /* nil means "must be nil" */
	char *prefix; /* first chars of data */
};

/*
 * run_fixture — drive the SSE parser through a fixture and check each event.
 *
 * expect[0..n-1] describes the SSE_OK events before SSE_DONE.
 * Returns number of failures introduced.
 */
static int
run_fixture(const char *name,
            const char *data, long len,
            ExpectedEvent *expect, int n)
{
	HTTPResp *r;
	SSEParser p;
	SSEEvent  ev;
	int       rc, i, prev;
	char      label[128];

	prev = failures;
	r = make_resp(data, len);
	sseinit(&p, r);

	for(i = 0; i < n; i++) {
		rc = ssestep(&p, &ev);
		snprint(label, sizeof label, "%s[%d]: rc==SSE_OK", name, i);
		CHECKEQ(rc, SSE_OK, label);
		if(rc != SSE_OK) break;

		snprint(label, sizeof label, "%s[%d]: data!=nil", name, i);
		CHECK(ev.data != nil, label);

		if(expect[i].event == nil) {
			snprint(label, sizeof label, "%s[%d]: event==nil", name, i);
			CHECKNIL(ev.event, label);
		} else {
			snprint(label, sizeof label, "%s[%d]: event==%s", name, i, expect[i].event);
			CHECKSTR(ev.event, expect[i].event, label);
		}

		if(expect[i].prefix != nil) {
			snprint(label, sizeof label, "%s[%d]: data prefix", name, i);
			CHECKPREFIX(ev.data, expect[i].prefix, label);
		}
	}

	/* next call must return SSE_DONE */
	rc = ssestep(&p, &ev);
	snprint(label, sizeof label, "%s: final rc==SSE_DONE", name);
	CHECKEQ(rc, SSE_DONE, label);

	free_resp(r);
	return failures - prev;
}

/* ── Unit tests ──────────────────────────────────────────────────────── */

static void
test_oai_text(void)
{
	/* 5 data events then [DONE] */
	ExpectedEvent expect[] = {
		{ nil, "{\"choices\":[]," },
		{ nil, "{\"choices\":[{\"index\":0," },
		{ nil, "{\"choices\":[{\"index\":0," },  /* "hello" delta */
		{ nil, "{\"choices\":[{\"index\":0," },  /* " world" delta */
		{ nil, "{\"choices\":[{\"finish_reason\":\"stop\"" },
	};
	run_fixture("oai_text", oai_text_sse, sizeof oai_text_sse - 1,
	            expect, nelem(expect));
}

static void
test_ant_text(void)
{
	/* 7 named events then [DONE] */
	ExpectedEvent expect[] = {
		{ "message_start",       "{\"message\":" },
		{ "content_block_start", "{\"content_block\":" },
		{ "content_block_delta", "{\"delta\":{\"text\":\"hello\"" },
		{ "content_block_delta", "{\"delta\":{\"text\":\" world\"" },
		{ "content_block_stop",  "{\"index\":0" },
		{ "message_delta",       "{\"copilot_usage\":" },
		{ "message_stop",        "{\"amazon-bedrock" },
	};
	run_fixture("ant_text", ant_text_sse, sizeof ant_text_sse - 1,
	            expect, nelem(expect));
}

static void
test_oai_tool(void)
{
	/* 11 data events then [DONE] */
	ExpectedEvent expect[] = {
		{ nil, "{\"choices\":[]," },               /* prompt filter */
		{ nil, "{\"choices\":[{\"index\":0," },     /* tool_calls init */
		{ nil, "{\"choices\":[{\"index\":0," },     /* arg chunk: {" */
		{ nil, "{\"choices\":[{\"index\":0," },     /* arg chunk: argv */
		{ nil, "{\"choices\":[{\"index\":0," },     /* arg chunk: ":[ */
		{ nil, "{\"choices\":[{\"index\":0," },     /* arg chunk: echo */
		{ nil, "{\"choices\":[{\"index\":0," },     /* arg chunk: ," */
		{ nil, "{\"choices\":[{\"index\":0," },     /* arg chunk: hello */
		{ nil, "{\"choices\":[{\"index\":0," },     /* arg chunk: "] */
		{ nil, "{\"choices\":[{\"index\":0," },     /* arg chunk: } */
		{ nil, "{\"choices\":[{\"finish_reason\":\"tool_calls\"" },
	};
	run_fixture("oai_tool", oai_tool_sse, sizeof oai_tool_sse - 1,
	            expect, nelem(expect));
}

static void
test_ant_tool(void)
{
	/* 11 named events then [DONE] */
	ExpectedEvent expect[] = {
		{ "message_start",       "{\"message\":" },
		{ "content_block_start", "{\"content_block\":{\"id\":" },
		{ "content_block_delta", "{\"delta\":{\"partial_json\":\"\"" },  /* empty */
		{ "content_block_delta", "{\"delta\":{\"partial_json\":\"{\\\"\"" },
		{ "content_block_delta", "{\"delta\":{\"partial_json\":\"ar\"" },
		{ "content_block_delta", "{\"delta\":{\"partial_json\":\"gv" },
		{ "content_block_delta", "{\"delta\":{\"partial_json\":\"echo" },
		{ "content_block_delta", "{\"delta\":{\"partial_json\":\"\\\"]}\"" },
		{ "content_block_stop",  "{\"index\":0" },
		{ "message_delta",       "{\"copilot_usage\":" },
		{ "message_stop",        "{\"amazon-bedrock" },
	};
	run_fixture("ant_tool", ant_tool_sse, sizeof ant_tool_sse - 1,
	            expect, nelem(expect));
}

/*
 * test_eof_without_done — feed a stream that ends without [DONE].
 * ssestep must return SSE_EOF, not hang or crash.
 */
static void
test_eof_without_done(void)
{
	static const char data[] =
		"event: content_block_delta\n"
		"data: {\"delta\":{\"text\":\"hi\"}}\n"
		"\n";
	/* deliberately no [DONE] */

	HTTPResp *r;
	SSEParser p;
	SSEEvent  ev;
	int rc;

	r = make_resp(data, sizeof data - 1);
	sseinit(&p, r);

	rc = ssestep(&p, &ev);
	CHECKEQ(rc, SSE_OK, "eof_without_done: first step ok");
	CHECKSTR(ev.event, "content_block_delta", "eof_without_done: event name");

	rc = ssestep(&p, &ev);
	CHECKEQ(rc, SSE_EOF, "eof_without_done: second step is SSE_EOF");

	free_resp(r);
}

/*
 * test_comments — lines starting with ':' are SSE comments and must be skipped.
 */
static void
test_comments(void)
{
	static const char data[] =
		": this is a comment\n"
		"data: {\"hello\":1}\n"
		"\n"
		"data: [DONE]\n"
		"\n";

	HTTPResp *r;
	SSEParser p;
	SSEEvent  ev;
	int rc;

	r = make_resp(data, sizeof data - 1);
	sseinit(&p, r);

	rc = ssestep(&p, &ev);
	CHECKEQ(rc, SSE_OK, "comments: data event received");
	CHECKPREFIX(ev.data, "{\"hello\"", "comments: data correct");
	CHECKNIL(ev.event, "comments: no event name");

	rc = ssestep(&p, &ev);
	CHECKEQ(rc, SSE_DONE, "comments: DONE after comment");

	free_resp(r);
}

/* ── Integration test ────────────────────────────────────────────────── */

static void
test_live_oai(char *session, char *sockpath)
{
	HTTPConn *c;
	HTTPResp *r;
	SSEParser p;
	SSEEvent  ev;
	char auth[1200];
	HTTPHdr hdrs[8];
	int nhdrs, rc, data_events, found_done;
	char body[] =
		"{\"model\":\"gpt-4o\",\"stream\":true,"
		"\"max_completion_tokens\":40,"
		"\"messages\":[{\"role\":\"user\","
		"\"content\":\"reply with exactly: hello world\"}]}";
	char text[256];

	print("  live OAI /chat/completions...\n");

	snprint(auth, sizeof auth, "Bearer %s", session);
	nhdrs = 0;
	hdrs[nhdrs].name = "Authorization";          hdrs[nhdrs].value = auth;                        nhdrs++;
	hdrs[nhdrs].name = "Content-Type";           hdrs[nhdrs].value = "application/json";          nhdrs++;
	hdrs[nhdrs].name = "User-Agent";             hdrs[nhdrs].value = "GitHubCopilotChat/0.35.0";  nhdrs++;
	hdrs[nhdrs].name = "Editor-Version";         hdrs[nhdrs].value = "vscode/1.107.0";            nhdrs++;
	hdrs[nhdrs].name = "Editor-Plugin-Version";  hdrs[nhdrs].value = "copilot-chat/0.35.0";       nhdrs++;
	hdrs[nhdrs].name = "Copilot-Integration-Id"; hdrs[nhdrs].value = "vscode-chat";               nhdrs++;
	hdrs[nhdrs].name = "Openai-Intent";          hdrs[nhdrs].value = "conversation-edits";        nhdrs++;

	c = portdial("api.individual.githubcopilot.com", "443", sockpath);
	if(c == nil) sysfatal("httpdial: %r");

	r = httppost(c, "/chat/completions", "api.individual.githubcopilot.com",
	             hdrs, nhdrs, body, strlen(body));
	if(r == nil) sysfatal("httppost: %r");

	CHECK(r->code == 200, "live OAI: HTTP 200");

	sseinit(&p, r);
	data_events = 0;
	found_done  = 0;
	text[0] = '\0';

	while((rc = ssestep(&p, &ev)) == SSE_OK) {
		data_events++;
		/* OAI has no event: lines */
		CHECKNIL(ev.event, "live OAI: event field is nil");
		/* accumulate text content from delta.content */
		{
			jsmntok_t toks[256];
			jsmn_parser jp;
			int n, ci, c0, di, vi;
			char chunk[64];

			jsmn_init(&jp);
			n = jsmn_parse(&jp, ev.data, strlen(ev.data), toks, 256);
			if(n < 0) continue;
			ci = jsonget(ev.data, toks, n, 0, "choices");
			if(ci < 0 || toks[ci].size == 0) continue;
			c0 = ci + 1;
			di = jsonget(ev.data, toks, n, c0, "delta");
			if(di < 0) continue;
			vi = jsonget(ev.data, toks, n, di, "content");
			if(vi < 0 || toks[vi].type != JSMN_STRING) continue;
			jsonstr(ev.data, &toks[vi], chunk, sizeof chunk);
			if(strlen(text) + strlen(chunk) < sizeof text - 1)
				strcat(text, chunk);
		}
	}
	found_done = (rc == SSE_DONE);

	CHECK(data_events > 0, "live OAI: got data events");
	CHECK(found_done,       "live OAI: stream ended with SSE_DONE");
	CHECK(strstr(text, "hello") != nil || strstr(text, "Hello") != nil,
	      "live OAI: text contains 'hello'");
	print("  assembled text: %s\n", text);

	httprespfree(r);
	httpclose(c);
}

static void
test_live_ant(char *session, char *sockpath)
{
	HTTPConn *c;
	HTTPResp *r;
	SSEParser p;
	SSEEvent  ev;
	char auth[1200];
	HTTPHdr hdrs[8];
	int nhdrs, rc, data_events, found_done;
	char body[] =
		"{\"model\":\"claude-sonnet-4.5\",\"stream\":true,"
		"\"max_tokens\":40,"
		"\"messages\":[{\"role\":\"user\","
		"\"content\":\"reply with exactly: hello world\"}]}";
	char text[256];

	print("  live ANT /v1/messages...\n");

	snprint(auth, sizeof auth, "Bearer %s", session);
	nhdrs = 0;
	hdrs[nhdrs].name = "Authorization";          hdrs[nhdrs].value = auth;                        nhdrs++;
	hdrs[nhdrs].name = "Content-Type";           hdrs[nhdrs].value = "application/json";          nhdrs++;
	hdrs[nhdrs].name = "User-Agent";             hdrs[nhdrs].value = "GitHubCopilotChat/0.35.0";  nhdrs++;
	hdrs[nhdrs].name = "Editor-Version";         hdrs[nhdrs].value = "vscode/1.107.0";            nhdrs++;
	hdrs[nhdrs].name = "Editor-Plugin-Version";  hdrs[nhdrs].value = "copilot-chat/0.35.0";       nhdrs++;
	hdrs[nhdrs].name = "Copilot-Integration-Id"; hdrs[nhdrs].value = "vscode-chat";               nhdrs++;

	c = portdial("api.individual.githubcopilot.com", "443", sockpath);
	if(c == nil) sysfatal("httpdial: %r");

	r = httppost(c, "/v1/messages", "api.individual.githubcopilot.com",
	             hdrs, nhdrs, body, strlen(body));
	if(r == nil) sysfatal("httppost: %r");

	CHECK(r->code == 200, "live ANT: HTTP 200");

	sseinit(&p, r);
	data_events = 0;
	found_done  = 0;
	text[0] = '\0';

	while((rc = ssestep(&p, &ev)) == SSE_OK) {
		data_events++;
		/* ANT must have event names for all but [DONE] */
		if(ev.event == nil) {
			/* [DONE] is caught before SSE_OK, so this shouldn't happen */
			fprint(2, "WARN: ANT event has nil event name: %.40s\n", ev.data);
		}
		/* extract text_delta chunks */
		if(ev.event != nil && strcmp(ev.event, "content_block_delta") == 0) {
			jsmntok_t toks[64];
			jsmn_parser jp;
			int n, di, vi;
			char chunk[64], dtype[32];

			jsmn_init(&jp);
			n = jsmn_parse(&jp, ev.data, strlen(ev.data), toks, 64);
			if(n < 0) continue;
			di = jsonget(ev.data, toks, n, 0, "delta");
			if(di < 0) continue;
			vi = jsonget(ev.data, toks, n, di, "type");
			if(vi < 0) continue;
			jsonstr(ev.data, &toks[vi], dtype, sizeof dtype);
			if(strcmp(dtype, "text_delta") != 0) continue;
			vi = jsonget(ev.data, toks, n, di, "text");
			if(vi < 0) continue;
			jsonstr(ev.data, &toks[vi], chunk, sizeof chunk);
			if(strlen(text) + strlen(chunk) < sizeof text - 1)
				strcat(text, chunk);
		}
	}
	found_done = (rc == SSE_DONE);

	CHECK(data_events > 0, "live ANT: got data events");
	CHECK(found_done,       "live ANT: stream ended with SSE_DONE");
	CHECK(strstr(text, "hello") != nil || strstr(text, "Hello") != nil,
	      "live ANT: text contains 'hello'");
	print("  assembled text: %s\n", text);

	httprespfree(r);
	httpclose(c);
}

static void
test_live(char *sockpath, char *tokpath)
{
	char buf[4096];
	int fd;
	long len;
	OAuthToken *ot;

	print("=== integration tests: live SSE ===\n");

	fd = open(tokpath, OREAD);
	if(fd < 0) sysfatal("open %s: %r", tokpath);
	len = readn(fd, buf, sizeof buf - 1);
	close(fd);
	if(len <= 0) sysfatal("empty: %s", tokpath);
	buf[len] = '\0';
	while(len > 0 && (buf[len-1]=='\n'||buf[len-1]=='\r'||buf[len-1]==' '))
		buf[--len] = '\0';

	ot = oauthsession(buf, sockpath);
	if(ot == nil) sysfatal("oauthsession: %r");

	test_live_oai(ot->token, sockpath);
	test_live_ant(ot->token, sockpath);

	oauthtokenfree(ot);
	print("integration tests done\n\n");
}

/* ── Main ────────────────────────────────────────────────────────────── */

void
threadmain(int argc, char *argv[])
{
	char *sockpath = nil, *tokpath = nil;

	ARGBEGIN{
	case 's': sockpath = ARGF(); break;
	case 't': tokpath  = ARGF(); break;
	}ARGEND
	USED(argv);

	print("=== unit tests: SSE parser ===\n");
	test_oai_text();
	test_ant_text();
	test_oai_tool();
	test_ant_tool();
	test_eof_without_done();
	test_comments();
	print("unit tests done\n\n");

	if(sockpath != nil && tokpath != nil)
		test_live(sockpath, tokpath);
	else
		print("(skipping integration tests: pass -s sockpath -t tokenfile)\n");

	if(failures > 0) {
		fprint(2, "\n%d failure(s)\n", failures);
		threadexitsall("FAIL");
	}
	print("all tests passed\n");
	threadexitsall(nil);
}
