/*
 * jsontest.c — unit tests for json.c helpers
 *
 * Tests against fixed JSON fixtures representative of the actual API
 * shapes used in later stages:
 *   - flat object field extraction (OAuth device code response)
 *   - nested field extraction (SSE delta)
 *   - array walk (models list, tool_calls)
 *   - string unescaping
 *   - integer parsing
 *   - JSON string emission with escaping
 *   - jsonnext correctness across object/array/scalar tokens
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>

#include "json.h"

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
	if((a) != (b)) { \
		fprint(2, "FAIL: %s: got %ld want %ld\n", \
		       msg, (long)(a), (long)(b)); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

#define CHECKSTR(a, b, msg) do { \
	if(strcmp((a),(b)) != 0) { \
		fprint(2, "FAIL: %s: got \"%s\" want \"%s\"\n", \
		       msg, (a), (b)); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

enum { MAXTOK = 1024 };

static int
parse(const char *js, jsmntok_t *toks, int maxtok)
{
	jsmn_parser p;
	int n;

	jsmn_init(&p);
	n = jsmn_parse(&p, js, strlen(js), toks, maxtok);
	if(n < 0)
		sysfatal("jsmn_parse failed: %d on: %.60s", n, js);
	return n;
}

/* ── OAuth device code response ──────────────────────────────────────── */
static void
test_oauth(void)
{
	static const char js[] =
		"{\"device_code\":\"DEV123\","
		"\"user_code\":\"ABCD-EFGH\","
		"\"verification_uri\":\"https://github.com/login/device\","
		"\"expires_in\":899,"
		"\"interval\":5}";

	jsmntok_t toks[MAXTOK];
	int ntoks, vi;
	char buf[256];

	ntoks = parse(js, toks, MAXTOK);
	CHECK(ntoks > 0, "oauth: parse succeeds");

	vi = jsonget(js, toks, ntoks, 0, "device_code");
	CHECK(vi >= 0, "oauth: device_code found");
	jsonstr(js, &toks[vi], buf, sizeof buf);
	CHECKSTR(buf, "DEV123", "oauth: device_code value");

	vi = jsonget(js, toks, ntoks, 0, "verification_uri");
	CHECK(vi >= 0, "oauth: verification_uri found");
	jsonstr(js, &toks[vi], buf, sizeof buf);
	CHECKSTR(buf, "https://github.com/login/device", "oauth: verification_uri value");

	vi = jsonget(js, toks, ntoks, 0, "expires_in");
	CHECK(vi >= 0, "oauth: expires_in found");
	CHECKEQ(jsonint(js, &toks[vi]), 899, "oauth: expires_in value");

	vi = jsonget(js, toks, ntoks, 0, "interval");
	CHECK(vi >= 0, "oauth: interval found");
	CHECKEQ(jsonint(js, &toks[vi]), 5, "oauth: interval value");

	/* missing key */
	vi = jsonget(js, toks, ntoks, 0, "nonexistent");
	CHECKEQ(vi, -1, "oauth: missing key returns -1");
}

/* ── /models array walk ──────────────────────────────────────────────── */
static void
test_models(void)
{
	static const char js[] =
		"{\"object\":\"list\","
		"\"data\":["
		  "{\"id\":\"gpt-4o\",\"name\":\"GPT-4o\","
		   "\"vendor\":\"Azure OpenAI\","
		   "\"supported_endpoints\":[\"/chat/completions\"]},"
		  "{\"id\":\"claude-sonnet-4\",\"name\":\"Claude Sonnet 4\","
		   "\"vendor\":\"Anthropic\","
		   "\"supported_endpoints\":[\"/chat/completions\",\"/v1/messages\"]}"
		"]}";

	jsmntok_t toks[MAXTOK];
	int ntoks, ai, elem, vi, ei;
	char buf[256];
	int i;

	ntoks = parse(js, toks, MAXTOK);
	CHECK(ntoks > 0, "models: parse succeeds");

	ai = jsonget(js, toks, ntoks, 0, "data");
	CHECK(ai >= 0, "models: data array found");
	CHECK(toks[ai].type == JSMN_ARRAY, "models: data is array");
	CHECKEQ(toks[ai].size, 2, "models: data has 2 elements");

	/* walk elements */
	elem = ai + 1;
	for(i = 0; i < toks[ai].size; i++) {
		CHECK(toks[elem].type == JSMN_OBJECT, "models: element is object");
		elem = jsonnext(toks, ntoks, elem);
	}
	CHECKEQ(elem, ntoks, "models: jsonnext exhausts array correctly");

	/* check first element fields */
	elem = ai + 1;
	vi = jsonget(js, toks, ntoks, elem, "id");
	CHECK(vi >= 0, "models[0]: id found");
	jsonstr(js, &toks[vi], buf, sizeof buf);
	CHECKSTR(buf, "gpt-4o", "models[0]: id value");

	vi = jsonget(js, toks, ntoks, elem, "vendor");
	CHECK(vi >= 0, "models[0]: vendor found");
	jsonstr(js, &toks[vi], buf, sizeof buf);
	CHECKSTR(buf, "Azure OpenAI", "models[0]: vendor value");

	/* walk supported_endpoints of first element */
	ei = jsonget(js, toks, ntoks, elem, "supported_endpoints");
	CHECK(ei >= 0, "models[0]: supported_endpoints found");
	CHECK(toks[ei].type == JSMN_ARRAY, "models[0]: supported_endpoints is array");
	CHECKEQ(toks[ei].size, 1, "models[0]: supported_endpoints has 1 entry");

	/* check second element */
	elem = jsonnext(toks, ntoks, ai + 1);
	vi = jsonget(js, toks, ntoks, elem, "id");
	CHECK(vi >= 0, "models[1]: id found");
	jsonstr(js, &toks[vi], buf, sizeof buf);
	CHECKSTR(buf, "claude-sonnet-4", "models[1]: id value");

	ei = jsonget(js, toks, ntoks, elem, "supported_endpoints");
	CHECKEQ(toks[ei].size, 2, "models[1]: supported_endpoints has 2 entries");
}

/* ── OpenAI SSE delta (text) ─────────────────────────────────────────── */
static void
test_oai_delta_text(void)
{
	static const char js[] =
		"{\"choices\":[{"
		  "\"delta\":{\"role\":\"assistant\",\"content\":\"Hello\"},"
		  "\"finish_reason\":null,"
		  "\"index\":0"
		"}]}";

	jsmntok_t toks[MAXTOK];
	int ntoks, ci, c0, di, vi;
	char buf[256];

	ntoks = parse(js, toks, MAXTOK);
	CHECK(ntoks > 0, "oai_text: parse succeeds");

	ci = jsonget(js, toks, ntoks, 0, "choices");
	CHECK(ci >= 0 && toks[ci].type == JSMN_ARRAY, "oai_text: choices array");

	c0 = ci + 1;  /* first choice */
	di = jsonget(js, toks, ntoks, c0, "delta");
	CHECK(di >= 0 && toks[di].type == JSMN_OBJECT, "oai_text: delta object");

	vi = jsonget(js, toks, ntoks, di, "content");
	CHECK(vi >= 0, "oai_text: content found");
	jsonstr(js, &toks[vi], buf, sizeof buf);
	CHECKSTR(buf, "Hello", "oai_text: content value");

	vi = jsonget(js, toks, ntoks, c0, "finish_reason");
	CHECK(vi >= 0, "oai_text: finish_reason found");
	/* finish_reason is null (primitive) */
	CHECK(toks[vi].type == JSMN_PRIMITIVE, "oai_text: finish_reason is primitive");
	CHECK(js[toks[vi].start] == 'n', "oai_text: finish_reason is null");
}

/* ── OpenAI SSE delta (tool_calls) ───────────────────────────────────── */
static void
test_oai_delta_toolcall(void)
{
	static const char js[] =
		"{\"choices\":[{"
		  "\"delta\":{"
		    "\"tool_calls\":[{"
		      "\"index\":0,"
		      "\"id\":\"call_abc123\","
		      "\"type\":\"function\","
		      "\"function\":{"
		        "\"name\":\"exec\","
		        "\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\""
		      "}"
		    "}]"
		  "},"
		  "\"finish_reason\":\"tool_calls\","
		  "\"index\":0"
		"}]}";

	jsmntok_t toks[MAXTOK];
	int ntoks, ci, c0, di, tci, tc0, fi, vi;
	char buf[512];

	ntoks = parse(js, toks, MAXTOK);
	CHECK(ntoks > 0, "tool_calls: parse succeeds");

	ci  = jsonget(js, toks, ntoks, 0, "choices");
	c0  = ci + 1;
	di  = jsonget(js, toks, ntoks, c0, "delta");
	tci = jsonget(js, toks, ntoks, di, "tool_calls");
	CHECK(tci >= 0 && toks[tci].type == JSMN_ARRAY, "tool_calls: array found");
	CHECKEQ(toks[tci].size, 1, "tool_calls: 1 element");

	tc0 = tci + 1;  /* first tool call */
	vi = jsonget(js, toks, ntoks, tc0, "id");
	CHECK(vi >= 0, "tool_calls[0]: id found");
	jsonstr(js, &toks[vi], buf, sizeof buf);
	CHECKSTR(buf, "call_abc123", "tool_calls[0]: id value");

	fi = jsonget(js, toks, ntoks, tc0, "function");
	CHECK(fi >= 0, "tool_calls[0]: function object found");

	vi = jsonget(js, toks, ntoks, fi, "name");
	CHECK(vi >= 0, "tool_calls[0]: function.name found");
	jsonstr(js, &toks[vi], buf, sizeof buf);
	CHECKSTR(buf, "exec", "tool_calls[0]: function.name value");

	vi = jsonget(js, toks, ntoks, fi, "arguments");
	CHECK(vi >= 0, "tool_calls[0]: function.arguments found");
	jsonstr(js, &toks[vi], buf, sizeof buf);
	CHECKSTR(buf, "{\"cmd\":\"ls\"}", "tool_calls[0]: arguments unescaped");

	/* finish_reason */
	vi = jsonget(js, toks, ntoks, c0, "finish_reason");
	jsonstr(js, &toks[vi], buf, sizeof buf);
	CHECKSTR(buf, "tool_calls", "tool_calls: finish_reason value");
}

/* ── String escaping round-trip ──────────────────────────────────────── */
static void
test_escape(void)
{
	/* Parse a JSON string with escapes, then emit it back */
	static const char js[] =
		"\"hello\\nworld\\t\\\"quoted\\\"\\u0041\"";
	jsmntok_t toks[MAXTOK];
	int ntoks;
	char buf[256];
	char out[512];
	Biobuf *b;

	ntoks = parse(js, toks, MAXTOK);
	CHECK(ntoks == 1, "escape: single string token");

	jsonstr(js, &toks[0], buf, sizeof buf);
	CHECKSTR(buf, "hello\nworld\t\"quoted\"A", "escape: unescape correct");

	/* emit back to a memory buffer via a pipe */
	{
		int p[2];
		long n;

		if(pipe(p) < 0) sysfatal("pipe: %r");
		b = mallocz(sizeof(Biobuf), 1);
		if(b == nil) sysfatal("mallocz Biobuf: %r");
		Binit(b, p[1], OWRITE);
		jsonemitstr(b, buf);
		Bflush(b);
		Bterm(b);
		free(b);
		close(p[1]);
		n = readn(p[0], out, sizeof out - 1);
		close(p[0]);
		if(n < 0) n = 0;
		out[n] = '\0';
	}
	CHECKSTR(out, "\"hello\\nworld\\t\\\"quoted\\\"A\"", "escape: re-emit correct");
}

/* ── Control character emission ──────────────────────────────────────── */
static void
test_ctrlchar(void)
{
	char src[4] = { 'a', 0x01, 'b', '\0' };
	char out[64];
	Biobuf *b;
	int p[2];
	long n;

	if(pipe(p) < 0) sysfatal("pipe: %r");
	b = mallocz(sizeof(Biobuf), 1);
	if(b == nil) sysfatal("mallocz Biobuf: %r");
	Binit(b, p[1], OWRITE);
	jsonemitstr(b, src);
	Bflush(b);
	Bterm(b);
	free(b);
	close(p[1]);
	n = readn(p[0], out, sizeof out - 1);
	close(p[0]);
	if(n < 0) n = 0;
	out[n] = '\0';
	CHECKSTR(out, "\"a\\u0001b\"", "ctrlchar: control byte escaped as \\uXXXX");
}

void
threadmain(int argc, char *argv[])
{
	USED(argc); USED(argv);

	print("=== json tests ===\n");
	test_oauth();
	test_models();
	test_oai_delta_text();
	test_oai_delta_toolcall();
	test_escape();
	test_ctrlchar();

	if(failures > 0) {
		fprint(2, "\n%d failure(s)\n", failures);
		threadexitsall("FAIL");
	}
	print("\nall tests passed\n");
	threadexitsall(nil);
}
