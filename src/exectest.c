/*
 * exectest.c — unit and integration tests for the exec tool (exec.c)
 *
 * All tests run without network access.
 *
 * Part 1: execparse
 *   1.1  basic argv parsing: ["echo","hello"]
 *   1.2  argv with stdin: ["cat"], stdin="hello\n"
 *   1.3  multi-arg: ["printf","%s %s","foo","bar"]
 *   1.4  empty argv → error
 *   1.5  missing argv key → error
 *   1.6  argv element not a string → error
 *   1.7  stdin absent → empty string
 *
 * Part 2: execrun — basic execution
 *   2.1  echo hello                    → output "hello\n", exitcode 0
 *   2.2  printf '%s\n' world           → output "world\n", exitcode 0
 *   2.3  cat with stdin "hello"        → output "hello", exitcode 0
 *   2.4  exit 42 (via sh -c)           → exitcode 42
 *   2.5  ls /nonexistent               → exitcode non-zero
 *   2.6  unknown binary                → execrun returns nil (exec fails)
 *
 * Part 3: execresultstr
 *   3.1  exitcode 0  → output only, no suffix
 *   3.2  exitcode 1  → output + "\nexited 1"
 *   3.3  empty output, exitcode 2 → "\nexited 2"
 *
 * Part 4: output truncation (EXEC_MAXOUT)
 *   4.1  Write exactly EXEC_MAXOUT bytes → not truncated, all present
 *   4.2  Write EXEC_MAXOUT+100 bytes → truncated, tail preserved,
 *        marker present
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>

#include "9ai.h"
#include "json.h"
#include "exec.h"

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
		fprint(2, "FAIL: %s: got \"%s\" want \"%s\" (line %d)\n", \
		       msg, _a ? _a : "(nil)", _b, __LINE__); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

#define CHECKNIL(a, msg) do { \
	if((a) != nil) { \
		fprint(2, "FAIL: %s: expected nil, got non-nil (line %d)\n", \
		       msg, __LINE__); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

#define CHECKNONIL(a, msg) do { \
	if((a) == nil) { \
		fprint(2, "FAIL: %s: got nil (line %d)\n", msg, __LINE__); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

#define CHECKCONTAINS(s, sub, msg) do { \
	const char *_s = (s), *_sub = (sub); \
	if(_s == nil || strstr(_s, _sub) == nil) { \
		fprint(2, "FAIL: %s: \"%s\" does not contain \"%s\" (line %d)\n", \
		       msg, _s ? _s : "(nil)", _sub, __LINE__); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

/* ── helpers ────────────────────────────────────────────────────────── */

/* run execrun from a JSON string literal */
static ExecResult *
run(const char *json)
{
	return execrun(json, strlen(json));
}

/* ── Part 1: execparse ──────────────────────────────────────────────── */

static void
test_parse_basic(void)
{
	char *argv[16];
	char *sin;
	int   argc;
	const char *js = "{\"argv\":[\"echo\",\"hello\"]}";

	print("\n-- 1.1 execparse: basic argv\n");
	argc = execparse(js, strlen(js), argv, 16, &sin);
	CHECKEQ(argc, 2, "argc == 2");
	CHECKSTR(argv[0], "echo",  "argv[0] == echo");
	CHECKSTR(argv[1], "hello", "argv[1] == hello");
	CHECK(argv[2] == nil,       "argv[2] == nil (nil-terminated)");
	CHECKSTR(sin, "",           "stdin empty when absent");
	free(argv[0]); free(argv[1]); free(sin);
}

static void
test_parse_with_stdin(void)
{
	char *argv[16];
	char *sin;
	int   argc;
	const char *js = "{\"argv\":[\"cat\"],\"stdin\":\"hello\\n\"}";

	print("\n-- 1.2 execparse: argv + stdin\n");
	argc = execparse(js, strlen(js), argv, 16, &sin);
	CHECKEQ(argc, 1,     "argc == 1");
	CHECKSTR(argv[0], "cat", "argv[0] == cat");
	CHECKSTR(sin, "hello\n", "stdin == hello\\n");
	free(argv[0]); free(sin);
}

static void
test_parse_multiarg(void)
{
	char *argv[16];
	char *sin;
	int   argc;
	const char *js = "{\"argv\":[\"printf\",\"%s %s\",\"foo\",\"bar\"]}";

	print("\n-- 1.3 execparse: multi-arg\n");
	argc = execparse(js, strlen(js), argv, 16, &sin);
	CHECKEQ(argc, 4,         "argc == 4");
	CHECKSTR(argv[0], "printf", "argv[0]");
	CHECKSTR(argv[1], "%s %s",  "argv[1]");
	CHECKSTR(argv[2], "foo",    "argv[2]");
	CHECKSTR(argv[3], "bar",    "argv[3]");
	CHECK(argv[4] == nil,        "argv[4] nil");
	free(argv[0]); free(argv[1]); free(argv[2]); free(argv[3]); free(sin);
}

static void
test_parse_empty_argv(void)
{
	char *argv[16];
	char *sin = nil;
	int   argc;
	const char *js = "{\"argv\":[]}";

	print("\n-- 1.4 execparse: empty argv → error\n");
	argc = execparse(js, strlen(js), argv, 16, &sin);
	CHECKEQ(argc, -1, "empty argv returns -1");
	CHECKNIL(sin,     "stdin not allocated on error");
}

static void
test_parse_missing_argv(void)
{
	char *argv[16];
	char *sin = nil;
	int   argc;
	const char *js = "{\"stdin\":\"foo\"}";

	print("\n-- 1.5 execparse: missing argv → error\n");
	argc = execparse(js, strlen(js), argv, 16, &sin);
	CHECKEQ(argc, -1, "missing argv returns -1");
	CHECKNIL(sin,     "stdin not allocated on error");
}

static void
test_parse_nonstring_argv(void)
{
	char *argv[16];
	char *sin = nil;
	int   argc;
	const char *js = "{\"argv\":[42]}";

	print("\n-- 1.6 execparse: non-string argv element → error\n");
	argc = execparse(js, strlen(js), argv, 16, &sin);
	CHECKEQ(argc, -1, "non-string argv[0] returns -1");
	CHECKNIL(sin,     "stdin not allocated on error");
}

static void
test_parse_absent_stdin(void)
{
	char *argv[16];
	char *sin;
	int   argc;
	const char *js = "{\"argv\":[\"ls\"]}";

	print("\n-- 1.7 execparse: absent stdin → empty string\n");
	argc = execparse(js, strlen(js), argv, 16, &sin);
	CHECKEQ(argc, 1,   "argc == 1");
	CHECKSTR(sin, "",  "stdin is empty string when absent");
	CHECK(sin != nil,  "sin is non-nil (empty string, not nil)");
	free(argv[0]); free(sin);
}

/* ── Part 2: execrun ────────────────────────────────────────────────── */

static void
test_run_echo(void)
{
	ExecResult *r;
	print("\n-- 2.1 execrun: echo hello\n");
	r = run("{\"argv\":[\"echo\",\"hello\"]}");
	CHECKNONIL(r,                          "execrun returns non-nil");
	if(r == nil) return;
	CHECKCONTAINS(r->output, "hello",      "output contains 'hello'");
	CHECKEQ(r->exitcode, 0,                "exitcode 0");
	CHECKEQ(r->truncated, 0,              "not truncated");
	execresultfree(r);
}

static void
test_run_printf(void)
{
	ExecResult *r;
	print("\n-- 2.2 execrun: printf\n");
	r = run("{\"argv\":[\"printf\",\"%s\\n\",\"world\"]}");
	CHECKNONIL(r, "execrun returns non-nil");
	if(r == nil) return;
	CHECKCONTAINS(r->output, "world", "output contains 'world'");
	CHECKEQ(r->exitcode, 0,           "exitcode 0");
	execresultfree(r);
}

static void
test_run_cat_stdin(void)
{
	ExecResult *r;
	print("\n-- 2.3 execrun: cat with stdin\n");
	r = run("{\"argv\":[\"cat\"],\"stdin\":\"hello from stdin\"}");
	CHECKNONIL(r, "execrun returns non-nil");
	if(r == nil) return;
	CHECKCONTAINS(r->output, "hello from stdin", "output echoes stdin");
	CHECKEQ(r->exitcode, 0,                      "exitcode 0");
	execresultfree(r);
}

static void
test_run_nonzero_exit(void)
{
	ExecResult *r;
	print("\n-- 2.4 execrun: non-zero exit\n");
	/* 'false' exits with code 1 on all Unix systems */
	r = run("{\"argv\":[\"false\"]}");
	CHECKNONIL(r, "execrun returns non-nil even on failure");
	if(r == nil) return;
	CHECK(r->exitcode != 0, "exitcode non-zero for 'false'");
	execresultfree(r);
}

static void
test_run_ls_nonexistent(void)
{
	ExecResult *r;
	print("\n-- 2.5 execrun: ls /nonexistent\n");
	r = run("{\"argv\":[\"ls\",\"/9ai-nonexistent-path-abc\"]}");
	CHECKNONIL(r, "execrun returns non-nil");
	if(r == nil) return;
	CHECK(r->exitcode != 0, "exitcode non-zero for missing path");
	execresultfree(r);
}

static void
test_run_unknown_binary(void)
{
	ExecResult *r;
	print("\n-- 2.6 execrun: unknown binary → nil\n");
	r = run("{\"argv\":[\"/9ai-no-such-binary-xyz\"]}");
	/* threadspawn fails → execrun returns nil */
	CHECKNIL(r, "execrun returns nil for unknown binary");
	if(r != nil) execresultfree(r);
}

/* ── Part 3: execresultstr ──────────────────────────────────────────── */

static void
test_resultstr_success(void)
{
	ExecResult r;
	char buf[256];
	const char *out = "some output\n";

	print("\n-- 3.1 execresultstr: exitcode 0 → no suffix\n");
	memset(&r, 0, sizeof r);
	r.output    = (char *)out;
	r.outputlen = strlen(out);
	r.exitcode  = 0;

	execresultstr(&r, buf, sizeof buf);
	CHECKSTR(buf, "some output\n", "exitcode 0: output only");
}

static void
test_resultstr_failure(void)
{
	ExecResult r;
	char buf[256];
	const char *out = "oops\n";

	print("\n-- 3.2 execresultstr: exitcode 1 → suffix appended\n");
	memset(&r, 0, sizeof r);
	r.output    = (char *)out;
	r.outputlen = strlen(out);
	r.exitcode  = 1;

	execresultstr(&r, buf, sizeof buf);
	CHECKCONTAINS(buf, "oops\n",    "output present");
	CHECKCONTAINS(buf, "exited 1",  "exit suffix present");
}

static void
test_resultstr_empty_output(void)
{
	ExecResult r;
	char buf[256];

	print("\n-- 3.3 execresultstr: empty output, exitcode 2\n");
	memset(&r, 0, sizeof r);
	r.output    = strdup("");
	r.outputlen = 0;
	r.exitcode  = 2;

	execresultstr(&r, buf, sizeof buf);
	CHECKCONTAINS(buf, "exited 2", "exit suffix when no output");
	free(r.output);
}

/* ── Part 4: output truncation ──────────────────────────────────────── */

/*
 * Build a JSON exec call that writes exactly `nbytes` to stdout via
 * a shell here-doc approach.  We use dd to produce exactly N bytes.
 */
static void
test_truncation_exact(void)
{
	ExecResult *r;
	char json[256];
	long want = EXEC_MAXOUT;

	print("\n-- 4.1 output exactly EXEC_MAXOUT bytes → not truncated\n");

	/* dd if=/dev/zero bs=1 count=N | tr '\0' 'x' */
	snprint(json, sizeof json,
	        "{\"argv\":[\"sh\",\"-c\","
	        "\"dd if=/dev/zero bs=1 count=%ld 2>/dev/null | tr '\\\\0' 'x'\"]}",
	        want);
	r = run(json);
	CHECKNONIL(r, "execrun returns non-nil");
	if(r == nil) return;
	CHECKEQ(r->truncated, 0,  "not truncated at exactly EXEC_MAXOUT");
	CHECKEQ(r->outputlen, want, "outputlen == EXEC_MAXOUT");
	CHECKEQ(r->exitcode, 0,   "exitcode 0");
	execresultfree(r);
}

static void
test_truncation_overflow(void)
{
	ExecResult *r;
	char json[256];
	long extra = 100;
	long total = EXEC_MAXOUT + extra;

	print("\n-- 4.2 output EXEC_MAXOUT+100 bytes → truncated, tail kept\n");

	snprint(json, sizeof json,
	        "{\"argv\":[\"sh\",\"-c\","
	        "\"dd if=/dev/zero bs=1 count=%ld 2>/dev/null | tr '\\\\0' 'x'\"]}",
	        total);
	r = run(json);
	CHECKNONIL(r, "execrun returns non-nil");
	if(r == nil) return;
	CHECKEQ(r->truncated, 1,        "truncated flag set");
	CHECKEQ(r->outputlen, EXEC_MAXOUT, "outputlen capped at EXEC_MAXOUT");
	CHECKCONTAINS(r->output, "[...truncated...]", "truncation marker present");
	/* tail should be all 'x' bytes (past the marker) */
	{
		const char *tail = r->output + strlen("[...truncated...]\n");
		int all_x = 1, i;
		for(i = 0; tail[i] != '\0'; i++)
			if(tail[i] != 'x') { all_x = 0; break; }
		CHECK(all_x, "tail bytes are all 'x' (most recent data)");
	}
	execresultfree(r);
}

/* ── Main ────────────────────────────────────────────────────────────── */

void
threadmain(int argc, char *argv[])
{
	USED(argc); USED(argv);

	print("=== Part 1: execparse ===\n");
	test_parse_basic();
	test_parse_with_stdin();
	test_parse_multiarg();
	test_parse_empty_argv();
	test_parse_missing_argv();
	test_parse_nonstring_argv();
	test_parse_absent_stdin();

	print("\n=== Part 2: execrun ===\n");
	test_run_echo();
	test_run_printf();
	test_run_cat_stdin();
	test_run_nonzero_exit();
	test_run_ls_nonexistent();
	test_run_unknown_binary();

	print("\n=== Part 3: execresultstr ===\n");
	test_resultstr_success();
	test_resultstr_failure();
	test_resultstr_empty_output();

	print("\n=== Part 4: truncation ===\n");
	test_truncation_exact();
	test_truncation_overflow();

	if(failures > 0) {
		fprint(2, "\n%d test(s) FAILED\n", failures);
		threadexitsall("FAIL");
	}
	print("\nall tests passed\n");
	threadexitsall(nil);
}
