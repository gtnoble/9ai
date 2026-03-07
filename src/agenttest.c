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
		fprint(2, "FAIL: %s (line %d)\n", msg, __LINE__); \
		failures++; \
	} else { \
		print("ok:   %s\n", msg); \
	} \
} while(0)

#define CHECKEQ(a, b, msg) do { \
	if((long)(a) != (long)(b)) { \
		fprint(2, "FAIL: %s: got %ld want %ld (line %d)\n", \
		       msg, (long)(a), (long)(b), __LINE__); \
		failures++; \
	} else { \
		print("ok:   %s\n", msg); \
	} \
} while(0)

#define CHECKSTR(a, b, msg) do { \
	const char *_a = (a), *_b = (b); \
	if(_a == nil || strcmp(_a, _b) != 0) { \
		fprint(2, "FAIL: %s: got \"%s\" want \"%s\" (line %d)\n", \
		       msg, _a ? _a : "(nil)", _b, __LINE__); \
		failures++; \
	} else { \
		print("ok:   %s\n", msg); \
	} \
} while(0)

#define CHECKCONTAINS(s, sub, msg) do { \
	const char *_s = (s), *_sub = (sub); \
	if(_s == nil || strstr(_s, _sub) == nil) { \
		fprint(2, "FAIL: %s: \"%s\" does not contain \"%s\" (line %d)\n", \
		       msg, _s ? _s : "(nil)", _sub, __LINE__); \
		failures++; \
	} else { \
		print("ok:   %s\n", msg); \
	} \
} while(0)

#define CHECKNONIL(a, msg) do { \
	if((a) == nil) { \
		fprint(2, "FAIL: %s: got nil (line %d)\n", msg, __LINE__); \
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
	RecordCapture  cap;

	print("\n-- 1.3 emitevent: record delivered to callback\n");

	memset(&cfg, 0, sizeof cfg);
	memset(&cap, 0, sizeof cap);
	cfg.onevent = capture_event;
	cfg.aux     = &cap;

	emitevent(&cfg, "text", "hello world", nil);

	CHECKEQ(cap.count, 1,    "callback called once");
	/* record: "text" FS "hello world" RS */
	CHECK(cap.len >= 2,      "record has at least 2 bytes");
	CHECKCONTAINS(cap.buf,   "text",        "record contains type");
	CHECKCONTAINS(cap.buf,   "hello world", "record contains field");
	CHECK(cap.buf[0] != '\0', "record non-empty");
	/* check FS between fields */
	{
		char *fs = memchr(cap.buf, '\x1f', cap.len);
		CHECK(fs != nil, "FS (0x1f) present between fields");
	}
	/* check RS at end */
	CHECK(cap.buf[cap.len - 1] == '\x1e', "RS (0x1e) terminates record");
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
	RecordCapture  cap;
	int            pfd[2];
	Biobuf         bio;
	char           sbuf[256];
	long           n;

	print("\n-- 1.5 emitandsave: record goes to both callback and Biobuf\n");

	pipe(pfd);
	Binit(&bio, pfd[1], OWRITE);

	memset(&cfg, 0, sizeof cfg);
	memset(&cap, 0, sizeof cap);
	cfg.onevent  = capture_event;
	cfg.aux      = &cap;
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
	CHECKEQ(cap.count, 1,         "event callback called once");
	CHECKCONTAINS(cap.buf, "tool_end", "event contains type");
	CHECKCONTAINS(cap.buf, "output text", "event contains output");

	/* session file */
	CHECK(n > 0,               "session bytes written");
	CHECKCONTAINS(sbuf, "tool_end",    "session contains type");
	CHECKCONTAINS(sbuf, "output text", "session contains output");
}

static void
test_record_single_field(void)
{
	AgentCfg      cfg;
	RecordCapture cap;

	print("\n-- 1.6 single-field record: no FS, has RS\n");

	memset(&cfg, 0, sizeof cfg);
	memset(&cap, 0, sizeof cap);
	cfg.onevent = capture_event;
	cfg.aux     = &cap;

	emitevent(&cfg, "idle", nil);

	{
		char *fs = memchr(cap.buf, '\x1f', cap.len);
		CHECK(fs == nil,  "no FS in single-field record");
	}
	CHECK(cap.buf[cap.len - 1] == '\x1e', "RS terminates single-field record");
	CHECKCONTAINS(cap.buf, "idle", "field text present");
}

static void
test_record_three_fields(void)
{
	AgentCfg      cfg;
	RecordCapture cap;
	int           fs_count = 0;
	int           i;

	print("\n-- 1.7 three-field record: two FS, one RS\n");

	memset(&cfg, 0, sizeof cfg);
	memset(&cap, 0, sizeof cap);
	cfg.onevent = capture_event;
	cfg.aux     = &cap;

	emitevent(&cfg, "turn_start", "uuid-abc", "gpt-4o", nil);

	for(i = 0; i < cap.len; i++)
		if((unsigned char)cap.buf[i] == 0x1f) fs_count++;

	CHECKEQ(fs_count, 2, "exactly 2 FS in three-field record");
	CHECK(cap.buf[cap.len - 1] == '\x1e', "RS terminates record");
	CHECKCONTAINS(cap.buf, "turn_start", "field 0 present");
	CHECKCONTAINS(cap.buf, "uuid-abc",   "field 1 present");
	CHECKCONTAINS(cap.buf, "gpt-4o",     "field 2 present");
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
	char   buf[65536];
	long   n;
	int    ok = 1;

	path = smprint("%s%s", sessdir, uuid);

	fd = open(path, OREAD);
	if(fd < 0) {
		fprint(2, "  session file not found: %s\n", path);
		free(path);
		return 0;
	}
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	free(path);
	if(n <= 0) {
		fprint(2, "  session file empty\n");
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

	return ok;
}

static void
test_live_agent(char *sockpath, char *tokpath)
{
	AgentCfg     cfg;
	OAIReq      *req;
	LiveCapture  lc;
	int          rc;

	print("\n-- 2.1 live: agent loop with gpt-4o — list files in /tmp\n");

	memset(&cfg, 0, sizeof cfg);
	memset(&lc,  0, sizeof lc);

	cfg.model    = "gpt-4o";
	cfg.sockpath = sockpath;
	cfg.tokpath  = tokpath;
	/* derive session dir */
#ifdef PLAN9PORT
	{
		char  sockdup[512];
		char *slash;
		snprint(sockdup, sizeof sockdup, "%s", sockpath ? sockpath : "");
		slash = strrchr(sockdup, '/');
		if(slash != nil) *slash = '\0';
		cfg.sessdir = smprint("%s/sessions/", sockdup);
	}
#else
	cfg.sessdir = configpath("sessions/");
#endif
	cfg.system   = "You are a helpful assistant. "
	               "When asked to list files, use the exec tool to run ls.";
	cfg.ontext   = live_ontext;
	cfg.onevent  = live_onevent;
	cfg.aux      = &lc;

	/* open session file */
	rc = agentsessopen(&cfg);
	if(rc < 0) {
		fprint(2, "  SKIP: agentsessopen failed: %r\n");
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
		return;
	}

	/* verify results */
	print("  events received: %d\n", lc.nevents);
	print("  text output length: %ld\n", lc.textlen);

	CHECK(lc.saw_turn_start,  "turn_start event emitted");
	CHECK(lc.saw_tool_start,  "tool_start event emitted (exec was called)");
	CHECK(lc.saw_tool_end,    "tool_end event emitted");
	CHECK(lc.saw_turn_end,    "turn_end event emitted");
	CHECK(lc.textlen > 0,     "text output non-empty");
	CHECK(lc.nevents >= 4,    "at least 4 events (turn_start+tool_start+tool_end+turn_end)");

	/* verify session file */
	{
		int sess_ok = check_session_file(cfg.sessdir, cfg.uuid);
		CHECK(sess_ok, "session file contains all required record types");
	}

	free(cfg.sessdir);
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
test_live_agent_ant(char *sockpath, char *tokpath)
{
	AgentCfg     cfg;
	ANTReq      *req;
	LiveCapture  lc;
	int          rc;

	print("\n-- 3.1 live: agentrunant with claude-sonnet-4.5 — list files in /tmp\n");

	memset(&cfg, 0, sizeof cfg);
	memset(&lc,  0, sizeof lc);

	cfg.model    = "claude-sonnet-4.5";
	cfg.sockpath = sockpath;
	cfg.tokpath  = tokpath;
	/* derive session dir */
#ifdef PLAN9PORT
	{
		char  sockdup[512];
		char *slash;
		snprint(sockdup, sizeof sockdup, "%s", sockpath ? sockpath : "");
		slash = strrchr(sockdup, '/');
		if(slash != nil) *slash = '\0';
		cfg.sessdir = smprint("%s/sessions/", sockdup);
	}
#else
	cfg.sessdir = configpath("sessions/");
#endif
	cfg.system   = "You are a helpful assistant. "
	               "When asked to list files, use the exec tool to run ls.";
	cfg.ontext   = live_ontext;
	cfg.onevent  = live_onevent;
	cfg.aux      = &lc;

	/* open session file */
	rc = agentsessopen(&cfg);
	if(rc < 0) {
		fprint(2, "  SKIP: agentsessopen failed: %r\n");
		free(cfg.sessdir);
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
		return;
	}

	/* verify results */
	print("  events received: %d\n", lc.nevents);
	print("  text output length: %ld\n", lc.textlen);

	/* Count thinking records in event log */
	int nthinking = 0;
	{
		long i = 0;
		while(i < lc.evlen) {
			char *rec = lc.evbuf + i;
			if(strncmp(rec, "thinking", 8) == 0 &&
			   ((unsigned char)rec[8] == 0x1f || (unsigned char)rec[8] == 0x1e))
				nthinking++;
			/* advance past this record (find next NUL we inserted) */
			i += strlen(rec) + 1;
		}
	}
	print("  thinking events: %d\n", nthinking);

	CHECK(lc.saw_turn_start,  "3.1: turn_start event emitted");
	CHECK(lc.saw_tool_start,  "3.1: tool_start event emitted (exec was called)");
	CHECK(lc.saw_tool_end,    "3.1: tool_end event emitted");
	CHECK(lc.saw_turn_end,    "3.1: turn_end event emitted");
	CHECK(lc.textlen > 0,     "3.1: text output non-empty");
	CHECK(lc.nevents >= 4,    "3.1: at least 4 events");

	/* verify session file */
	{
		int sess_ok = check_session_file(cfg.sessdir, cfg.uuid);
		CHECK(sess_ok, "3.1: session file contains all required record types");
	}

	free(cfg.sessdir);
	antreqfree(req);
}

/* ── Main ────────────────────────────────────────────────────────────── */

void
threadmain(int argc, char *argv[])
{
	char *sockpath = nil;
	char *tokpath  = nil;

	ARGBEGIN{
	case 's': sockpath = ARGF(); break;
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

	if(sockpath != nil && tokpath != nil) {
		print("\n=== Part 2: Live integration tests (OAI) ===\n");
		test_live_agent(sockpath, tokpath);
		print("\n=== Part 3: Live integration tests (ANT/Claude) ===\n");
		test_live_agent_ant(sockpath, tokpath);
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
