/*
 * anttest.c — unit and integration tests for the Anthropic Messages
 *             request builder and SSE delta parser (ant.c)
 *
 * Part 1: Unit tests (no network)
 *   1.1  Request JSON — user message, system prompt, stream, max_tokens
 *   1.2  Request JSON — assistant message content is an array
 *   1.3  Request JSON — tool_use block: id, name, input as JSON object
 *   1.4  Request JSON — tool_result block in user message content array
 *   1.5  Request JSON — thinking blocks are NOT serialised
 *   1.6  antreqhdrs  — X-Initiator "user" when last msg is user
 *   1.7  antreqhdrs  — X-Initiator "agent" when last msg is assistant
 *
 * Part 2: Unit tests — delta parser on fixtures (no network)
 *   2.1  ant_text_sse: ANTDText deltas → assembled text "hello world"
 *   2.2  ant_text_sse: final delta is ANTDStop with "end_turn"
 *   2.3  ant_text_sse: return ANT_DONE after stop
 *   2.4  ant_tool_sse: first delta is ANTDTool with name "exec"
 *   2.5  ant_tool_sse: ANTDToolArg deltas assemble to {"argv":["echo","hello"]}
 *   2.6  ant_tool_sse: final delta is ANTDStop with "tool_use"
 *   2.7  ant_tool_sse: return ANT_DONE after stop
 *
 * Part 3: Live integration tests (requires -t tokenpath)
 *   3.1  POST claude-sonnet-4.5 "say hello world and nothing else"
 *        — ANTDText deltas received; assembled text contains "hello" and "world"
 *        — ANTDStop with stop_reason "end_turn"
 *        — return ANT_DONE
 *   3.2  POST claude-sonnet-4.5 "list the files in /tmp using the exec tool"
 *        — ANTDTool delta received with tool_name "exec"
 *        — ANTDToolArg chunks received
 *        — ANTDStop with stop_reason "tool_use"
 *        — return ANT_DONE
 *
 * Usage:
 *   ./o.anttest                     (unit tests only)
 *   ./o.anttest -s <sock> -t <tok>  (unit + live)
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
#include "ant.h"

static int failures = 0;

/* ── Test macros ────────────────────────────────────────────────────── */

#define CHECK(cond, msg) do { \
	if(!(cond)) { \
		fprint(2, "FAIL: %s\n", msg); \
		failures++; \
	} else { \
		print("ok:   %s\n", msg); \
	} \
} while(0)

#define CHECKEQ(a, b, msg) do { \
	if((long)(a) != (long)(b)) { \
		fprint(2, "FAIL: %s: got %ld want %ld\n", \
		       msg, (long)(a), (long)(b)); \
		failures++; \
	} else { \
		print("ok:   %s\n", msg); \
	} \
} while(0)

#define CHECKSTR(a, b, msg) do { \
	const char *_a = (a), *_b = (b); \
	if(_a == nil || strcmp(_a, _b) != 0) { \
		fprint(2, "FAIL: %s: got \"%s\" want \"%s\"\n", \
		       msg, _a ? _a : "(nil)", _b); \
		failures++; \
	} else { \
		print("ok:   %s\n", msg); \
	} \
} while(0)

#define CHECKCONTAINS(s, sub, msg) do { \
	const char *_s = (s), *_sub = (sub); \
	if(_s == nil || strstr(_s, _sub) == nil) { \
		fprint(2, "FAIL: %s: \"%s\" does not contain \"%s\"\n", \
		       msg, _s ? _s : "(nil)", _sub); \
		failures++; \
	} else { \
		print("ok:   %s\n", msg); \
	} \
} while(0)

#define CHECKNONIL(a, msg) do { \
	if((a) == nil) { \
		fprint(2, "FAIL: %s: got nil\n", msg); \
		failures++; \
	} else { \
		print("ok:   %s\n", msg); \
	} \
} while(0)

#define CHECKNIL(a, msg) do { \
	if((a) != nil) { \
		fprint(2, "FAIL: %s: expected nil, got non-nil\n", msg); \
		failures++; \
	} else { \
		print("ok:   %s\n", msg); \
	} \
} while(0)

/* ── Fixtures (captured from live Copilot API via ssetest.c) ────────── */

/* ant_text_sse: text response with "hello world", stop_reason "end_turn" */
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

/* ant_tool_sse: tool_use response, exec("echo","hello"), stop_reason "tool_use" */
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

/* ── Fixture HTTPResp factory ────────────────────────────────────────── */

static HTTPResp *
make_resp(const char *data, long len)
{
	int p[2];
	HTTPConn *c;
	HTTPResp *r;

	if(pipe(p) < 0)
		sysfatal("pipe: %r");
	if(write(p[1], (char*)data, len) != len)
		sysfatal("write pipe: %r");
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

/* ── Part 1: Request builder unit tests ─────────────────────────────── */

static void
test_req_user_system(void)
{
	ANTReq *req;
	char   *body;
	long    len;

	print("-- 1.1 user message + system prompt\n");
	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("hello"));
	body = antreqjson(req, "you are helpful", &len);

	CHECKNONIL(body, "req JSON non-nil");
	CHECKCONTAINS(body, "\"model\":\"claude-sonnet-4.5\"", "model field");
	CHECKCONTAINS(body, "\"stream\":true", "stream true");
	CHECKCONTAINS(body, "\"max_tokens\":32000", "max_tokens 32000");
	/* system must now be a block array, not a plain string */
	CHECKCONTAINS(body, "\"system\":[", "system is array");
	CHECKCONTAINS(body, "\"type\":\"text\"", "system block type text");
	CHECKCONTAINS(body, "you are helpful", "system text present");
	CHECKCONTAINS(body, "\"cache_control\":{\"type\":\"ephemeral\"}", "system has cache_control");
	/* must NOT emit system as a bare string */
	CHECK(strstr(body, "\"system\":\"") == nil, "system not bare string");
	CHECKCONTAINS(body, "\"role\":\"user\"", "user role");
	CHECKCONTAINS(body, "hello", "user text");
	CHECKCONTAINS(body, "\"input_schema\"", "input_schema (not parameters)");
	/* must NOT use "parameters" for tool declaration */
	CHECK(strstr(body, "\"parameters\"") == nil, "no parameters key");
	/* must NOT use max_completion_tokens */
	CHECK(strstr(body, "max_completion_tokens") == nil, "no max_completion_tokens");

	free(body);
	antreqfree(req);
}

static void
test_req_assistant_array(void)
{
	ANTReq *req;
	char   *body;
	long    len;

	print("-- 1.2 assistant message → content array\n");
	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("hi"));
	antreqaddmsg(req, antmsgassistant("hello there"));
	body = antreqjson(req, nil, &len);

	CHECKNONIL(body, "req JSON non-nil");
	CHECKCONTAINS(body, "\"role\":\"assistant\"", "assistant role");
	/* assistant content must be a JSON array */
	CHECKCONTAINS(body, "\"content\":[", "content is array");
	CHECKCONTAINS(body, "\"type\":\"text\"", "text block type");
	CHECKCONTAINS(body, "hello there", "assistant text");

	free(body);
	antreqfree(req);
}

static void
test_req_tooluse(void)
{
	ANTReq *req;
	char   *body;
	long    len;

	print("-- 1.3 tool_use block\n");
	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("list /tmp"));
	antreqaddmsg(req, antmsgtooluse(nil,
	    "toolu_01abc", "exec", "{\"argv\":[\"ls\",\"/tmp\"]}"));
	body = antreqjson(req, nil, &len);

	CHECKNONIL(body, "req JSON non-nil");
	CHECKCONTAINS(body, "\"type\":\"tool_use\"", "tool_use type");
	CHECKCONTAINS(body, "\"id\":\"toolu_01abc\"", "tool id");
	CHECKCONTAINS(body, "\"name\":\"exec\"", "tool name");
	/* input must be a JSON object, not a string */
	CHECKCONTAINS(body, "\"input\":{", "input is object");
	CHECKCONTAINS(body, "\"argv\":[\"ls\",\"/tmp\"]", "argv in input");

	free(body);
	antreqfree(req);
}

static void
test_req_toolresult(void)
{
	ANTReq *req;
	char   *body;
	long    len;

	print("-- 1.4 tool_result block in user message\n");
	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("list /tmp"));
	antreqaddmsg(req, antmsgtooluse(nil,
	    "toolu_01abc", "exec", "{\"argv\":[\"ls\",\"/tmp\"]}"));
	antreqaddmsg(req, antmsgtoolresult("toolu_01abc", "file1\nfile2\n", 0));
	body = antreqjson(req, nil, &len);

	CHECKNONIL(body, "req JSON non-nil");
	CHECKCONTAINS(body, "\"type\":\"tool_result\"", "tool_result type");
	CHECKCONTAINS(body, "\"tool_use_id\":\"toolu_01abc\"", "tool_use_id");
	CHECKCONTAINS(body, "file1", "output content");
	CHECKCONTAINS(body, "\"is_error\":false", "is_error false");

	free(body);
	antreqfree(req);
}

static void
test_req_thinking_skipped(void)
{
	ANTReq   *req;
	ANTMsg   *m;
	ANTBlock *b;
	char     *body;
	long      len;

	print("-- 1.5 thinking blocks are NOT serialised\n");
	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("think about it"));

	/* manually build an assistant message with a thinking block */
	m = mallocz(sizeof *m, 1);
	m->role = ANTRoleAssistant;

	b = mallocz(sizeof *b, 1);
	b->type = ANTBlockThinking;
	b->text = strdup("some internal thought");
	m->content = b;

	ANTBlock *b2 = mallocz(sizeof *b2, 1);
	b2->type = ANTBlockText;
	b2->text = strdup("my answer");
	b->next = b2;

	antreqaddmsg(req, m);
	body = antreqjson(req, nil, &len);

	CHECKNONIL(body, "req JSON non-nil");
	/* thinking text must not appear */
	CHECK(strstr(body, "some internal thought") == nil, "thinking not serialised");
	/* regular text must appear */
	CHECKCONTAINS(body, "my answer", "text block present");

	free(body);
	antreqfree(req);
}

static void
test_hdrs_initiator_user(void)
{
	ANTReq  *req;
	HTTPHdr  hdrs[16];
	int      n, i;
	int      found_auth = 0, found_init = 0;
	char    *init_val = nil;

	print("-- 1.6 X-Initiator user\n");
	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("hi"));
	n = antreqhdrs(req, "test-token", hdrs, 16);

	CHECK(n >= 7, "at least 7 headers");
	for(i = 0; i < n; i++) {
		if(strcmp(hdrs[i].name, "Authorization") == 0) {
			found_auth = 1;
			CHECKCONTAINS(hdrs[i].value, "test-token", "auth has token");
		}
		if(strcmp(hdrs[i].name, "X-Initiator") == 0) {
			found_init = 1;
			init_val = hdrs[i].value;
		}
	}
	CHECK(found_auth, "Authorization header present");
	CHECK(found_init, "X-Initiator header present");
	CHECKSTR(init_val, "user", "X-Initiator is user");

	/* free smprint'd Authorization value */
	for(i = 0; i < n; i++)
		if(strcmp(hdrs[i].name, "Authorization") == 0)
			free(hdrs[i].value);
	antreqfree(req);
}

static void
test_hdrs_initiator_agent(void)
{
	ANTReq  *req;
	HTTPHdr  hdrs[16];
	int      n, i;
	char    *init_val = nil;

	print("-- 1.7 X-Initiator agent\n");
	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("hi"));
	antreqaddmsg(req, antmsgassistant("hello"));
	n = antreqhdrs(req, "tok", hdrs, 16);

	for(i = 0; i < n; i++)
		if(strcmp(hdrs[i].name, "X-Initiator") == 0)
			init_val = hdrs[i].value;
	CHECKSTR(init_val, "agent", "X-Initiator is agent");

	for(i = 0; i < n; i++)
		if(strcmp(hdrs[i].name, "Authorization") == 0)
			free(hdrs[i].value);
	antreqfree(req);
}

/* ── Part 2: Delta parser unit tests on fixtures ─────────────────────── */

static void
test_delta_text(void)
{
	HTTPResp *r;
	ANTParser p;
	ANTDelta  d;
	int       rc;
	char      text[4096];
	int       ntext = 0, nstop = 0;

	print("-- 2.1/2.2/2.3 ant_text_sse fixture\n");

	r = make_resp(ant_text_sse, sizeof ant_text_sse - 1);
	antinit(&p, r);
	text[0] = '\0';

	while((rc = antdelta(&p, &d)) == ANT_OK) {
		switch(d.type) {
		case ANTDText:
			ntext++;
			strncat(text, d.text, sizeof text - strlen(text) - 1);
			break;
		case ANTDStop:
			nstop++;
			/* 2.2: stop_reason must be "end_turn" */
			CHECKSTR(d.stop_reason, "end_turn", "stop_reason end_turn");
			break;
		}
	}

	/* 2.1: assembled text */
	CHECKCONTAINS(text, "hello", "text contains hello");
	CHECKCONTAINS(text, "world", "text contains world");
	CHECK(ntext >= 2, "at least 2 text deltas");
	/* 2.2: exactly one stop */
	CHECKEQ(nstop, 1, "exactly one stop delta");
	/* 2.3: loop ended with ANT_DONE */
	CHECKEQ(rc, ANT_DONE, "returns ANT_DONE");

	antterm(&p);
	free_resp(r);
}

static void
test_delta_tool(void)
{
	HTTPResp *r;
	ANTParser p;
	ANTDelta  d;
	int       rc;
	char      args[4096];
	int       ntool = 0, ntoolarg = 0, nstop = 0;
	char      tool_name_seen[128];

	print("-- 2.4/2.5/2.6/2.7 ant_tool_sse fixture\n");

	r = make_resp(ant_tool_sse, sizeof ant_tool_sse - 1);
	antinit(&p, r);
	args[0]          = '\0';
	tool_name_seen[0] = '\0';

	while((rc = antdelta(&p, &d)) == ANT_OK) {
		switch(d.type) {
		case ANTDTool:
			ntool++;
			/* 2.4: tool name */
			CHECKSTR(d.tool_name, "exec", "tool name is exec");
			CHECKNONIL(d.tool_id, "tool_id non-nil");
			strncat(tool_name_seen, d.tool_name, sizeof tool_name_seen - 1);
			break;
		case ANTDToolArg:
			ntoolarg++;
			strncat(args, d.text, sizeof args - strlen(args) - 1);
			break;
		case ANTDStop:
			nstop++;
			/* 2.6: stop_reason must be "tool_use" */
			CHECKSTR(d.stop_reason, "tool_use", "stop_reason tool_use");
			break;
		}
	}

	/* 2.4: exactly one ANTDTool delta */
	CHECKEQ(ntool, 1, "exactly one tool delta");
	/* 2.5: assembled args contain echo and hello */
	CHECKCONTAINS(args, "echo", "args contain echo");
	CHECKCONTAINS(args, "hello", "args contain hello");
	/* 2.6: exactly one stop */
	CHECKEQ(nstop, 1, "exactly one stop delta");
	/* 2.7: loop ended with ANT_DONE */
	CHECKEQ(rc, ANT_DONE, "returns ANT_DONE");

	antterm(&p);
	free_resp(r);
}

/* ── Part 2.5: antreqctxtokens and antreqtrim ────────────────────────── */

static void
test_ant_ctxtokens_empty(void)
{
	ANTReq *req;

	print("-- 2.5 antreqctxtokens: empty req → 0\n");

	req = antreqnew("claude-sonnet-4.5");
	CHECKEQ(antreqctxtokens(req), 0, "empty req has 0 tokens");
	antreqfree(req);
}

static void
test_ant_ctxtokens_counts_text(void)
{
	ANTReq *req;
	long    est;

	print("-- 2.5 antreqctxtokens: 8-char user message → 2 tokens\n");

	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("12345678"));  /* exactly 8 chars */
	est = antreqctxtokens(req);
	CHECKEQ(est, 2, "8 chars / 4 = 2 tokens");
	antreqfree(req);
}

static void
test_ant_ctxtokens_counts_tool_input(void)
{
	ANTReq *req;
	long    est;

	print("-- 2.5 antreqctxtokens: tool_input counted\n");

	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("go"));
	/* tool use with 8-char input JSON */
	antreqaddmsg(req, antmsgtooluse(nil, "toolu_01", "exec", "12345678"));
	est = antreqctxtokens(req);
	/* "go" = 2 + "12345678" = 8 → 10 → ceil(10/4) = 3 */
	CHECKEQ(est, 3, "user text + tool_input counted: ceil(10/4)=3");
	antreqfree(req);
}

static void
test_ant_trim_zero_noop(void)
{
	ANTReq *req;
	int     removed;

	print("-- 2.5 antreqtrim: trim 0 is no-op\n");

	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("hello"));
	antreqaddmsg(req, antmsgassistant("world"));
	removed = antreqtrim(req, 0);
	CHECKEQ(removed, 0,     "trim 0 removed 0 messages");
	CHECK(req->msgs != nil, "list non-empty after trim 0");
	antreqfree(req);
}

static void
test_ant_trim_one_turn(void)
{
	ANTReq *req;
	int     removed;

	print("-- 2.5 antreqtrim: trim 1 removes first user+assistant pair\n");

	req = antreqnew("claude-sonnet-4.5");
	/* turn 1 */
	antreqaddmsg(req, antmsguser("first question"));
	antreqaddmsg(req, antmsgassistant("first answer"));
	/* turn 2 */
	antreqaddmsg(req, antmsguser("second question"));
	antreqaddmsg(req, antmsgassistant("second answer"));

	removed = antreqtrim(req, 1);

	CHECK(removed > 0, "trim 1 removed messages");
	CHECK(req->msgs != nil, "list non-empty after trim");
	CHECKEQ(req->msgs->role, ANTRoleUser, "head is user message");
	{
		char *body = antreqjson(req, nil, nil);
		CHECK(body != nil, "serialised after trim");
		CHECKCONTAINS(body, "second question", "second turn present");
		CHECK(strstr(body, "first question") == nil, "first turn absent");
		free(body);
	}
	antreqfree(req);
}

static void
test_ant_trim_with_tool_turn(void)
{
	ANTReq *req;
	int     removed;

	print("-- 2.5 antreqtrim: trim 1 removes turn with tool_use+tool_result\n");

	req = antreqnew("claude-sonnet-4.5");
	/* turn 1: user → assistant tool_use → user tool_result */
	antreqaddmsg(req, antmsguser("list files"));
	antreqaddmsg(req, antmsgtooluse(nil, "toolu_01", "exec",
	    "{\"argv\":[\"ls\"]}"));
	antreqaddmsg(req, antmsgtoolresult("toolu_01", "file1\nfile2\n", 0));
	/* turn 2 */
	antreqaddmsg(req, antmsguser("what did you find?"));
	antreqaddmsg(req, antmsgassistant("two files"));

	removed = antreqtrim(req, 1);

	CHECK(removed >= 3, "at least 3 messages removed");
	{
		char *body = antreqjson(req, nil, nil);
		CHECK(body != nil, "serialised after trim");
		CHECKCONTAINS(body, "what did you find?", "turn 2 present");
		CHECK(strstr(body, "list files") == nil, "turn 1 user gone");
		CHECK(strstr(body, "file1") == nil,      "turn 1 tool result gone");
		free(body);
	}
	antreqfree(req);
}

static void
test_ant_trim_all(void)
{
	ANTReq *req;
	int     removed;

	print("-- 2.5 antreqtrim: trim N > actual turns removes everything\n");

	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("only question"));
	antreqaddmsg(req, antmsgassistant("only answer"));

	removed = antreqtrim(req, 99);

	CHECK(removed > 0,   "messages were removed");
	CHECKNIL(req->msgs,     "msgs nil after trim all");
	CHECKNIL(req->msgtail,  "msgtail nil after trim all");
	antreqfree(req);
}

static void
test_ant_trim_msgtail_updated(void)
{
	ANTReq *req;
	char   *body;

	print("-- 2.5 antreqtrim: msgtail updated; append after trim works\n");

	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("alpha"));
	antreqaddmsg(req, antmsgassistant("beta"));
	antreqaddmsg(req, antmsguser("gamma"));
	antreqaddmsg(req, antmsgassistant("delta"));

	antreqtrim(req, 1);
	antreqaddmsg(req, antmsguser("epsilon"));

	body = antreqjson(req, nil, nil);
	CHECK(body != nil, "serialised after trim + append");
	CHECKCONTAINS(body, "gamma",   "turn 2 present");
	CHECKCONTAINS(body, "epsilon", "appended message present");
	CHECK(strstr(body, "alpha") == nil, "turn 1 absent");
	free(body);
	antreqfree(req);
}

/* ── Part 2.6: cache_control placement ──────────────────────────────── */

static void
test_cache_system_block_array(void)
{
	ANTReq *req;
	char   *body;

	print("-- 2.6 system prompt emitted as block array with cache_control\n");

	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("hi"));
	body = antreqjson(req, "be helpful", nil);

	CHECKNONIL(body, "body non-nil");
	CHECKCONTAINS(body, "\"system\":[", "system is array");
	CHECKCONTAINS(body, "\"type\":\"text\"", "system text block");
	CHECKCONTAINS(body, "be helpful", "system text present");
	CHECKCONTAINS(body, "\"cache_control\":{\"type\":\"ephemeral\"}", "system cache_control");
	CHECK(strstr(body, "\"system\":\"") == nil, "system not bare string");

	free(body);
	antreqfree(req);
}

static void
test_cache_no_system_omitted(void)
{
	ANTReq *req;
	char   *body;

	print("-- 2.6 nil system prompt → no system field at all\n");

	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("hi"));
	body = antreqjson(req, nil, nil);

	CHECKNONIL(body, "body non-nil");
	CHECK(strstr(body, "\"system\"") == nil, "no system field");

	free(body);
	antreqfree(req);
}

static void
test_cache_last_user_text(void)
{
	ANTReq *req;
	char   *body;

	print("-- 2.6 last user message gets cache_control; earlier ones do not\n");

	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("first question"));
	antreqaddmsg(req, antmsgassistant("first answer"));
	antreqaddmsg(req, antmsguser("second question"));
	body = antreqjson(req, nil, nil);

	CHECKNONIL(body, "body non-nil");
	/* last user message must have cache_control */
	CHECKCONTAINS(body, "\"cache_control\":{\"type\":\"ephemeral\"}", "cache_control present");
	/* the second user message is emitted as a content array (block form) */
	/* first user message is a plain string and must NOT have cache_control
	 * attached (it appears before the assistant message, so it's not last) */
	{
		/*
		 * Verify that exactly one cache_control appears in the messages[]
		 * portion (there may also be one in the system block).
		 * Strategy: count occurrences of "cache_control" in the body.
		 * With a nil system prompt there is exactly one.
		 */
		int count = 0;
		const char *p = body;
		while((p = strstr(p, "\"cache_control\"")) != nil) {
			count++;
			p++;
		}
		CHECKEQ(count, 1, "exactly one cache_control in body (no system)");
	}

	free(body);
	antreqfree(req);
}

static void
test_cache_with_system_two_markers(void)
{
	ANTReq *req;
	char   *body;
	int     count;
	const char *p;

	print("-- 2.6 system + last user → two cache_control markers\n");

	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("hello"));
	body = antreqjson(req, "be concise", nil);

	CHECKNONIL(body, "body non-nil");

	count = 0;
	p = body;
	while((p = strstr(p, "\"cache_control\"")) != nil) {
		count++;
		p++;
	}
	CHECKEQ(count, 2, "two cache_control markers: system + last user");

	free(body);
	antreqfree(req);
}

static void
test_cache_tool_result_not_marked(void)
{
	ANTReq *req;
	char   *body;
	int     count;
	const char *p;

	print("-- 2.6 last message is tool_result → user text before it gets cache_control\n");

	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("list files"));
	antreqaddmsg(req, antmsgtooluse(nil, "toolu_01", "exec",
	    "{\"argv\":[\"ls\"]}"));
	antreqaddmsg(req, antmsgtoolresult("toolu_01", "file1\nfile2\n", 0));
	body = antreqjson(req, nil, nil);

	CHECKNONIL(body, "body non-nil");

	/*
	 * The last message is a tool_result (user role, but pure tool_result
	 * blocks).  The pre-pass must skip it and mark the "list files" user
	 * message instead.
	 *
	 * With nil system prompt there should be exactly one cache_control.
	 */
	count = 0;
	p = body;
	while((p = strstr(p, "\"cache_control\"")) != nil) {
		count++;
		p++;
	}
	CHECKEQ(count, 1, "exactly one cache_control on the user text message");

	/* the tool_result block itself must not have cache_control */
	{
		const char *tr = strstr(body, "\"type\":\"tool_result\"");
		CHECK(tr != nil, "tool_result block present");
		if(tr != nil) {
			/* look for cache_control between tool_result and next '}' */
			const char *end = strchr(tr, '}');
			int found = 0;
			if(end != nil) {
				char tmp[256];
				long n = end - tr;
				if(n >= (long)sizeof tmp) n = sizeof tmp - 1;
				memmove(tmp, tr, n);
				tmp[n] = '\0';
				found = (strstr(tmp, "cache_control") != nil);
			}
			CHECK(!found, "tool_result block has no cache_control");
		}
	}

	free(body);
	antreqfree(req);
}

static void
test_cache_flag_cleared_after_call(void)
{
	ANTReq   *req;
	ANTMsg   *m;
	ANTBlock *blk;
	char     *body;

	print("-- 2.6 cache_control flag cleared on ANTBlock after antreqjson returns\n");

	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("hello"));
	body = antreqjson(req, nil, nil);
	free(body);

	/* verify no block has cache_control set after the call */
	for(m = req->msgs; m != nil; m = m->next)
		for(blk = m->content; blk != nil; blk = blk->next)
			CHECKEQ(blk->cache_control, 0, "cache_control cleared post-call");

	antreqfree(req);
}



static void
test_live_text(char *tokpath)
{
	char       *refresh, *errbuf;
	OAuthToken *tok;
	HTTPConn   *c;
	HTTPResp   *r;
	ANTReq     *req;
	ANTParser   p;
	ANTDelta    d;
	HTTPHdr     hdrs[16];
	int         nhdrs, rc;
	char       *body;
	long        bodylen;
	char        text[65536];
	char        stop_reason[64];
	int         fd, n;
	char        buf[512];

	print("-- 3.1 live text turn claude-sonnet-4.5\n");

	/* load refresh token */
	fd = open(tokpath, OREAD);
	if(fd < 0) { fprint(2, "SKIP: cannot open token %s\n", tokpath); return; }
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0) { fprint(2, "SKIP: empty token file\n"); return; }
	buf[n] = '\0';
	while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
	refresh = buf;

	tok = oauthsession(refresh);
	if(tok == nil) {
		rerrstr(buf, sizeof buf);
		fprint(2, "SKIP: oauthsession: %s\n", buf);
		return;
	}

	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser("Reply with exactly two words: hello world"));
	nhdrs = antreqhdrs(req, tok->token, hdrs, 16);

	body = antreqjson(req, nil, &bodylen);
	if(body == nil) { fprint(2, "FAIL: antreqjson nil\n"); failures++; oauthtokenfree(tok); antreqfree(req); return; }

	c = tlsdial("api.individual.githubcopilot.com", "443");
	if(c == nil) { fprint(2, "SKIP: tlsdial failed\n"); free(body); oauthtokenfree(tok); antreqfree(req); return; }

	r = httppost(c, "/v1/messages", "api.individual.githubcopilot.com",
	             hdrs, nhdrs, body, bodylen);
	free(body);
	{
		int i;
		for(i = 0; i < nhdrs; i++)
			if(strcmp(hdrs[i].name, "Authorization") == 0)
				free(hdrs[i].value);
	}

	if(r == nil || r->code != 200) {
		if(r != nil) {
			httpreadbody(r);
			errbuf = r->body ? r->body : "(no body)";
			fprint(2, "FAIL 3.1: HTTP %d: %s\n", r->code, errbuf);
			httprespfree(r);
		} else {
			rerrstr(buf, sizeof buf);
			fprint(2, "FAIL 3.1: httppost: %s\n", buf);
		}
		failures++;
		httpclose(c);
		oauthtokenfree(tok);
		antreqfree(req);
		return;
	}

	antinit(&p, r);
	text[0]        = '\0';
	stop_reason[0] = '\0';
	int ntext = 0, nstop = 0;

	while((rc = antdelta(&p, &d)) == ANT_OK) {
		switch(d.type) {
		case ANTDText:
			ntext++;
			strncat(text, d.text, sizeof text - strlen(text) - 1);
			break;
		case ANTDStop:
			nstop++;
			strncat(stop_reason, d.stop_reason ? d.stop_reason : "", sizeof stop_reason - 1);
			break;
		}
	}

	antterm(&p);
	httprespfree(r);
	httpclose(c);
	oauthtokenfree(tok);
	antreqfree(req);

	CHECKEQ(rc, ANT_DONE, "3.1: ANT_DONE");
	CHECK(ntext > 0, "3.1: at least one text delta");
	CHECKCONTAINS(text, "hello", "3.1: text contains hello");
	CHECKCONTAINS(text, "world", "3.1: text contains world");
	CHECKEQ(nstop, 1, "3.1: one stop delta");
	CHECKSTR(stop_reason, "end_turn", "3.1: stop_reason end_turn");
}

static void
test_live_tool(char *tokpath)
{
	char       *refresh, *errbuf;
	OAuthToken *tok;
	HTTPConn   *c;
	HTTPResp   *r;
	ANTReq     *req;
	ANTParser   p;
	ANTDelta    d;
	HTTPHdr     hdrs[16];
	int         nhdrs, rc;
	char       *body;
	long        bodylen;
	char        args[65536];
	char        stop_reason[64];
	char        tool_name[128];
	int         fd, n;
	char        buf[512];

	print("-- 3.2 live tool turn claude-sonnet-4.5\n");

	fd = open(tokpath, OREAD);
	if(fd < 0) { fprint(2, "SKIP: cannot open token %s\n", tokpath); return; }
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0) { fprint(2, "SKIP: empty token file\n"); return; }
	buf[n] = '\0';
	while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
	refresh = buf;

	tok = oauthsession(refresh);
	if(tok == nil) {
		rerrstr(buf, sizeof buf);
		fprint(2, "SKIP: oauthsession: %s\n", buf);
		return;
	}

	req = antreqnew("claude-sonnet-4.5");
	antreqaddmsg(req, antmsguser(
	    "List the files in /tmp using the exec tool. "
	    "Use exactly: exec([\"ls\",\"/tmp\"])"));
	nhdrs = antreqhdrs(req, tok->token, hdrs, 16);

	body = antreqjson(req, nil, &bodylen);
	if(body == nil) { fprint(2, "FAIL: antreqjson nil\n"); failures++; oauthtokenfree(tok); antreqfree(req); return; }

	c = tlsdial("api.individual.githubcopilot.com", "443");
	if(c == nil) { fprint(2, "SKIP: tlsdial failed\n"); free(body); oauthtokenfree(tok); antreqfree(req); return; }

	r = httppost(c, "/v1/messages", "api.individual.githubcopilot.com",
	             hdrs, nhdrs, body, bodylen);
	free(body);
	{
		int i;
		for(i = 0; i < nhdrs; i++)
			if(strcmp(hdrs[i].name, "Authorization") == 0)
				free(hdrs[i].value);
	}

	if(r == nil || r->code != 200) {
		if(r != nil) {
			httpreadbody(r);
			errbuf = r->body ? r->body : "(no body)";
			fprint(2, "FAIL 3.2: HTTP %d: %s\n", r->code, errbuf);
			httprespfree(r);
		} else {
			rerrstr(buf, sizeof buf);
			fprint(2, "FAIL 3.2: httppost: %s\n", buf);
		}
		failures++;
		httpclose(c);
		oauthtokenfree(tok);
		antreqfree(req);
		return;
	}

	antinit(&p, r);
	args[0]       = '\0';
	stop_reason[0] = '\0';
	tool_name[0]   = '\0';
	int ntool = 0, ntoolarg = 0, nstop = 0;

	while((rc = antdelta(&p, &d)) == ANT_OK) {
		switch(d.type) {
		case ANTDTool:
			ntool++;
			strncat(tool_name, d.tool_name ? d.tool_name : "", sizeof tool_name - 1);
			break;
		case ANTDToolArg:
			ntoolarg++;
			strncat(args, d.text, sizeof args - strlen(args) - 1);
			break;
		case ANTDStop:
			nstop++;
			strncat(stop_reason, d.stop_reason ? d.stop_reason : "", sizeof stop_reason - 1);
			break;
		}
	}

	antterm(&p);
	httprespfree(r);
	httpclose(c);
	oauthtokenfree(tok);
	antreqfree(req);

	CHECKEQ(rc, ANT_DONE, "3.2: ANT_DONE");
	CHECKEQ(ntool, 1, "3.2: one tool delta");
	CHECKSTR(tool_name, "exec", "3.2: tool name exec");
	CHECK(ntoolarg > 0, "3.2: at least one toolarg delta");
	CHECKEQ(nstop, 1, "3.2: one stop delta");
	CHECKSTR(stop_reason, "tool_use", "3.2: stop_reason tool_use");
}

/* ── threadmain ─────────────────────────────────────────────────────── */

int mainstacksize = 65536;   /* antdelta uses ~4KB of toks[] per call; avoid overflow */

void
threadmain(int argc, char *argv[])
{
	char *tokpath  = nil;
	int   i;

	for(i = 1; i < argc; i++) {
		if(strcmp(argv[i], "-t") == 0 && i+1 < argc)
			tokpath = argv[++i];
	}

	print("=== Part 1: request builder unit tests ===\n");
	test_req_user_system();
	test_req_assistant_array();
	test_req_tooluse();
	test_req_toolresult();
	test_req_thinking_skipped();
	test_hdrs_initiator_user();
	test_hdrs_initiator_agent();

	print("=== Part 2: delta parser fixture tests ===\n");
	test_delta_text();
	test_delta_tool();

	print("=== Part 2.5: antreqctxtokens and antreqtrim ===\n");
	test_ant_ctxtokens_empty();
	test_ant_ctxtokens_counts_text();
	test_ant_ctxtokens_counts_tool_input();
	test_ant_trim_zero_noop();
	test_ant_trim_one_turn();
	test_ant_trim_with_tool_turn();
	test_ant_trim_all();
	test_ant_trim_msgtail_updated();

	print("=== Part 2.6: cache_control placement ===\n");
	test_cache_system_block_array();
	test_cache_no_system_omitted();
	test_cache_last_user_text();
	test_cache_with_system_two_markers();
	test_cache_tool_result_not_marked();
	test_cache_flag_cleared_after_call();

	if(tokpath != nil) {
		print("=== Part 3: live integration tests ===\n");
		test_live_text(tokpath);
		test_live_tool(tokpath);
	} else {
		print("(skip Part 3: no -t flag)\n");
	}

	if(failures == 0)
		print("\nAll tests passed.\n");
	else
		fprint(2, "\n%d test(s) FAILED.\n", failures);

	threadexitsall(failures ? "FAIL" : nil);
}
