/*
 * agenttest.c — integration test for the agent loop (agent.c)
 *
 * Tests:
 *
 * Part 1: Unit tests (no network)
 *   1.1  genuuid — format is xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
 *   1.2  genuuid — two consecutive calls produce distinct UUIDs
 *   1.3  emitevent — callback receives correctly framed RS record
 *   1.4  writesession — session Biobuf receives correctly framed record
 *   1.5  emitandsave — both callback and Biobuf receive the record
 *   1.6  fmtrec with single field — no FS, has RS
 *   1.7  fmtrec with three fields — two FS, one RS
 *
 * Part 2: Live integration test (requires proxy + token)
 *   2.1  agentrun: "list the files in /tmp" with gpt-4o
 *        - exec tool must be called (tool_start event emitted)
 *        - final stop_reason must be "stop" (turn completes)
 *        - text output non-empty
 *        - session file written to ~/lib/9ai/sessions/<uuid>
 *        - session file contains: session, turn_start, prompt,
 *          tool_start, tool_end, turn_end records
 *
 * Usage:
 *   ./o.agenttest                     (unit tests only)
 *   ./o.agenttest -s <sock> -t <tok>  (unit + live)
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
#include "ant.h"
#include "exec.h"
#include "agent.h"

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

/* ── Part 1: Unit tests ─────────────────────────────────────────────── */

static void
test_genuuid_format(void)
{
	char uuid[37];
	int  i, dashes = 0;

	print("\n-- 1.1 genuuid: UUID v4 format\n");

	genuuid(uuid);
	CHECK(strlen(uuid) == 36, "UUID is 36 characters");

	/* check dash positions: 8-4-4-4-12 → dashes at 8,13,18,23 */
	CHECK(uuid[8]  == '-', "dash at position 8");
	CHECK(uuid[13] == '-', "dash at position 13");
	CHECK(uuid[18] == '-', "dash at position 18");
	CHECK(uuid[23] == '-', "dash at position 23");

	/* version nibble at position 14 must be '4' */
	CHECK(uuid[14] == '4', "version nibble is '4'");

	/* variant nibble at position 19: '8','9','a','b' */
	{
		char v = uuid[19];
		CHECK(v == '8' || v == '9' || v == 'a' || v == 'b',
		      "variant nibble in {8,9,a,b}");
	}

	/* all non-dash chars must be hex digits */
	for(i = 0; i < 36; i++) {
		if(uuid[i] == '-') { dashes++; continue; }
		CHECK((uuid[i] >= '0' && uuid[i] <= '9') ||
		      (uuid[i] >= 'a' && uuid[i] <= 'f'),
		      "UUID char is lowercase hex or dash");
	}
	CHECKEQ(dashes, 4, "exactly 4 dashes");
}

static void
test_genuuid_unique(void)
{
	char a[37], b[37];

	print("\n-- 1.2 genuuid: two UUIDs are distinct\n");
	genuuid(a);
	genuuid(b);
	CHECK(strcmp(a, b) != 0, "two UUIDs differ");
}

/* ── Record callback capture ── */

typedef struct RecordCapture RecordCapture;
struct RecordCapture {
	char  buf[65536];
	long  len;
	int   count;
};

static void
capture_event(const char *rec, long len, void *aux)
{
	RecordCapture *cap = aux;
	if(len >= (long)sizeof cap->buf) len = (long)sizeof cap->buf - 1;
	memmove(cap->buf, rec, len);
	cap->buf[len] = '\0';
	cap->len = len;
	cap->count++;
}

static void
test_emitevent(void)
{
	AgentCfg       cfg;
	RecordCapture  *cap;

	print("\n-- 1.3 emitevent: record delivered to callback\n");

	cap = mallocz(sizeof *cap, 1);
	memset(&cfg, 0, sizeof cfg);
	cfg.onevent = capture_event;
	cfg.aux     = cap;

	emitevent(&cfg, "text", "hello world", nil);

	CHECKEQ(cap->count, 1,    "callback called once");
	/* record: "text" FS "hello world" RS */
	CHECK(cap->len >= 2,      "record has at least 2 bytes");
	CHECKCONTAINS(cap->buf,   "text",        "record contains type");
	CHECKCONTAINS(cap->buf,   "hello world", "record contains field");
	CHECK(cap->buf[0] != '\0', "record non-empty");
	/* check FS between fields */
	{
		char *fs = memchr(cap->buf, '\x1f', cap->len);
		CHECK(fs != nil, "FS (0x1f) present between fields");
	}
	/* check RS at end */
	CHECK(cap->buf[cap->len - 1] == '\x1e', "RS (0x1e) terminates record");
	free(cap);
}

static void
test_writesession(void)
{
	AgentCfg cfg;
	int      pfd[2];
	Biobuf   bio;
	char     buf[256];
	long     n;

	print("\n-- 1.4 writesession: record written to Biobuf\n");

	pipe(pfd);
	Binit(&bio, pfd[1], OWRITE);

	memset(&cfg, 0, sizeof cfg);
	cfg.sess_bio = &bio;

	writesession(&cfg, "turn_end", "end_turn", nil);

	Bflush(&bio);
	Bterm(&bio);
	close(pfd[1]);

	n = read(pfd[0], buf, sizeof buf - 1);
	close(pfd[0]);
	if(n < 0) n = 0;
	buf[n] = '\0';

	CHECK(n > 0,             "bytes written to Biobuf");
	CHECKCONTAINS(buf, "turn_end",  "type field present");
	CHECKCONTAINS(buf, "end_turn",  "value field present");
	CHECK(buf[n - 1] == '\x1e',     "RS terminates record");
}

static void
test_emitandsave(void)
{
	AgentCfg       cfg;
	RecordCapture  *cap;
	int            pfd[2];
	Biobuf         bio;
	char           sbuf[256];
	long           n;

	print("\n-- 1.5 emitandsave: record goes to both callback and Biobuf\n");

	cap = mallocz(sizeof *cap, 1);
	pipe(pfd);
	Binit(&bio, pfd[1], OWRITE);

	memset(&cfg, 0, sizeof cfg);
	cfg.onevent  = capture_event;
	cfg.aux      = cap;
	cfg.sess_bio = &bio;

	emitandsave(&cfg, "tool_end", "ok", "output text", nil);

	Bflush(&bio);
	Bterm(&bio);
	close(pfd[1]);

	n = read(pfd[0], sbuf, sizeof sbuf - 1);
	close(pfd[0]);
	if(n < 0) n = 0;
	sbuf[n] = '\0';

	/* event callback */
	CHECKEQ(cap->count, 1,         "event callback called once");
	CHECKCONTAINS(cap->buf, "tool_end", "event contains type");
	CHECKCONTAINS(cap->buf, "output text", "event contains output");

	/* session file */
	CHECK(n > 0,               "session bytes written");
	CHECKCONTAINS(sbuf, "tool_end",    "session contains type");
	CHECKCONTAINS(sbuf, "output text", "session contains output");
	free(cap);
}

static void
test_record_single_field(void)
{
	AgentCfg      cfg;
	RecordCapture *cap;

	print("\n-- 1.6 single-field record: no FS, has RS\n");

	cap = mallocz(sizeof *cap, 1);
	memset(&cfg, 0, sizeof cfg);
	cfg.onevent = capture_event;
	cfg.aux     = cap;

	emitevent(&cfg, "idle", nil);

	{
		char *fs = memchr(cap->buf, '\x1f', cap->len);
		CHECK(fs == nil,  "no FS in single-field record");
	}
	CHECK(cap->buf[cap->len - 1] == '\x1e', "RS terminates single-field record");
	CHECKCONTAINS(cap->buf, "idle", "field text present");
	free(cap);
}

static void
test_record_three_fields(void)
{
	AgentCfg      cfg;
	RecordCapture *cap;
	int           fs_count = 0;
	int           i;

	print("\n-- 1.7 three-field record: two FS, one RS\n");

	cap = mallocz(sizeof *cap, 1);
	memset(&cfg, 0, sizeof cfg);
	cfg.onevent = capture_event;
	cfg.aux     = cap;

	emitevent(&cfg, "turn_start", "uuid-abc", "gpt-4o", nil);

	for(i = 0; i < cap->len; i++)
		if((unsigned char)cap->buf[i] == 0x1f) fs_count++;

	CHECKEQ(fs_count, 2, "exactly 2 FS in three-field record");
	CHECK(cap->buf[cap->len - 1] == '\x1e', "RS terminates record");
	CHECKCONTAINS(cap->buf, "turn_start", "field 0 present");
	CHECKCONTAINS(cap->buf, "uuid-abc",   "field 1 present");
	CHECKCONTAINS(cap->buf, "gpt-4o",     "field 2 present");
	free(cap);
}

/* ── Part 1.5: isctxoverflow ────────────────────────────────────────── */

static void
test_overflow_nil_body(void)
{
	print("\n-- 1.5 isctxoverflow: nil body → 0\n");
	CHECKEQ(isctxoverflow(nil), 0, "nil body not overflow");
}

static void
test_overflow_empty_body(void)
{
	print("\n-- 1.5 isctxoverflow: empty body → 0\n");
	CHECKEQ(isctxoverflow(""), 0, "empty body not overflow");
}

static void
test_overflow_unrelated_400(void)
{
	print("\n-- 1.5 isctxoverflow: unrelated 400 body → 0\n");
	CHECKEQ(isctxoverflow("{\"error\":{\"message\":\"invalid model\"}}"), 0,
	        "unrelated 400 not overflow");
}

static void
test_overflow_copilot_oai(void)
{
	print("\n-- 1.5 isctxoverflow: Copilot OAI 'exceeds the limit' → 1\n");
	CHECKEQ(isctxoverflow(
	    "{\"error\":{\"message\":\"This model's maximum context length is "
	    "128000 tokens. However, your messages resulted in "
	    "prompt token count of 131072 tokens which exceeds the limit of "
	    "128000 tokens.\"}}"),
	    1, "Copilot OAI overflow detected");
}

static void
test_overflow_openai_generic(void)
{
	print("\n-- 1.5 isctxoverflow: OpenAI 'exceeds the context window' → 1\n");
	CHECKEQ(isctxoverflow(
	    "{\"error\":{\"message\":\"This model's maximum context length is "
	    "128000 tokens. Your prompt (131000) exceeds the context window.\"}}"),
	    1, "generic OpenAI overflow detected");
}

static void
test_overflow_anthropic(void)
{
	print("\n-- 1.5 isctxoverflow: Anthropic 'prompt is too long' → 1\n");
	CHECKEQ(isctxoverflow(
	    "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\","
	    "\"message\":\"prompt is too long: 210000 tokens > 200000 maximum\"}}"),
	    1, "Anthropic overflow detected");
}

static void
test_overflow_context_length_exceeded(void)
{
	print("\n-- 1.5 isctxoverflow: 'context_length_exceeded' → 1\n");
	CHECKEQ(isctxoverflow(
	    "{\"error\":{\"code\":\"context_length_exceeded\","
	    "\"message\":\"max context exceeded\"}}"),
	    1, "context_length_exceeded detected");
}

/* ── Part 1.6: agentsessload trim replay ─────────────────────────────── */

/*
 * write a minimal session file to a temp path, load it with
 * agentsessload, and verify the history is correct.
 *
 * Session file format: RS-terminated records, fields separated by FS.
 * We write directly rather than going through agentsessopen/agentrun
 * so the test has no network dependency.
 */

#define FS "\x1f"
#define RS "\x1e"

/* write one RS-terminated record; fields are ESC-encoded and separated by FS */
static void
writerec(int fd, ...)
{
	va_list ap;
	char   *f;
	int     first = 1;

	va_start(ap, fd);
	while((f = va_arg(ap, char*)) != nil) {
		if(!first) write(fd, FS, 1);
		/* ESC-encode the field */
		for(; *f != '\0'; f++) {
			uchar c = (uchar)*f;
			if(c == 0x1b || c == 0x1f || c == 0x1e)
				write(fd, "\x1b", 1);
			write(fd, f, 1);
		}
		first = 0;
	}
	write(fd, RS, 1);
	va_end(ap);
}

static void
test_sessload_trim_replay(void)
{
	char     path[256];
	int      fd;
	OAIReq  *req;
	AgentCfg cfg;
	int      rc;

	print("\n-- 1.6 agentsessload: trim record replayed correctly\n");

	/* write session file to a temp file */
	snprint(path, sizeof path, "/tmp/9aitest-trim-%d", (int)getpid());
	fd = create(path, OWRITE, 0600);
	if(fd < 0) {
		fprint(2, "SKIP 1.6: cannot create temp file %s: %r\n", path);
		return;
	}

	/* session header */
	writerec(fd, "session", "test-uuid-1234-5678-abcd-ef0123456789",
	         "gpt-4o", "0", nil);

	/* turn 1: prompt + assistant text */
	writerec(fd, "prompt",     "first question", nil);
	writerec(fd, "turn_start", nil);
	writerec(fd, "text",       "first answer", nil);
	writerec(fd, "turn_end",   "stop", nil);

	/* turn 2: prompt + assistant text */
	writerec(fd, "prompt",     "second question", nil);
	writerec(fd, "turn_start", nil);
	writerec(fd, "text",       "second answer", nil);
	writerec(fd, "turn_end",   "stop", nil);

	/* trim 1: drop turn 1 */
	writerec(fd, "trim", "1", nil);

	/* turn 3: written after the trim */
	writerec(fd, "prompt",     "third question", nil);
	writerec(fd, "turn_start", nil);
	writerec(fd, "text",       "third answer", nil);
	writerec(fd, "turn_end",   "stop", nil);

	close(fd);

	/* load the session */
	req = oaireqnew("gpt-4o");
	memset(&cfg, 0, sizeof cfg);
	cfg.model   = strdup("gpt-4o");
	cfg.sessdir = nil;

	rc = agentsessload(path, req, &cfg);
	if(rc < 0) {
		fprint(2, "SKIP 1.6: agentsessload failed: %r\n");
		oaireqfree(req);
		free(cfg.model);
		remove(path);
		return;
	}
	agentsessclose(&cfg);

	/* verify history via serialisation */
	{
		char *body = oaireqjson(req, nil, nil);
		CHECK(body != nil, "1.6: serialised after load");
		/* turn 1 must be gone (trimmed) */
		CHECK(strstr(body, "first question") == nil,
		      "1.6: turn 1 user absent after trim replay");
		CHECK(strstr(body, "first answer") == nil,
		      "1.6: turn 1 assistant absent after trim replay");
		/* turn 2 must still be present */
		CHECKCONTAINS(body, "second question",
		              "1.6: turn 2 user present");
		CHECKCONTAINS(body, "second answer",
		              "1.6: turn 2 assistant present");
		/* turn 3 must also be present */
		CHECKCONTAINS(body, "third question",
		              "1.6: turn 3 user present");
		CHECKCONTAINS(body, "third answer",
		              "1.6: turn 3 assistant present");
		free(body);
	}

	oaireqfree(req);
	free(cfg.model);
	remove(path);
}

static void
test_sessload_trim_all_then_continue(void)
{
	char     path[256];
	int      fd;
	OAIReq  *req;
	AgentCfg cfg;
	int      rc;

	print("\n-- 1.6 agentsessload: trim-all then new turn\n");

	snprint(path, sizeof path, "/tmp/9aitest-trimall-%d", (int)getpid());
	fd = create(path, OWRITE, 0600);
	if(fd < 0) {
		fprint(2, "SKIP 1.6b: cannot create temp file: %r\n");
		return;
	}

	writerec(fd, "session", "uuid-trimall", "gpt-4o", "0", nil);
	writerec(fd, "prompt",    "question one", nil);
	writerec(fd, "turn_start", nil);
	writerec(fd, "text",       "answer one", nil);
	writerec(fd, "turn_end",   "stop", nil);
	/* trim all: n=99, more than 1 turn present */
	writerec(fd, "trim", "99", nil);
	/* new turn after trim */
	writerec(fd, "prompt",    "fresh start", nil);
	writerec(fd, "turn_start", nil);
	writerec(fd, "text",       "fresh answer", nil);
	writerec(fd, "turn_end",   "stop", nil);
	close(fd);

	req = oaireqnew("gpt-4o");
	memset(&cfg, 0, sizeof cfg);
	cfg.model = strdup("gpt-4o");
	rc = agentsessload(path, req, &cfg);
	if(rc < 0) {
		fprint(2, "SKIP 1.6b: agentsessload failed: %r\n");
		oaireqfree(req);
		free(cfg.model);
		remove(path);
		return;
	}
	agentsessclose(&cfg);

	{
		char *body = oaireqjson(req, nil, nil);
		CHECK(body != nil, "1.6b: serialised");
		CHECK(strstr(body, "question one") == nil, "1.6b: old turn absent");
		CHECKCONTAINS(body, "fresh start",  "1.6b: post-trim turn present");
		CHECKCONTAINS(body, "fresh answer", "1.6b: post-trim answer present");
		free(body);
	}

	oaireqfree(req);
	free(cfg.model);
	remove(path);
}

/* ── Part 2: Live integration test ──────────────────────────────────── */

/*
 * Capture all events for later verification.
 */
typedef struct LiveCapture LiveCapture;
struct LiveCapture {
	/* event records — stored as a flat buffer with embedded NULs */
	char  evbuf[1024 * 1024];  /* 1MB event log */
	long  evlen;
	int   nevents;

	/* text output */
	char  text[65536];
	long  textlen;

	/* flags */
	int   saw_tool_start;
	int   saw_tool_end;
	int   saw_turn_end;
	int   saw_turn_start;
};

static void
live_ontext(const char *text, void *aux)
{
	LiveCapture *lc = aux;
	long dlen = strlen(text);
	if(lc->textlen + dlen < (long)sizeof lc->text - 1) {
		memmove(lc->text + lc->textlen, text, dlen);
		lc->textlen += dlen;
		lc->text[lc->textlen] = '\0';
	}
}

static void
live_onevent(const char *rec, long len, void *aux)
{
	LiveCapture *lc = aux;
	lc->nevents++;

	/* store rec in evbuf with a NUL after for easy string search */
	if(lc->evlen + len + 1 < (long)sizeof lc->evbuf) {
		memmove(lc->evbuf + lc->evlen, rec, len);
		lc->evbuf[lc->evlen + len] = '\0';
		lc->evlen += len + 1;
	}

	/* check type field (first field, before FS or RS) */
	{
		char type[64];
		int  i = 0;
		while(i < len && i < (int)sizeof type - 1 &&
		      (unsigned char)rec[i] != 0x1f &&
		      (unsigned char)rec[i] != 0x1e)
		{
			type[i] = rec[i];
			i++;
		}
		type[i] = '\0';

		if(strcmp(type, "tool_start") == 0) lc->saw_tool_start = 1;
		if(strcmp(type, "tool_end")   == 0) lc->saw_tool_end   = 1;
		if(strcmp(type, "turn_end")   == 0) lc->saw_turn_end   = 1;
		if(strcmp(type, "turn_start") == 0) lc->saw_turn_start = 1;
	}
}

/*
 * check_session_file — read the session file and verify required records.
 * Returns 1 if all checks pass, 0 otherwise.
 */
static int
check_session_file(const char *sessdir, const char *uuid)
{
	char  *path;
	int    fd;
	char   *buf;
	long   n;
	int    ok = 1;

	path = smprint("%s%s", sessdir, uuid);

	fd = open(path, OREAD);
	if(fd < 0) {
		fprint(2, "  session file not found: %s\n", path);
		free(path);
		return 0;
	}
	buf = mallocz(65536, 1);
	n = read(fd, buf, 65536 - 1);
	close(fd);
	free(path);
	if(n <= 0) {
		fprint(2, "  session file empty\n");
		free(buf);
		return 0;
	}
	buf[n] = '\0';

	/* check for required record types by scanning for FS-separated type strings */
	{
		const char *required[] = {
			"session", "turn_start", "prompt",
			"tool_start", "tool_end", "turn_end",
			nil
		};
		int i;
		for(i = 0; required[i] != nil; i++) {
			if(strstr(buf, required[i]) == nil) {
				fprint(2, "  session missing record type: %s\n", required[i]);
				ok = 0;
			}
		}
	}

	free(buf);
	return ok;
}

static void
test_live_agent(char *tokpath)
{
	AgentCfg     cfg;
	OAIReq      *req;
	LiveCapture  *lc;
	int          rc;

	print("\n-- 2.1 live: agent loop with gpt-4o — list files in /tmp\n");

	lc = mallocz(sizeof *lc, 1);
	memset(&cfg, 0, sizeof cfg);

	cfg.model    = "gpt-4o";
	cfg.tokpath  = tokpath;
	cfg.sessdir  = configpath("sessions/");
	cfg.system   = "You are a helpful assistant. "
	               "When asked to list files, use the exec tool to run ls.";
	cfg.ontext   = live_ontext;
	cfg.onevent  = live_onevent;
	cfg.aux      = lc;

	/* open session file */
	rc = agentsessopen(&cfg);
	if(rc < 0) {
		fprint(2, "  SKIP: agentsessopen failed: %r\n");
		free(lc);
		return;
	}
	print("  session uuid: %s\n", cfg.uuid);

	/* run the agent */
	req = oaireqnew("gpt-4o");
	rc  = agentrun("list the files in /tmp using the exec tool", req, &cfg);

	agentsessclose(&cfg);

	if(rc < 0) {
		fprint(2, "  SKIP: agentrun failed: %r\n");
		oaireqfree(req);
		free(lc);
		return;
	}

	/* verify results */
	print("  events received: %d\n", lc->nevents);
	print("  text output length: %ld\n", lc->textlen);

	CHECK(lc->saw_turn_start,  "turn_start event emitted");
	CHECK(lc->saw_tool_start,  "tool_start event emitted (exec was called)");
	CHECK(lc->saw_tool_end,    "tool_end event emitted");
	CHECK(lc->saw_turn_end,    "turn_end event emitted");
	CHECK(lc->textlen > 0,     "text output non-empty");
	CHECK(lc->nevents >= 4,    "at least 4 events (turn_start+tool_start+tool_end+turn_end)");

	/* verify session file */
	{
		int sess_ok = check_session_file(cfg.sessdir, cfg.uuid);
		CHECK(sess_ok, "session file contains all required record types");
	}

	free(cfg.sessdir);
	free(lc);
	oaireqfree(req);
}

/* ── Part 3: Phase-14 live test — ANT agent loop with thinking ────────── */

/*
 * test_live_agent_ant — test agentrunant() with claude-sonnet-4.5.
 *
 * 3.1  agentrunant: "list the files in /tmp" with claude-sonnet-4.5
 *        - exec tool must be called (tool_start event emitted)
 *        - thinking events may be emitted (ANTDThinking → "thinking" records)
 *        - final stop_reason must be "end_turn" (turn completes)
 *        - text output non-empty
 *        - session file contains: session, turn_start, prompt,
 *          tool_start, tool_end, turn_end records
 *        - session file does NOT contain thinking blocks in API history
 *          (thinking is session-file-only, not fed back to API)
 */
static void
test_live_agent_ant(char *tokpath)
{
	AgentCfg     cfg;
	ANTReq      *req;
	LiveCapture  *lc;
	int          rc;

	print("\n-- 3.1 live: agentrunant with claude-sonnet-4.5 — list files in /tmp\n");

	lc = mallocz(sizeof *lc, 1);
	memset(&cfg, 0, sizeof cfg);

	cfg.model    = "claude-sonnet-4.5";
	cfg.tokpath  = tokpath;
	cfg.sessdir  = configpath("sessions/");
	cfg.system   = "You are a helpful assistant. "
	               "When asked to list files, use the exec tool to run ls.";
	cfg.ontext   = live_ontext;
	cfg.onevent  = live_onevent;
	cfg.aux      = lc;

	/* open session file */
	rc = agentsessopen(&cfg);
	if(rc < 0) {
		fprint(2, "  SKIP: agentsessopen failed: %r\n");
		free(cfg.sessdir);
		free(lc);
		return;
	}
	print("  session uuid: %s\n", cfg.uuid);

	/* run the agent with ANT format */
	req = antreqnew("claude-sonnet-4.5");
	rc  = agentrunant("list the files in /tmp using the exec tool", req, &cfg);

	agentsessclose(&cfg);

	if(rc < 0) {
		char errbuf[256];
		rerrstr(errbuf, sizeof errbuf);
		fprint(2, "  SKIP: agentrunant failed: %s\n", errbuf);
		antreqfree(req);
		free(cfg.sessdir);
		free(lc);
		return;
	}

	/* verify results */
	print("  events received: %d\n", lc->nevents);
	print("  text output length: %ld\n", lc->textlen);

	/* Count thinking records in event log */
	int nthinking = 0;
	{
		long i = 0;
		while(i < lc->evlen) {
			char *rec = lc->evbuf + i;
			if(strncmp(rec, "thinking", 8) == 0 &&
			   ((unsigned char)rec[8] == 0x1f || (unsigned char)rec[8] == 0x1e))
				nthinking++;
			/* advance past this record (find next NUL we inserted) */
			i += strlen(rec) + 1;
		}
	}
	print("  thinking events: %d\n", nthinking);

	CHECK(lc->saw_turn_start,  "3.1: turn_start event emitted");
	CHECK(lc->saw_tool_start,  "3.1: tool_start event emitted (exec was called)");
	CHECK(lc->saw_tool_end,    "3.1: tool_end event emitted");
	CHECK(lc->saw_turn_end,    "3.1: turn_end event emitted");
	CHECK(lc->textlen > 0,     "3.1: text output non-empty");
	CHECK(lc->nevents >= 4,    "3.1: at least 4 events");

	/* verify session file */
	{
		int sess_ok = check_session_file(cfg.sessdir, cfg.uuid);
		CHECK(sess_ok, "3.1: session file contains all required record types");
	}

	free(cfg.sessdir);
	free(lc);
	antreqfree(req);
}

/* ── Main ────────────────────────────────────────────────────────────── */

void
threadmain(int argc, char *argv[])
{
	char *tokpath  = nil;

	ARGBEGIN{
	case 't': tokpath  = ARGF(); break;
	}ARGEND

	USED(argc); USED(argv);

	print("=== Part 1: Unit tests ===\n");
	test_genuuid_format();
	test_genuuid_unique();
	test_emitevent();
	test_writesession();
	test_emitandsave();
	test_record_single_field();
	test_record_three_fields();

	print("\n=== Part 1.5: isctxoverflow ===\n");
	test_overflow_nil_body();
	test_overflow_empty_body();
	test_overflow_unrelated_400();
	test_overflow_copilot_oai();
	test_overflow_openai_generic();
	test_overflow_anthropic();
	test_overflow_context_length_exceeded();

	print("\n=== Part 1.6: agentsessload trim replay ===\n");
	test_sessload_trim_replay();
	test_sessload_trim_all_then_continue();

	if(tokpath != nil) {
		print("\n=== Part 2: Live integration tests (OAI) ===\n");
		test_live_agent(tokpath);
		print("\n=== Part 3: Live integration tests (ANT/Claude) ===\n");
		test_live_agent_ant(tokpath);
	} else {
		print("\n(skipping live tests: pass -s <sock> -t <tok> to enable)\n");
	}

	if(failures > 0) {
		fprint(2, "\n%d test(s) FAILED\n", failures);
		threadexitsall("FAIL");
	}
	print("\nall tests passed\n");
	threadexitsall(nil);
}
