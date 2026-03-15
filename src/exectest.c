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
 *   2.2  echo world                    → output "world\n", exitcode 0
 *   2.3  cat with stdin "hello"        → output "hello", exitcode 0
 *   2.4  test 0 = 1                     → exitcode non-zero
 *   2.5  ls /nonexistent               → exitcode non-zero
 *   2.6  unknown binary                → execrun returns nil (exec fails)
 *
 * Part 3: execresultstr
 *   3.1  exitcode 0  → output only, no suffix
 *   3.2  exitcode 1  → output + "\nexited 1"
 *   3.3  empty output, exitcode 2 → "\nexited 2"
 *
 * Part 4: output truncation
 *   4.1  Write exactly TEST_CAP bytes → not truncated, all present
 *   4.2  Write TEST_CAP+100 bytes → truncated, tail preserved,
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
		fprint(2, "FAIL: %s: expected nil, got non-nil\n", \
		       msg); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

#define CHECKNONIL(a, msg) do { \
	if((a) == nil) { \
		fprint(2, "FAIL: %s: got nil\n", msg); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

#define CHECKCONTAINS(s, sub, msg) do { \
	const char *_s = (s), *_sub = (sub); \
	if(_s == nil || strstr(_s, _sub) == nil) { \
		fprint(2, "FAIL: %s: \"%s\" does not contain \"%s\"\n", \
		       msg, _s ? _s : "(nil)", _sub); \
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
	return execrun(json, strlen(json), 512*1024);
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
	{
		char errbuf[256];
		const char *ep = errbuf;
		rerrstr(errbuf, sizeof errbuf);
		CHECKCONTAINS(ep, "non-empty array", "empty argv: error mentions non-empty array");
		CHECKCONTAINS(ep, "Expected:",       "empty argv: error shows expected schema");
	}
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
	{
		char errbuf[256];
		const char *ep = errbuf;
		rerrstr(errbuf, sizeof errbuf);
		CHECKCONTAINS(ep, "missing the required 'argv'", "missing argv: error identifies missing field");
		CHECKCONTAINS(ep, "Expected:",                   "missing argv: error shows expected schema");
	}

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
	r = run("{\"argv\":[\"/bin/echo\",\"hello\"]}");
	CHECKNONIL(r,                          "execrun returns non-nil");
	if(r == nil) return;
	CHECKCONTAINS(r->output, "hello",      "output contains 'hello'");
	CHECKEQ(r->exitcode, 0,                "exitcode 0");
	CHECKEQ(r->truncated, 0,              "not truncated");
	execresultfree(r);
}

static void
test_run_echo_world(void)
{
	ExecResult *r;
	print("\n-- 2.2 execrun: echo world\n");
	r = run("{\"argv\":[\"echo\",\"world\"]}");
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
	/* 'test 0 = 1' always fails with a non-zero exit on Plan 9 */
	r = run("{\"argv\":[\"test\",\"0\",\"=\",\"1\"]}");
	CHECKNONIL(r, "execrun returns non-nil even on failure");
	if(r == nil) return;
	CHECK(r->exitcode != 0, "exitcode non-zero for 'test 0 = 1'");
	execresultfree(r);
}

static void
test_run_ls_nonexistent(void)
{
	ExecResult *r;
	print("\n-- 2.5 execrun: ls /nonexistent\n");
	r = run("{\"argv\":[\"/bin/ls\",\"/9ai-nonexistent-path-abc\"]}");
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
	CHECKNIL(r, "execrun returns nil for unknown binary");
	if(r == nil) {
		char errbuf[256];
		rerrstr(errbuf, sizeof errbuf);
		CHECKCONTAINS(errbuf, "not found", "errstr mentions 'not found'");
		CHECKCONTAINS(errbuf, "/9ai-no-such-binary-xyz", "errstr names the binary");
	}
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
 * run_cat_stdin_n — run cat with exactly nbytes of 'x' as stdin.
 *
 * Builds the JSON on the heap since nbytes can exceed the test cap.
 * The JSON is:  {"argv":["cat"],"stdin":"xxx..."}
 * The stdin value is nbytes 'x' characters (no JSON escaping needed).
 */

enum { TEST_CAP = 64 * 1024 };  /* 64 KB cap used in truncation tests */

static ExecResult *
run_cat_stdin_n(long nbytes)
{
	/* JSON overhead: {"argv":["cat"],"stdin":"..."} */
	long jlen = 30 + nbytes;
	char *json = malloc(jlen + 1);
	char *p;
	ExecResult *r;

	if(json == nil)
		return nil;
	p = json;
	p += sprint(p, "{\"argv\":[\"cat\"],\"stdin\":\"");
	memset(p, 'x', nbytes);
	p += nbytes;
	p += sprint(p, "\"}");
	*p = '\0';
	r = execrun(json, (int)(p - json), TEST_CAP);
	free(json);
	return r;
}

static void
test_truncation_exact(void)
{
	ExecResult *r;
	long want = TEST_CAP;

	print("\n-- 4.1 output exactly TEST_CAP bytes → not truncated\n");
	r = run_cat_stdin_n(want);
	CHECKNONIL(r, "execrun returns non-nil");
	if(r == nil) return;
	CHECKEQ(r->truncated, 0,    "not truncated at exactly TEST_CAP");
	CHECKEQ(r->outputlen, want, "outputlen == TEST_CAP");
	CHECKEQ(r->exitcode, 0,     "exitcode 0");
	execresultfree(r);
}

static void
test_truncation_overflow(void)
{
	ExecResult *r;
	long extra    = 100;
	long total    = TEST_CAP + extra;
	long marklen;

	print("\n-- 4.2 output TEST_CAP+100 bytes → truncated, tail kept\n");
	r = run_cat_stdin_n(total);
	CHECKNONIL(r, "execrun returns non-nil");
	if(r == nil) return;
	CHECKEQ(r->truncated, 1, "truncated flag set");
	CHECKCONTAINS(r->output, "[...truncated...]", "truncation marker present");
	/* output is marker + TEST_CAP bytes; all payload bytes are 'x' */
	{
		const char *marker = "[...truncated...]\n";
		marklen = strlen(marker);
		long paylen = r->outputlen - marklen;
		int all_x = 1;
		long i;
		for(i = 0; i < paylen; i++)
			if(r->output[marklen + i] != 'x') { all_x = 0; break; }
		CHECK(all_x, "tail bytes after marker are all 'x'");
		CHECKEQ(paylen, TEST_CAP, "tail length == TEST_CAP");
	}
	execresultfree(r);
}

/* ── Main ────────────────────────────────────────────────────────────── */

/*
 * execparse uses heap-allocated toks[] (8 KB) and buf[] (4 KB), but the
 * execrun call chain still needs headroom for execparse's other locals,
 * collectoutput's locals, and the test functions themselves.  Use a
 * generous stack to be safe, matching the pattern in anttest.c.
 */
int mainstacksize = 65536;

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
	test_run_echo_world();
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
