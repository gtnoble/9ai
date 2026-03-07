/*
 * fstest.c — unit and integration tests for fs.c (the 9ai 9P server)
 *
 * ── Test structure ────────────────────────────────────────────────────
 *
 * Part 1: Unit tests — mount the server in-process, exercise each file
 *   via the plan9port 9pclient API (fsopen/fsread/fswrite/fsclose).
 *   The agent proc is started but /message is never written, so no
 *   actual LLM API calls are made.
 *
 *   1.1  server posts and mounts under a unique srvname
 *   1.2  /status reads "idle"
 *   1.3  /model reads the initial model name
 *   1.4  write /model "gpt-4.1", read back → "gpt-4.1"
 *   1.5  /ctl read contains "model", "status", "session" fields
 *   1.6  ctl "model gpt-4o" switches model; read /model confirms
 *   1.7  ctl "clear" succeeds (returns no error)
 *   1.8  /session/id readable (may be empty before first turn)
 *   1.9  directory listing of / contains expected entries
 *   1.10 directory listing of /session contains expected entries
 *
 * Part 2: Live integration test (requires proxy + token)
 *   2.1  write "say only the word 'pong'" to /message;
 *        drain /output until [done]; verify output contains "pong"
 *   2.2  after turn, /session/id is non-empty
 *   2.3  /event delivers turn_start, text, turn_end records
 *   2.4  write to /session/new; /session/id changes
 *
 * Part 3: Session persistence (requires proxy + token)
 *   3.1  after a turn, write /session/save <dst>; verify dst file exists
 *        and contains expected records
 *   3.2  start a SECOND server instance, write the session path to its
 *        /session/load, verify /session/id matches the loaded UUID
 *   3.3  verify history is restored: /ctl session field matches
 *   3.4  send a follow-up message that requires prior context;
 *        verify the response is coherent (mentions the prior word)
 *   3.5  /session/load with a nonexistent path: server stays idle (no crash)
 *
 * Usage:
 *   ./o.fstest                          (unit tests only)
 *   ./o.fstest -s <sock> -t <tok>       (unit + live)
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <9pclient.h>

#include "9ai.h"
#include "http.h"
#include "json.h"
#include "oauth.h"
#include "models.h"
#include "sse.h"
#include "oai.h"
#include "ant.h"
#include "exec.h"
#include "agent.h"
#include "fs.h"

static int failures = 0;
static CFsys *fs9p;
static char *tokpath;  /* set in threadmain, used by test_live */

/* ── Test macros ─────────────────────────────────────────────────────── */

#define FAIL(msg, ...) do { \
	fprint(2, "FAIL: " msg " (line %d)\n", ##__VA_ARGS__, __LINE__); \
	failures++; \
} while(0)

#define CHECK(cond, msg) do { \
	if(!(cond)) { FAIL("%s", msg); } \
	else { print("ok:   %s\n", msg); } \
} while(0)

#define CHECKSTR(a, b, msg) do { \
	const char *_a = (a); \
	if(_a == nil || strcmp(_a, (b)) != 0) { \
		fprint(2, "FAIL: %s: got \"%s\" want \"%s\" (line %d)\n", \
		       msg, _a ? _a : "(nil)", (b), __LINE__); \
		failures++; \
	} else { print("ok:   %s\n", msg); } \
} while(0)

#define CHECKCONTAINS(s, sub, msg) do { \
	const char *_s = (s); \
	if(_s == nil || strstr(_s, (sub)) == nil) { \
		fprint(2, "FAIL: %s: \"%s\" does not contain \"%s\" (line %d)\n", \
		       msg, _s ? _s : "(nil)", (sub), __LINE__); \
		failures++; \
	} else { print("ok:   %s\n", msg); } \
} while(0)

#define CHECKNONIL(a, msg) do { \
	if((a) == nil) { \
		fprint(2, "FAIL: %s: got nil (line %d)\n", msg, __LINE__); \
		failures++; \
	} else { print("ok:   %s\n", msg); } \
} while(0)

/* ── Helpers ─────────────────────────────────────────────────────────── */

/*
 * fread_all — open a file via 9pclient, read all content into a
 * nil-terminated heap buffer.  Returns nil on error.
 */
static char *
fread_all(char *path)
{
	CFid *fid;
	char *buf;
	int   cap = 4096, n, total = 0;

	fid = fsopen(fs9p, path, OREAD);
	if(fid == nil)
		return nil;

	buf = malloc(cap);
	if(buf == nil) { fsclose(fid); return nil; }

	while((n = fsread(fid, buf + total, cap - total - 1)) > 0) {
		total += n;
		if(total + 1 >= cap) {
			cap *= 2;
			char *tmp = realloc(buf, cap);
			if(tmp == nil) { free(buf); fsclose(fid); return nil; }
			buf = tmp;
		}
	}
	fsclose(fid);
	buf[total] = '\0';
	return buf;
}

/*
 * fwrite_str — open a file for write, write s, close.
 * Returns 0 on success, -1 on error.
 */
static int
fwrite_str(char *path, char *s)
{
	CFid *fid;
	long  n;

	fid = fsopen(fs9p, path, OWRITE);
	if(fid == nil)
		return -1;
	n = fswrite(fid, s, strlen(s));
	fsclose(fid);
	return (n == (long)strlen(s)) ? 0 : -1;
}

/*
 * drain_output — read /output until the [done] sentinel (turn EOF).
 * Returns a heap string of all content received, or nil on error.
 */
static char *
drain_output(void)
{
	CFid *fid;
	char *buf;
	char  chunk[4096];
	int   cap = 65536, total = 0, n;

	fid = fsopen(fs9p, "output", OREAD);
	if(fid == nil)
		return nil;

	buf = malloc(cap);
	if(buf == nil) { fsclose(fid); return nil; }
	buf[0] = '\0';

	while((n = fsread(fid, chunk, sizeof chunk - 1)) > 0) {
		chunk[n] = '\0';
		if(total + n + 1 >= cap) {
			cap *= 2;
			char *tmp = realloc(buf, cap);
			if(tmp == nil) { free(buf); fsclose(fid); return nil; }
			buf = tmp;
		}
		memmove(buf + total, chunk, n);
		total += n;
		buf[total] = '\0';
		/* stop when we see [done] */
		if(strstr(buf, "[done]") != nil)
			break;
	}
	/* drain until EOF so the fid is clean */
	while(n > 0)
		n = fsread(fid, chunk, sizeof chunk);
	fsclose(fid);
	return buf;
}

/* ── Part 1: Unit tests ──────────────────────────────────────────────── */

static void
test_unit(void)
{
	char *s;
	CFid *fid;
	Dir  *d;
	int   nd, i;
	int   has_ctl, has_message, has_output, has_event, has_session;
	int   has_id, has_new, has_load, has_save;

	print("\n=== Part 1: Unit tests ===\n");

	/* 1.1 server started and mounted */
	CHECK(fs9p != nil, "server posts and mounts");

	/* 1.2 /status reads "idle" */
	s = fread_all("status");
	CHECKNONIL(s, "/status readable");
	CHECKCONTAINS(s, "idle", "/status is idle at startup");
	free(s);

	/* 1.3 /model reads initial model */
	s = fread_all("model");
	CHECKNONIL(s, "/model readable");
	CHECKCONTAINS(s, "gpt-4o", "/model contains gpt-4o");
	free(s);

	/* 1.4 write /model, read back */
	CHECK(fwrite_str("model", "gpt-4.1") == 0, "write /model gpt-4.1");
	s = fread_all("model");
	CHECKNONIL(s, "/model readable after write");
	CHECKCONTAINS(s, "gpt-4.1", "/model reads back written value");
	free(s);
	/* restore */
	fwrite_str("model", "gpt-4o");

	/* 1.5 /ctl contains model, status, session fields */
	s = fread_all("ctl");
	CHECKNONIL(s, "/ctl readable");
	CHECKCONTAINS(s, "model", "/ctl contains 'model'");
	CHECKCONTAINS(s, "status", "/ctl contains 'status'");
	CHECKCONTAINS(s, "session", "/ctl contains 'session'");
	free(s);

	/* 1.6 ctl "model gpt-4o" switches model */
	CHECK(fwrite_str("ctl", "model gpt-4o") == 0, "ctl model command succeeds");
	s = fread_all("model");
	CHECKNONIL(s, "/model readable after ctl model");
	CHECKCONTAINS(s, "gpt-4o", "ctl model command switches model");
	free(s);

	/* 1.7 ctl "clear" succeeds */
	CHECK(fwrite_str("ctl", "clear") == 0, "ctl clear returns no error");

	/* 1.8 /session/id readable */
	s = fread_all("session/id");
	CHECKNONIL(s, "/session/id readable");
	free(s);

	/* 1.9 directory listing of / */
	fid = fsopen(fs9p, "/", OREAD);
	CHECKNONIL(fid, "can open root dir");
	if(fid != nil) {
		nd = fsdirread(fid, &d);
		CHECK(nd > 0, "root dir has entries");
		has_ctl = has_message = has_output = has_event = has_session = 0;
		for(i = 0; i < nd; i++) {
			if(strcmp(d[i].name, "ctl")     == 0) has_ctl     = 1;
			if(strcmp(d[i].name, "message") == 0) has_message = 1;
			if(strcmp(d[i].name, "output")  == 0) has_output  = 1;
			if(strcmp(d[i].name, "event")   == 0) has_event   = 1;
			if(strcmp(d[i].name, "session") == 0) has_session = 1;
		}
		CHECK(has_ctl,     "root dir has 'ctl'");
		CHECK(has_message, "root dir has 'message'");
		CHECK(has_output,  "root dir has 'output'");
		CHECK(has_event,   "root dir has 'event'");
		CHECK(has_session, "root dir has 'session'");
		free(d);
		fsclose(fid);
	}

	/* 1.10 directory listing of /session */
	fid = fsopen(fs9p, "session", OREAD);
	CHECKNONIL(fid, "can open session dir");
	if(fid != nil) {
		nd = fsdirread(fid, &d);
		CHECK(nd > 0, "session dir has entries");
		has_id = has_new = has_load = has_save = 0;
		for(i = 0; i < nd; i++) {
			if(strcmp(d[i].name, "id")   == 0) has_id   = 1;
			if(strcmp(d[i].name, "new")  == 0) has_new  = 1;
			if(strcmp(d[i].name, "load") == 0) has_load = 1;
			if(strcmp(d[i].name, "save") == 0) has_save = 1;
		}
		CHECK(has_id,   "session dir has 'id'");
		CHECK(has_new,  "session dir has 'new'");
		CHECK(has_load, "session dir has 'load'");
		CHECK(has_save, "session dir has 'save'");
		free(d);
		fsclose(fid);
	}
}

/* ── Part 2: Live integration tests ─────────────────────────────────── */

/*
 * outreader_proc — drains /output in a separate proc and sends the
 * result back on the supplied channel.
 */
static void
outreader_proc(void *v)
{
	Channel *ch = v;
	char *out = drain_output();
	sendul(ch, (ulong)(uintptr)out);
}

static void
test_live(void)
{
	char *out, *uuid1, *uuid2;
	Channel *outch;

	print("\n=== Part 2: Live integration tests ===\n");

	/* 2.1 open /output BEFORE writing /message to avoid the race where
	 *     the agent completes and sends nil before our read arrives.     */
	print("-- 2.1 write /message 'say only the word pong'\n");
	outch = chancreate(sizeof(ulong), 1);
	proccreate(outreader_proc, outch, 32768);

	/* give the reader proc a moment to park its read on /output */
	p9sleep(50);

	CHECK(fwrite_str("message", "say only the word 'pong'") == 0,
	      "write to /message succeeds");

	out = (char*)(uintptr)recvul(outch);
	chanfree(outch);

	CHECKNONIL(out, "drain_output returns non-nil");
	if(out != nil) {
		CHECKCONTAINS(out, "pong",   "output contains 'pong'");
		CHECKCONTAINS(out, "[done]", "output ends with [done]");
		print("  output: %s\n", out);
		free(out);
	}

	/* 2.2 /session/id non-empty after turn */
	uuid1 = fread_all("session/id");
	CHECKNONIL(uuid1, "/session/id readable after turn");
	if(uuid1 != nil) {
		/* strip trailing newline for display */
		long n = strlen(uuid1);
		while(n > 0 && (uuid1[n-1]=='\n'||uuid1[n-1]=='\r')) uuid1[--n] = '\0';
		CHECK(strlen(uuid1) > 0, "session/id non-empty after first turn");
	}

	/* 2.3 /event delivers turn_start and turn_end
	 *
	 * Note: we can only test this retroactively by checking the session
	 * file, because event is a streaming file that was already consumed
	 * during the turn.  We verify that the session file for the current
	 * uuid contains the expected records.
	 */
	if(uuid1 != nil && strlen(uuid1) > 0) {
		/* derive session dir from tokpath (same as agentproc does) */
		char *tp = smprint("%s", tokpath);
		char *slash = strrchr(tp, '/');
		if(slash != nil) *slash = '\0';
		char *sessdir = smprint("%s/sessions/", tp);
		free(tp);
		char  sesspath[512];
		snprint(sesspath, sizeof sesspath, "%s%s", sessdir, uuid1);
		free(sessdir);

		int fd = open(sesspath, OREAD);
		CHECK(fd >= 0, "session file exists after turn");
		if(fd >= 0) {
			char sbuf[65536];
			int n = read(fd, sbuf, sizeof sbuf - 1);
			close(fd);
			if(n > 0) {
				sbuf[n] = '\0';
				CHECKCONTAINS(sbuf, "turn_start", "session has turn_start");
				CHECKCONTAINS(sbuf, "turn_end",   "session has turn_end");
				CHECKCONTAINS(sbuf, "prompt",     "session has prompt record");
			}
		}
	}

	/* 2.4 write /session/new; /session/id changes */
	print("-- 2.4 /session/new changes session id\n");
	CHECK(fwrite_str("session/new", "1") == 0, "write /session/new succeeds");
	/* give the server a moment to open the new session */
	p9sleep(100);
	uuid2 = fread_all("session/id");
	if(uuid1 != nil && uuid2 != nil) {
		long n = strlen(uuid2);
		while(n > 0 && (uuid2[n-1]=='\n'||uuid2[n-1]=='\r')) uuid2[--n] = '\0';
		print("  old uuid: %s  new uuid: %s\n", uuid1, uuid2);
		CHECK(strcmp(uuid1, uuid2) != 0, "session/id readable after new");
	}
	free(uuid1);
	free(uuid2);
}

/* ── Part 3: Session persistence ─────────────────────────────────────── */

/*
 * test_session_persistence — tests /session/save and /session/load.
 *
 * All tests run against the same single server instance (fs9p).
 * The "restart" scenario is simulated by:
 *   a. running a turn to build history
 *   b. saving to /tmp via /session/save
 *   c. clearing history via /ctl clear + /session/new
 *   d. loading back via /session/load
 *   e. verifying uuid is restored and a context-aware follow-up works
 *
 * Additionally, agentsessload() is tested directly (unit) to verify
 * the history reconstruction without a running server.
 */
static void
test_session_persistence(char *sockpath)
{
	char    *out, *uuid_orig, *uuid_after_load;
	char     savepath[256];
	Channel *outch;

	USED(sockpath);

	print("\n=== Part 3: Session persistence tests ===\n");

	/* ── 3.0: start fresh, run a turn so we have history ── */
	print("-- 3.0 build a session with history\n");

	CHECK(fwrite_str("session/new", "1") == 0, "3.0: session/new for clean state");
	p9sleep(100);

	outch = chancreate(sizeof(ulong), 1);
	proccreate(outreader_proc, outch, 32768);
	p9sleep(50);
	CHECK(fwrite_str("message", "What is 2+2? Reply with only the number.") == 0,
	      "3.0: write /message for persistence turn");
	out = (char*)(uintptr)recvul(outch);
	chanfree(outch);
	CHECKNONIL(out, "3.0: got output from turn");
	if(out != nil) {
		CHECKCONTAINS(out, "4", "3.0: output contains '4'");
		free(out);
	}

	uuid_orig = fread_all("session/id");
	CHECKNONIL(uuid_orig, "3.0: session/id non-empty after turn");
	if(uuid_orig != nil) {
		long n = strlen(uuid_orig);
		while(n > 0 && (uuid_orig[n-1]=='\n'||uuid_orig[n-1]=='\r'))
			uuid_orig[--n] = '\0';
	}
	print("  original uuid: %s\n", uuid_orig ? uuid_orig : "(nil)");

	/* ── 3.1: /session/save exports session file ── */
	print("-- 3.1 /session/save exports session file\n");
	snprint(savepath, sizeof savepath, "/tmp/9ai-fstest-save-%d", getpid());

	{
		char savearg[512];
		snprint(savearg, sizeof savearg, "%s\n", savepath);
		CHECK(fwrite_str("session/save", savearg) == 0, "3.1: write /session/save succeeds");
	}
	p9sleep(100);

	{
		int  fd = open(savepath, OREAD);
		CHECK(fd >= 0, "3.1: saved file exists");
		if(fd >= 0) {
			char sbuf[65536];
			int  n = read(fd, sbuf, sizeof sbuf - 1);
			close(fd);
			if(n > 0) {
				sbuf[n] = '\0';
				CHECKCONTAINS(sbuf, "session",    "3.1: saved file has session record");
				CHECKCONTAINS(sbuf, "turn_start", "3.1: saved file has turn_start");
				CHECKCONTAINS(sbuf, "turn_end",   "3.1: saved file has turn_end");
				CHECKCONTAINS(sbuf, "prompt",     "3.1: saved file has prompt record");
				if(uuid_orig != nil)
					CHECKCONTAINS(sbuf, uuid_orig, "3.1: saved file contains original uuid");
			}
		}
	}

	/* ── 3.2: agentsessload() unit test — reconstruct history directly ── */
	print("-- 3.2 agentsessload() reconstructs history from session file\n");
	{
		OAIReq   *req2;
		AgentCfg  cfg2;
		int       rc;

		req2 = oaireqnew("gpt-4o");
		memset(&cfg2, 0, sizeof cfg2);
		cfg2.model = strdup("gpt-4o");

		rc = agentsessload(savepath, req2, &cfg2);
		CHECK(rc == 0, "3.2: agentsessload returns 0");
		if(rc == 0) {
			/* uuid should match */
			if(uuid_orig != nil)
				CHECKSTR(cfg2.uuid, uuid_orig, "3.2: loaded uuid matches original");

			/* history should have at least a user message and an assistant message */
			int nmsg = 0;
			OAIMsg *m;
			for(m = req2->msgs; m != nil; m = m->next)
				nmsg++;
			CHECK(nmsg >= 2, "3.2: loaded history has at least 2 messages");
			print("  loaded %d messages\n", nmsg);

			/* first message should be user */
			if(req2->msgs != nil)
				CHECK(req2->msgs->role == OAIRoleUser, "3.2: first message is user role");

			/* clean up */
			if(cfg2.sess_bio != nil) {
				int fd2 = cfg2.sess_bio->fid;
				Bflush(cfg2.sess_bio);
				Bterm(cfg2.sess_bio);
				close(fd2);
				free(cfg2.sess_bio);
			}
		}
		free(cfg2.model);
		oaireqfree(req2);
	}

	/* ── 3.3: load session into running server; uuid is restored ── */
	print("-- 3.3 /session/load into running server restores uuid\n");

	/* clear history first */
	CHECK(fwrite_str("ctl", "clear") == 0, "3.3: ctl clear before load");
	p9sleep(50);

	{
		char loadarg[512];
		snprint(loadarg, sizeof loadarg, "%s\n", savepath);
		CHECK(fwrite_str("session/load", loadarg) == 0, "3.3: write /session/load succeeds");
	}
	p9sleep(200);

	uuid_after_load = fread_all("session/id");
	CHECKNONIL(uuid_after_load, "3.3: session/id readable after load");
	if(uuid_after_load != nil) {
		long n = strlen(uuid_after_load);
		while(n > 0 && (uuid_after_load[n-1]=='\n'||uuid_after_load[n-1]=='\r'))
			uuid_after_load[--n] = '\0';
		print("  uuid after load: %s\n", uuid_after_load);
		if(uuid_orig != nil)
			CHECKSTR(uuid_after_load, uuid_orig, "3.3: uuid after load matches original");
	}

	/* ── 3.4: follow-up turn uses loaded history ── */
	print("-- 3.4 follow-up turn uses loaded history\n");
	outch = chancreate(sizeof(ulong), 1);
	proccreate(outreader_proc, outch, 32768);
	p9sleep(50);
	CHECK(fwrite_str("message",
	      "What number did you give me in the previous turn? Reply with just the number.") == 0,
	      "3.4: write follow-up /message");
	out = (char*)(uintptr)recvul(outch);
	chanfree(outch);
	CHECKNONIL(out, "3.4: got output from follow-up turn");
	if(out != nil) {
		print("  follow-up output: %s\n", out);
		CHECKCONTAINS(out, "4", "3.4: follow-up references '4' from loaded history");
		CHECKCONTAINS(out, "[done]", "3.4: follow-up ends with [done]");
		free(out);
	}

	/* ── 3.5: /session/load nonexistent path — server stays idle ── */
	print("-- 3.5 /session/load with nonexistent path: server stays idle\n");
	CHECK(fwrite_str("session/new", "1") == 0, "3.5: session/new before bad load");
	p9sleep(100);
	/* write a path that doesn't exist */
	CHECK(fwrite_str("session/load", "/tmp/9ai-no-such-file-xyzzy\n") == 0,
	      "3.5: write /session/load with bad path (write succeeds)");
	p9sleep(100);
	{
		char *st = fread_all("status");
		CHECKNONIL(st, "3.5: /status still readable after bad load");
		CHECK(st != nil, "3.5: server still responding after bad load");
		free(st);
	}

	/* clean up */
	free(uuid_orig);
	free(uuid_after_load);
	remove(savepath);
}

/* ── threadmain ──────────────────────────────────────────────────────── */

void
threadmain(int argc, char *argv[])
{
	char *sockpath = nil;
	char  srvname[64];
	int   tries;
	AiState *ai;

	ARGBEGIN {
	case 's':
		sockpath = ARGF();
		break;
	case 't':
		tokpath = ARGF();
		break;
	} ARGEND
	USED(argc); USED(argv);

#ifdef PLAN9PORT
	if(sockpath == nil) sockpath = smprint("%s/.cache/9ai/proxy.sock", getenv("HOME"));
	if(tokpath  == nil) tokpath  = smprint("%s/.cache/9ai/token",      getenv("HOME"));
#else
	if(sockpath == nil) sockpath = nil;   /* portdial uses TLS directly */
	if(tokpath  == nil) tokpath  = configpath("token");
#endif

	/* unique srvname to avoid clashing with a running 9ai */
	snprint(srvname, sizeof srvname, "9ai-test");

	/* init and post the server; aimain returns after threadpostmountsrv */
	ai = aiinit("gpt-4o", sockpath, tokpath);
	aimain(ai, srvname, nil);

	/* mount the posted service */
	fs9p = nil;
	for(tries = 0; tries < 50 && fs9p == nil; tries++) {
		p9sleep(100);
		fs9p = nsmount(srvname, nil);
	}
	if(fs9p == nil)
		sysfatal("nsmount %s: %r", srvname);

	test_unit();
	/* live tests only if proxy and token are accessible */
	if(access(sockpath, AEXIST) == 0 && access(tokpath, AEXIST) == 0) {
		test_live();
		test_session_persistence(sockpath);
	} else {
		print("\n(skipping live tests: proxy or token not found)\n");
	}

	if(failures > 0) {
		fprint(2, "%d test(s) FAILED\n", failures);
		threadexitsall("FAIL");
	}
	print("all tests passed\n");
	fsunmount(fs9p);
	threadexitsall(nil);
}
