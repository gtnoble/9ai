/*
 * exec.c — exec tool implementation
 *
 * See exec.h for the interface.
 *
 * ── Pipe layout ───────────────────────────────────────────────────────
 *
 * Before threadspawn():
 *   stdin_pipe[0]  → child stdin  (child reads)
 *   stdin_pipe[1]  → write end    (parent writes, then closes)
 *   out_pipe[0]    → read end     (parent collects output here)
 *   out_pipe[1]    → child stdout+stderr (child writes)
 *
 * child_fds passed to threadspawn():
 *   child_fds[0] = stdin_pipe[0]
 *   child_fds[1] = out_pipe[1]    ← child stdout
 *   child_fds[2] = out_pipe[1]    ← child stderr (merged)
 *
 * threadspawn() closes child_fds[0..2] in the parent once the child
 * has started.  After it returns we own:
 *   stdin_pipe[1]  — write stdin then close
 *   out_pipe[0]    — read output until EOF
 *
 * ── Output ring buffer ────────────────────────────────────────────────
 *
 * We allocate EXEC_MAXOUT bytes upfront and fill them as a ring buffer.
 * Once full, new bytes overwrite the oldest (wpos wraps to 0).  At EOF
 * we linearise the ring so the result is the most recent EXEC_MAXOUT
 * bytes, with "[...truncated...]\n" prepended to mark the loss.
 *
 * ── Wait semantics ────────────────────────────────────────────────────
 *
 * We use procwait(pid) — the libthread-safe wait that consumes from
 * threadwaitchan.  threadspawn's internal execproc already calls
 * waitfor(pid) and delivers the Waitmsg to threadwaitchan; using
 * waitfor() directly from our thread proc would race with that.
 *
 * ── Exit code extraction ──────────────────────────────────────────────
 *
 * On plan9port, Waitmsg.msg for a non-zero exit looks like:
 *   "exit status 1"  (from lib9/wait.c _p9waitnohang)
 * We scan backward for the trailing integer.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>

#include "json.h"
#include "exec.h"

/* ── execparse ──────────────────────────────────────────────────────── */

int
execparse(const char *args_json, int args_len,
          char *argv[], int maxargv, char **stdin_out)
{
	enum { MAXTOK = 512 };
	jsmn_parser  p;
	jsmntok_t    toks[MAXTOK];
	int          ntoks;
	int          argv_i, stdin_i;
	int          argc, elem, i;
	char         buf[EXEC_MAXARG];

	jsmn_init(&p);
	ntoks = jsmn_parse(&p, args_json, args_len, toks, MAXTOK);
	if(ntoks < 1 || toks[0].type != JSMN_OBJECT) {
		werrstr("exec: bad tool args JSON (ntoks=%d)", ntoks);
		return -1;
	}

	/* find "argv" array */
	argv_i = jsonget(args_json, toks, ntoks, 0, "argv");
	if(argv_i < 0 || toks[argv_i].type != JSMN_ARRAY) {
		werrstr("exec: missing or non-array 'argv'");
		return -1;
	}
	argc = toks[argv_i].size;
	if(argc <= 0) {
		werrstr("exec: empty argv");
		return -1;
	}
	if(argc >= maxargv - 1) {
		werrstr("exec: too many argv elements (%d)", argc);
		return -1;
	}

	/* copy each argv element */
	elem = argv_i + 1;
	for(i = 0; i < argc; i++) {
		if(toks[elem].type != JSMN_STRING) {
			werrstr("exec: argv[%d] is not a string", i);
			while(--i >= 0) free(argv[i]);
			return -1;
		}
		if(jsonstr(args_json, &toks[elem], buf, sizeof buf) < 0) {
			werrstr("exec: argv[%d] too long or decode error", i);
			while(--i >= 0) free(argv[i]);
			return -1;
		}
		argv[i] = strdup(buf);
		if(argv[i] == nil) {
			werrstr("exec: strdup argv[%d]: %r", i);
			while(--i >= 0) free(argv[i]);
			return -1;
		}
		elem = jsonnext(toks, ntoks, elem);
	}
	argv[argc] = nil;

	/* find optional "stdin" string */
	stdin_i = jsonget(args_json, toks, ntoks, 0, "stdin");
	if(stdin_i >= 0 && toks[stdin_i].type == JSMN_STRING) {
		int raw_len = toks[stdin_i].end - toks[stdin_i].start;
		char *sinbuf = malloc(raw_len + 1);
		if(sinbuf == nil) {
			for(i = 0; i < argc; i++) free(argv[i]);
			argv[0] = nil;
			werrstr("exec: malloc stdin: %r");
			return -1;
		}
		if(jsonstr(args_json, &toks[stdin_i], sinbuf, raw_len + 1) < 0) {
			free(sinbuf);
			sinbuf = strdup("");
		}
		*stdin_out = sinbuf;
	} else {
		*stdin_out = strdup("");
	}

	return argc;
}

/* ── collectoutput ──────────────────────────────────────────────────── */

/*
 * collectoutput — drain fd into a ring buffer capped at EXEC_MAXOUT.
 *
 * Returns a malloc'd nil-terminated string containing the output (or
 * the most recent EXEC_MAXOUT bytes with a truncation marker prepended).
 * Sets *lenp and *truncated.  Returns nil only on allocation failure.
 */
static char *
collectoutput(int fd, long *lenp, int *truncated)
{
	static const char trunc_marker[] = "[...truncated...]\n";
	enum { TRUNC_LEN = sizeof(trunc_marker) - 1 };
	enum { CAP = EXEC_MAXOUT };
	enum { CHUNK = 8192 };

	char *ring;
	long  wpos;     /* next write position in ring */
	int   wrapped;  /* set once wpos has looped past CAP */
	char  tmp[CHUNK];
	long  n, rem, src, space, copy;

	*truncated = 0;
	*lenp      = 0;

	ring = malloc(CAP + 1);
	if(ring == nil)
		return nil;

	wpos    = 0;
	wrapped = 0;

	for(;;) {
		n = read(fd, tmp, sizeof tmp);
		if(n <= 0)
			break;

		if(!wrapped && wpos + n <= CAP) {
			/* fast path: fits without wrapping */
			memmove(ring + wpos, tmp, n);
			wpos += n;
		} else {
			/* ring write: may wrap */
			wrapped = 1;
			rem = n;
			src = 0;
			while(rem > 0) {
				space = CAP - wpos;
				copy  = rem < space ? rem : space;
				memmove(ring + wpos, tmp + src, copy);
				wpos  = (wpos + copy) % CAP;
				src  += copy;
				rem  -= copy;
			}
		}
	}

	if(!wrapped) {
		/* no overflow: ring[0..wpos) is the full output */
		ring[wpos] = '\0';
		*lenp = wpos;
		return ring;
	}

	/*
	 * Overflow: linearise ring starting at wpos (oldest byte).
	 * ring[wpos..CAP) ++ ring[0..wpos) = chronological order.
	 */
	*truncated = 1;
	{
		char *linear = malloc(CAP + 1);
		if(linear == nil) {
			free(ring);
			return nil;
		}
		long tail = CAP - wpos;
		memmove(linear, ring + wpos, tail);
		memmove(linear + tail, ring, wpos);
		linear[CAP] = '\0';
		free(ring);
		ring = linear;
	}

	/* overwrite beginning with truncation marker */
	if((long)TRUNC_LEN <= (long)CAP)
		memmove(ring, trunc_marker, TRUNC_LEN);

	*lenp = CAP;
	return ring;
}

/* ── execrun ────────────────────────────────────────────────────────── */

ExecResult *
execrun(const char *args_json, int args_len)
{
	char       *argv[EXEC_MAXARGV + 1];
	char       *stdin_str;
	int         argc;
	int         stdin_pipe[2], out_pipe[2];
	int         child_fds[3];
	int         pid;
	ExecResult *r;
	Waitmsg    *w;
	int         i;

	/* parse tool args JSON */
	argc = execparse(args_json, args_len, argv, EXEC_MAXARGV, &stdin_str);
	if(argc < 0)
		return nil;

	/* create pipes */
	if(pipe(stdin_pipe) < 0) {
		werrstr("exec: stdin pipe: %r");
		goto err_free_argv;
	}
	if(pipe(out_pipe) < 0) {
		werrstr("exec: out pipe: %r");
		close(stdin_pipe[0]);
		close(stdin_pipe[1]);
		goto err_free_argv;
	}

	/*
	 * Hand child-side fds to threadspawn.
	 * threadspawn closes child_fds[0..2] in the parent after fork.
	 */
	child_fds[0] = stdin_pipe[0];
	child_fds[1] = out_pipe[1];
	child_fds[2] = out_pipe[1];   /* merge stderr into stdout */

	pid = threadspawn(child_fds, argv[0], argv);
	if(pid < 0) {
		werrstr("exec: threadspawn %s: %r", argv[0]);
		/*
		 * threadspawn failed before fork — child_fds may or may not
		 * have been closed.  Close all to be safe.
		 */
		close(stdin_pipe[0]);
		close(stdin_pipe[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		goto err_free_argv;
	}

	/*
	 * Parent now owns:
	 *   stdin_pipe[1]  — write end (child's stdin)
	 *   out_pipe[0]    — read end  (child's stdout+stderr)
	 *
	 * threadspawn closed: stdin_pipe[0], out_pipe[1] (both copies)
	 */

	/* write stdin then close */
	if(stdin_str[0] != '\0') {
		long slen    = strlen(stdin_str);
		long written = 0, n;
		while(written < slen) {
			n = write(stdin_pipe[1], stdin_str + written, slen - written);
			if(n <= 0)
				break;
			written += n;
		}
	}
	close(stdin_pipe[1]);

	/* allocate result */
	r = mallocz(sizeof *r, 1);
	if(r == nil) {
		werrstr("exec: mallocz result: %r");
		close(out_pipe[0]);
		goto err_free_argv;
	}
	r->pid = pid;

	/* collect combined output */
	r->output = collectoutput(out_pipe[0], &r->outputlen, &r->truncated);
	close(out_pipe[0]);

	if(r->output == nil) {
		werrstr("exec: collectoutput: %r");
		execresultfree(r);
		goto err_free_argv;
	}

	/* wait for child via thread-safe procwait */
	w = procwait(pid);
	if(w != nil) {
		/*
		 * Waitmsg.msg is "" on success.
		 * On non-zero exit plan9port sets it to e.g. "exit status 1".
		 * Scan backward for the trailing integer.
		 */
		if(w->msg != nil && w->msg[0] != '\0') {
			char *p = w->msg + strlen(w->msg);
			while(p > w->msg && p[-1] >= '0' && p[-1] <= '9')
				p--;
			if(*p >= '1' && *p <= '9')
				r->exitcode = atoi(p);
			else
				r->exitcode = 1;
		}
		free(w);
	}

	for(i = 0; i < argc; i++)
		free(argv[i]);
	free(stdin_str);
	return r;

err_free_argv:
	for(i = 0; i < argc; i++)
		free(argv[i]);
	free(stdin_str);
	return nil;
}

/* ── execabort ──────────────────────────────────────────────────────── */

void
execabort(int pid)
{
	Waitmsg *w;
	int      i;

	if(pid <= 0)
		return;

	/* SIGTERM */
	postnote(PNPROC, pid, "kill");

	/* poll for up to 2 seconds */
	for(i = 0; i < 20; i++) {
		sleep(100);
		w = waitnohang();
		if(w != nil) {
			int dead = (w->pid == pid);
			free(w);
			if(dead)
				return;
		}
	}

	/* force-kill */
	postnote(PNPROC, pid, "kill");
}

/* ── execresultstr ──────────────────────────────────────────────────── */

char *
execresultstr(ExecResult *r, char *buf, int n)
{
	int outlen;

	if(r == nil || buf == nil || n <= 0) {
		if(buf && n > 0) buf[0] = '\0';
		return buf;
	}

	/*
	 * Reserve 32 bytes for "\nexited N\0" suffix.
	 * Copy output up to that limit.
	 */
	outlen = (int)r->outputlen;
	if(outlen > n - 33)
		outlen = n - 33;
	if(outlen < 0)
		outlen = 0;

	if(r->output != nil)
		memmove(buf, r->output, outlen);
	buf[outlen] = '\0';

	if(r->exitcode != 0)
		snprint(buf + outlen, n - outlen, "\nexited %d", r->exitcode);

	return buf;
}

/* ── execresultfree ─────────────────────────────────────────────────── */

void
execresultfree(ExecResult *r)
{
	if(r == nil)
		return;
	free(r->output);
	free(r);
}
