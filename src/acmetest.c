/*
 * acmetest.c — unit tests for 9ai-acme pure-string helpers.
 *
 * Covers:
 *   - prompttext_body  (prompt.c)
 *   - splitrec         (record.c)
 *   - render_tool_start, render_tool_end, render_thinking  (render.c)
 *   - parsesessfile    (sessfile.c)
 *   - getevent         (acmeevent.c)
 *   - isuuid, ismodelid (acmeevent.c)
 *
 * Build:  mk test-acme
 * Run:    ./o.acmetest
 */

#include <u.h>
#include <libc.h>
#include <thread.h>
#include "prompt.h"
#include "record.h"
#include "render.h"
#include "sessfile.h"
#include "acmeevent.h"

/* ── test harness ── */

static int passed = 0;
static int failed = 0;
static int section_fails = 0;

static void
section(char *name)
{
	section_fails = failed;
	print("\n── %s ──\n", name);
}

static void
check(char *name, int ok)
{
	if(ok) {
		print("PASS  %s\n", name);
		passed++;
	} else {
		print("FAIL  %s\n", name);
		failed++;
	}
}

static void
checkstr(char *name, char *got, char *want)
{
	int ok;
	if(want == nil)
		ok = (got == nil);
	else
		ok = (got != nil && strcmp(got, want) == 0);

	if(ok) {
		print("PASS  %s\n", name);
		passed++;
	} else {
		print("FAIL  %s\n", name);
		if(want == nil)
			print("      want: (nil)\n");
		else
			print("      want: %q\n", want);
		if(got == nil)
			print("      got:  (nil)\n");
		else
			print("      got:  %q\n", got);
		failed++;
	}
}

/* ── UTF-8 constants ── */
#define SEP    "\n\n════════════════════════════════════════════════════════════\n\n"
#define ECHO   "\xe2\x96\xb6"   /* ▶ */
#define STEER  "\xe2\x86\xa9"   /* ↩ */
#define STATUS "\xe2\x97\x8f"   /* ● */
#define BAR    "\xe2\x94\x82"   /* │ */
#define TCAP   "\xe2\x94\x8c"   /* ┌ */
#define TFOOT  "\xe2\x94\x94"   /* └ */
#define GEAR   "\xe2\x9a\x99"   /* ⚙ */
#define CHECK  "\xe2\x9c\x93"   /* ✓ */
#define CROSS  "\xe2\x9c\x97"   /* ✗ */

/* ═══════════════════════════════════════════════════════════
 * prompttext_body
 * ═══════════════════════════════════════════════════════════ */

static void
test_prompttext(void)
{
	char *r;
	section("prompttext_body");

	r = prompttext_body(STATUS " ready [gpt-4o]\n\nHi, there!\n");
	checkstr("basic first send", r, "Hi, there!");
	free(r);

	r = prompttext_body("Hello world");
	checkstr("no newline at all returns text", r, "Hello world");
	free(r);

	r = prompttext_body("Hello world\n");
	checkstr("single line with newline treated as status only", r, nil);

	r = prompttext_body(STATUS " ready [gpt-4o]\n\n"
	                    ECHO " Hi, there!\n\n"
	                    "The answer is 42.\n"
	                    SEP
	                    "Second question\n");
	checkstr("after separator", r, "Second question");
	free(r);

	r = prompttext_body(STATUS " ready\n\n"
	                    ECHO " first message\n\n"
	                    "reply text\n\n"
	                    "second message\n");
	checkstr("after echo", r, "second message");
	free(r);

	r = prompttext_body(STATUS " ready\n\n"
	                    STEER " Steer: do something else\n\n"
	                    "actual question\n");
	checkstr("skip steer echo", r, "actual question");
	free(r);

	r = prompttext_body(STATUS " ready\n\nline one\nline two\nline three\n");
	checkstr("multiline paragraph", r, "line one\nline two\nline three");
	free(r);

	r = prompttext_body(STATUS " ready\n\n"
	                    ECHO " q1\n\na1\n"
	                    SEP
	                    ECHO " q2\n\na2\n"
	                    SEP
	                    "q3\n");
	checkstr("multiple separators", r, "q3");
	free(r);

	r = prompttext_body("");
	checkstr("empty body", r, nil);

	r = prompttext_body(STATUS " ready\n");
	checkstr("only status line", r, nil);

	r = prompttext_body(STATUS " ready\n\n");
	checkstr("status then blank only", r, nil);

	r = prompttext_body(STATUS " ready\n\nhello   \n\n");
	checkstr("trailing whitespace stripped", r, "hello");
	free(r);

	r = prompttext_body(STATUS " ready\n\n   hello\n");
	checkstr("leading whitespace stripped", r, "hello");
	free(r);

	r = prompttext_body(STATUS " ready\n" SEP);
	checkstr("only separator", r, nil);

	r = prompttext_body(STATUS " ready\n" SEP ECHO " old prompt\n");
	checkstr("separator then echo only", r, nil);

	r = prompttext_body(STATUS " ready [gpt-4o] 5307ab16\n\nHi, there!\n");
	checkstr("realistic first send", r, "Hi, there!");
	free(r);

	r = prompttext_body(STATUS " ready [gpt-4o] 5307ab16\n\n"
	                    ECHO " Hi, there!\n\n"
	                    "Hello! How can I help you today?\n"
	                    SEP
	                    "What is 2+2?\n");
	checkstr("realistic second send", r, "What is 2+2?");
	free(r);
}

/* ═══════════════════════════════════════════════════════════
 * splitrec
 * ═══════════════════════════════════════════════════════════ */

/* build a RS-terminated record with FS-separated fields */
static int
buildrec(char *buf, int bufsz, ...)
{
	va_list ap;
	char   *tmp[64];
	int     n = 0;
	char   *f;
	char   *rec;
	long    len;

	va_start(ap, bufsz);
	while((f = va_arg(ap, char*)) != nil && n < 64)
		tmp[n++] = f;
	va_end(ap);

	rec = fmtrecfields(tmp, n, &len);
	if(rec == nil) return 0;
	if(len >= bufsz) { free(rec); return 0; }
	memmove(buf, rec, len + 1);  /* include NUL */
	free(rec);
	return (int)len;
}

static void
test_splitrec(void)
{
	char  buf[256];
	char *fields[16];
	int   n, nf;

	section("splitrec");

	/* single field */
	n = buildrec(buf, sizeof buf, "hello", nil);
	nf = splitrec(buf, n, fields, 16);
	check("single field count", nf == 1);
	checkstr("single field value", fields[0], "hello");

	/* three fields */
	n = buildrec(buf, sizeof buf, "turn_start", "uuid-1234", "gpt-4o", nil);
	nf = splitrec(buf, n, fields, 16);
	check("three fields count", nf == 3);
	checkstr("field[0]", fields[0], "turn_start");
	checkstr("field[1]", fields[1], "uuid-1234");
	checkstr("field[2]", fields[2], "gpt-4o");

	/* empty field in middle */
	n = buildrec(buf, sizeof buf, "tool_end", "", "some error", nil);
	nf = splitrec(buf, n, fields, 16);
	check("empty middle field count", nf == 3);
	checkstr("empty middle field", fields[1], "");
	checkstr("field after empty", fields[2], "some error");

	/* field containing embedded spaces and unicode */
	n = buildrec(buf, sizeof buf, "thinking", "hello\nworld ☃", nil);
	nf = splitrec(buf, n, fields, 16);
	check("unicode field count", nf == 2);
	checkstr("unicode field value", fields[1], "hello\nworld ☃");

	/* maxfields truncation */
	n = buildrec(buf, sizeof buf, "a", "b", "c", "d", "e", nil);
	nf = splitrec(buf, n, fields, 3);
	check("maxfields truncates", nf == 3);

	/* empty record (just RS) — single empty field */
	buf[0] = RS; buf[1] = '\0';
	nf = splitrec(buf, 1, fields, 16);
	check("empty record (RS only) nf==1", nf == 1);
	checkstr("empty record field is empty string", fields[0], "");

	/* no RS terminator (bare string) */
	strcpy(buf, "bare");
	nf = splitrec(buf, strlen(buf), fields, 16);
	check("bare string field count", nf == 1);
	checkstr("bare string field value", fields[0], "bare");
}

/* ═══════════════════════════════════════════════════════════
 * render_tool_start
 * ═══════════════════════════════════════════════════════════ */

static void
test_render_tool_start(void)
{
	char  *fields[8];
	char  *r;
	char   longarg[300];

	section("render_tool_start");

	/* basic: exec tool with two argv */
	fields[0] = "tool_start";
	fields[1] = "exec";
	fields[2] = "id-001";
	fields[3] = "ls";
	fields[4] = "-la";
	r = render_tool_start(fields, 5);
	check("exec tool not nil", r != nil);
	check("exec tool starts with ┌", r != nil && strncmp(r, "\n" TCAP, 4) == 0);
	check("exec tool contains name", r != nil && strstr(r, "exec") != nil);
	check("exec tool contains args", r != nil && strstr(r, "ls -la") != nil);
	free(r);

	/* no argv */
	fields[0] = "tool_start";
	fields[1] = "read_file";
	fields[2] = "id-002";
	r = render_tool_start(fields, 3);
	check("no-argv tool not nil", r != nil);
	check("no-argv tool contains name", r != nil && strstr(r, "read_file") != nil);
	free(r);

	/* missing name (nf==1) */
	fields[0] = "tool_start";
	r = render_tool_start(fields, 1);
	check("missing name uses '?'", r != nil && strstr(r, "?") != nil);
	free(r);

	/* long args get truncated */
	fields[0] = "tool_start";
	fields[1] = "write_file";
	fields[2] = "id-003";
	memset(longarg, 'x', 299);
	longarg[299] = '\0';
	fields[3] = longarg;
	r = render_tool_start(fields, 4);
	check("long args truncated: not nil", r != nil);
	check("long args truncated: ends with ...", r != nil &&
	      strstr(r, "...") != nil);
	free(r);
}

/* ═══════════════════════════════════════════════════════════
 * render_tool_end
 * ═══════════════════════════════════════════════════════════ */

static void
test_render_tool_end(void)
{
	char  *fields[4];
	char  *r;

	section("render_tool_end");

	/* success */
	fields[0] = "tool_end";
	fields[1] = "ok";
	r = render_tool_end(fields, 2);
	check("ok not nil", r != nil);
	check("ok contains ✓", r != nil && strstr(r, CHECK) != nil);
	check("ok does not contain ✗", r != nil && strstr(r, CROSS) == nil);
	free(r);

	/* error with message */
	fields[0] = "tool_end";
	fields[1] = "err";
	fields[2] = "file not found\nmore detail";
	r = render_tool_end(fields, 3);
	check("err not nil", r != nil);
	check("err contains ✗", r != nil && strstr(r, CROSS) != nil);
	check("err shows first line only", r != nil && strstr(r, "file not found") != nil);
	check("err omits second line", r != nil && strstr(r, "more detail") == nil);
	free(r);

	/* error with empty message */
	fields[0] = "tool_end";
	fields[1] = "err";
	fields[2] = "";
	r = render_tool_end(fields, 3);
	check("err empty msg not nil", r != nil);
	check("err empty msg contains ✗", r != nil && strstr(r, CROSS) != nil);
	free(r);

	/* error with nf==2 (no message field) */
	fields[0] = "tool_end";
	fields[1] = "err";
	r = render_tool_end(fields, 2);
	check("err nf==2 not nil", r != nil);
	check("err nf==2 contains ✗", r != nil && strstr(r, CROSS) != nil);
	free(r);
}

/* ═══════════════════════════════════════════════════════════
 * render_thinking
 * ═══════════════════════════════════════════════════════════ */

static void
test_render_thinking(void)
{
	char *r;
	int   cnt;
	char *p;

	section("render_thinking");

	/* single line — no embedded newlines */
	r = render_thinking("I am thinking.");
	check("single line not nil", r != nil);
	check("starts with newline+bar", r != nil &&
	      strncmp(r, "\n" BAR " ", 5) == 0);
	check("contains text", r != nil && strstr(r, "I am thinking.") != nil);
	check("no extra bar mid-line", r != nil &&
	      strstr(r, "I am thinking." BAR) == nil);
	free(r);

	/* multi-line — each line after first gets │ prefix */
	r = render_thinking("line one\nline two\nline three");
	check("multiline not nil", r != nil);
	check("multiline starts with bar", r != nil &&
	      strncmp(r, "\n" BAR " ", 5) == 0);
	check("line two gets bar", r != nil &&
	      strstr(r, "\n" BAR " line two") != nil);
	/* count occurrences of BAR */
	if(r != nil) {
		cnt = 0;
		p = r;
		while((p = strstr(p, BAR)) != nil) { cnt++; p += 3; }
		check("three bars for three lines", cnt == 3);
	}
	free(r);

	/* empty chunk */
	r = render_thinking("");
	check("empty chunk not nil", r != nil);
	check("empty chunk has leading bar", r != nil &&
	      strncmp(r, "\n" BAR " ", 5) == 0);
	free(r);

	/* trailing newline — the \n is output but no bar follows since chunk[i+1]=='\0' */
	r = render_thinking("thought\n");
	check("trailing newline: contains text", r != nil && strstr(r, "thought") != nil);
	check("trailing newline: ends with \\n not bar",
	      r != nil && r[strlen(r)-1] == '\n');
	free(r);
}

/* ═══════════════════════════════════════════════════════════
 * parsesessfile
 * ═══════════════════════════════════════════════════════════ */

/* write a session file to a temp path and return the path */
static char *
write_sessfile(char *contents, int len)
{
	static int seq = 0;
	char *path = smprint("/tmp/9aitest.%d.sess", ++seq);
	int fd = create(path, OWRITE, 0600);
	if(fd < 0) {
		free(path);
		return nil;
	}
	write(fd, contents, len);
	close(fd);
	return path;
}

/* build a RS-terminated record from a list of fields, appending to buf */
static int
append_rec(char *buf, int off, int bufsz, ...)
{
	va_list ap;
	char   *f;
	int     n = off;
	int     flen;

	va_start(ap, bufsz);
	while((f = va_arg(ap, char*)) != nil) {
		flen = strlen(f);
		if(n + flen + 2 >= bufsz) break;
		memmove(buf + n, f, flen);
		n += flen;
		buf[n++] = FS;
	}
	va_end(ap);
	if(n > off) buf[n-1] = RS;
	return n;
}

static void
test_parsesessfile(void)
{
	char  buf[1024];
	int   n = 0;
	char  uuid[37], model[64], ts[32], snippet[SESS_SNIPPET + 4];
	char *path;
	int   rc;
	char  longprompt[200];

	section("parsesessfile");

	/* ── basic: session + prompt records ── */
	n = 0;
	n = append_rec(buf, n, sizeof buf,
	               "session", "aaaabbbb-cccc-dddd-eeee-ffffffffffff", "gpt-4o", "1700000000",
	               nil);
	n = append_rec(buf, n, sizeof buf,
	               "prompt", "Hello, world!", nil);

	path = write_sessfile(buf, n);
	if(path == nil) { print("SKIP  parsesessfile (can't write tmp)\n"); return; }

	rc = parsesessfile(path, uuid, model, ts, snippet);
	check("basic: returns 0", rc == 0);
	checkstr("basic: uuid", uuid, "aaaabbbb-cccc-dddd-eeee-ffffffffffff");
	checkstr("basic: model", model, "gpt-4o");
	check("basic: ts non-empty", ts[0] != '\0');
	checkstr("basic: snippet", snippet, "Hello, world!");
	remove(path); free(path);

	/* ── session with no prompt record ── */
	n = 0;
	n = append_rec(buf, n, sizeof buf,
	               "session", "11112222-3333-4444-5555-666677778888", "claude-3", "1700000000",
	               nil);

	path = write_sessfile(buf, n);
	if(path == nil) { print("SKIP  no prompt (can't write tmp)\n"); return; }
	rc = parsesessfile(path, uuid, model, ts, snippet);
	check("no prompt: returns 0", rc == 0);
	checkstr("no prompt: uuid", uuid, "11112222-3333-4444-5555-666677778888");
	checkstr("no prompt: snippet empty", snippet, "");
	remove(path); free(path);

	/* ── file with no session record ── */
	n = 0;
	n = append_rec(buf, n, sizeof buf, "garbage", "data", nil);

	path = write_sessfile(buf, n);
	if(path == nil) { print("SKIP  no session record (can't write tmp)\n"); return; }
	rc = parsesessfile(path, uuid, model, ts, snippet);
	check("no session record: returns -1", rc == -1);
	remove(path); free(path);

	/* ── nonexistent file ── */
	rc = parsesessfile("/tmp/9aitest_nonexistent.sess", uuid, model, ts, snippet);
	check("nonexistent file: returns -1", rc == -1);

	/* ── long prompt gets truncated with ellipsis ── */
	memset(longprompt, 'a', 199);
	longprompt[199] = '\0';
	n = 0;
	n = append_rec(buf, n, sizeof buf,
	               "session", "aaaabbbb-cccc-dddd-eeee-ffffffffffff", "gpt-4o", "1700000000",
	               nil);
	n = append_rec(buf, n, sizeof buf, "prompt", longprompt, nil);

	path = write_sessfile(buf, n);
	if(path == nil) { print("SKIP  long prompt (can't write tmp)\n"); return; }
	rc = parsesessfile(path, uuid, model, ts, snippet);
	check("long prompt: returns 0", rc == 0);
	check("long prompt: snippet length <= SESS_SNIPPET", (int)strlen(snippet) <= SESS_SNIPPET);
	check("long prompt: ends with ...",
	      strlen(snippet) >= 3 &&
	      snippet[strlen(snippet)-1] == '.' &&
	      snippet[strlen(snippet)-2] == '.' &&
	      snippet[strlen(snippet)-3] == '.');
	remove(path); free(path);

	/* ── prompt with embedded newlines collapsed to spaces ── */
	n = 0;
	n = append_rec(buf, n, sizeof buf,
	               "session", "aaaabbbb-cccc-dddd-eeee-ffffffffffff", "gpt-4o", "1700000000",
	               nil);
	n = append_rec(buf, n, sizeof buf, "prompt", "line one\nline two\nline three", nil);

	path = write_sessfile(buf, n);
	if(path == nil) { print("SKIP  newlines collapsed (can't write tmp)\n"); return; }
	rc = parsesessfile(path, uuid, model, ts, snippet);
	check("newlines collapsed: returns 0", rc == 0);
	check("newlines collapsed: no newline in snippet", strchr(snippet, '\n') == nil);
	check("newlines collapsed: contains content", strstr(snippet, "line one") != nil);
	remove(path); free(path);
}

/* ═══════════════════════════════════════════════════════════
 * getevent
 * ═══════════════════════════════════════════════════════════ */

/* write an acme event string to a pipe and parse it */
static int
parse_event_str(char *s, AcmeEvent *e)
{
	int pfd[2];
	int rc;
	char buf[ACMEEVENT_BUFSZ];
	int  bufp = 0, nbuf = 0;

	if(pipe(pfd) < 0) return -1;
	write(pfd[1], s, strlen(s));
	close(pfd[1]);
	rc = getevent(pfd[0], buf, &bufp, &nbuf, e);
	close(pfd[0]);
	return rc;
}

static void
test_getevent(void)
{
	AcmeEvent e;
	int rc;
	int pfd[2];
	char evbuf[ACMEEVENT_BUFSZ];
	int  bufp, nbuf;

	section("getevent");

	/* ── simple Mx execute with text ── */
	/* format: c1 c2 q0SP q1SP flagSP nrSP text \n */
	rc = parse_event_str("Mx12 34 0 4 Send\n", &e);
	check("simple Mx: returns 1", rc == 1);
	check("simple Mx: c1='M'", e.c1 == 'M');
	check("simple Mx: c2='x'", e.c2 == 'x');
	check("simple Mx: q0=12", e.q0 == 12);
	check("simple Mx: q1=34", e.q1 == 34);
	check("simple Mx: flag=0", e.flag == 0);
	check("simple Mx: nr=4", e.nr == 4);
	checkstr("simple Mx: text", e.text, "Send");

	/* ── keyboard input (KI) ── */
	rc = parse_event_str("KI0 1 0 1 x\n", &e);
	check("KI: returns 1", rc == 1);
	check("KI: c1='K'", e.c1 == 'K');
	check("KI: c2='I'", e.c2 == 'I');

	/* ── zero-length text (nr=0) ── */
	rc = parse_event_str("Mx5 5 0 0 \n", &e);
	check("zero-length text: returns 1", rc == 1);
	checkstr("zero-length text: empty", e.text, "");

	/* ── flag & 2: expansion event ── */
	/* first event: empty (nr=0), flag=2 */
	/* expansion event: same c1/c2, has the expanded text */
	rc = parse_event_str("ML0 0 2 0 \nML5 9 0 4 Stop\n", &e);
	check("flag&2 expansion: returns 1", rc == 1);
	check("flag&2 expansion: eq0=5", e.eq0 == 5);
	check("flag&2 expansion: eq1=9", e.eq1 == 9);
	checkstr("flag&2 expansion: text", e.text, "Stop");

	/* ── UTF-8 text ── */
	/* "▶" is 3 bytes = 1 rune */
	rc = parse_event_str("MI0 1 0 1 \xe2\x96\xb6\n", &e);
	check("UTF-8 rune: returns 1", rc == 1);
	check("UTF-8 rune: text matches", memcmp(e.text, "\xe2\x96\xb6", 3) == 0);

	/* ── multiple events in sequence ── */
	bufp = 0; nbuf = 0;
	pipe(pfd);
	write(pfd[1], "Mx0 4 0 4 Send\nMx0 4 0 4 Stop\n", 31);
	close(pfd[1]);
	rc = getevent(pfd[0], evbuf, &bufp, &nbuf, &e);
	check("seq event 1: returns 1", rc == 1);
	checkstr("seq event 1: text", e.text, "Send");
	rc = getevent(pfd[0], evbuf, &bufp, &nbuf, &e);
	check("seq event 2: returns 1", rc == 1);
	checkstr("seq event 2: text", e.text, "Stop");
	rc = getevent(pfd[0], evbuf, &bufp, &nbuf, &e);
	check("seq EOF: returns 0", rc == 0);
	close(pfd[0]);

	/* ── EOF immediately ── */
	pipe(pfd);
	close(pfd[1]);
	bufp = nbuf = 0;
	rc = getevent(pfd[0], evbuf, &bufp, &nbuf, &e);
	check("immediate EOF: returns 0", rc == 0);
	close(pfd[0]);
}

/* ═══════════════════════════════════════════════════════════
 * isuuid / ismodelid
 * ═══════════════════════════════════════════════════════════ */

static void
test_predicates(void)
{
	section("isuuid / ismodelid");

	/* isuuid */
	check("valid UUID",        isuuid("550e8400-e29b-41d4-a716-446655440000") == 1);
	check("valid UUID 2",      isuuid("aaaabbbb-cccc-dddd-eeee-ffffffffffff") == 1);
	check("too short",         isuuid("550e8400-e29b-41d4") == 0);
	check("too long",          isuuid("550e8400-e29b-41d4-a716-4466554400001") == 0);
	check("wrong dash pos",    isuuid("550e840-0e29b-41d4-a716-446655440000") == 0);
	check("no dashes",         isuuid("550e8400e29b41d4a716446655440000") == 0);
	check("empty string",      isuuid("") == 0);

	/* ismodelid */
	check("simple model id",   ismodelid("gpt-4o") == 1);
	check("model with slash",  ismodelid("openai/gpt-4o") == 1);
	check("model with dots",   ismodelid("claude-3.5-sonnet") == 1);
	check("model with space",  ismodelid("gpt 4o") == 0);
	check("model with tab",    ismodelid("gpt\t4o") == 0);
	check("model with hash",   ismodelid("gpt#4o") == 0);
	check("empty model",       ismodelid("") == 0);
	check("# comment line",    ismodelid("# Middle-click a model id") == 0);
}

/* ═══════════════════════════════════════════════════════════
 * threadmain
 * ═══════════════════════════════════════════════════════════ */

void
threadmain(int argc, char *argv[])
{
	USED(argc); USED(argv);

	test_prompttext();
	test_splitrec();
	test_render_tool_start();
	test_render_tool_end();
	test_render_thinking();
	test_parsesessfile();
	test_getevent();
	test_predicates();

	print("\n%d passed, %d failed\n", passed, failed);
	if(failed > 0)
		threadexitsall("FAIL");
	threadexitsall(nil);
}
