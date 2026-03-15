/*
 * exec.c — exec tool implementation
 *
 * See exec.h for the interface.
 *
 * ── Pipe layout ───────────────────────────────────────────────────────
 *
 * Before spawnchild():
 *   stdin_pipe[0]  → child stdin  (child reads)
 *   stdin_pipe[1]  → write end    (parent writes, then closes)
 *   out_pipe[0]    → read end     (parent collects output here)
 *   out_pipe[1]    → child stdout+stderr (child writes)
 *
 * child_fds passed to spawnchild():
 *   child_fds[0] = stdin_pipe[0]
 *   child_fds[1] = out_pipe[1]    ← child stdout
 *   child_fds[2] = out_pipe[1]    ← child stderr (merged)
 *
 * On plan9port, threadspawn() closes child_fds[0..2] in the parent.
 * On 9front, spawnchild() uses RFFDG so the child gets a full copy of
 * the parent's fd table, then explicitly closes stdin_pipe[1] and
 * out_pipe[0] (the parent-side ends) before exec — preventing the
 * deadlock where those open copies would block EOF on the pipes.
 *
 * After spawnchild() returns the parent owns:
 *   stdin_pipe[1]  — write stdin then close
 *   out_pipe[0]    — read output until EOF
 *
 * ── Output ring buffer ────────────────────────────────────────────────
 *
 * We allocate maxout bytes upfront and fill them as a ring buffer.
 * Once full, new bytes overwrite the oldest (wpos wraps to 0).  At EOF
 * we linearise the ring so the result is the most recent maxout bytes,
 * with "[...truncated...]\n" prepended to mark the loss.
 *
 * ── Wait semantics ────────────────────────────────────────────────────
 *
 * We use procwait(pid) from libthread, which is safe on both plan9port
 * and native 9front.
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
#include "9ai.h"
#include "exec.h"

/* ── platform spawn/wait helpers ────────────────────────────────────── */

#ifdef PLAN9PORT

static int
spawnchild(int child_fds[3], int close_fds[2], char *prog, char *argv[])
{
	USED(close_fds);  /* threadspawn handles fd cleanup on plan9port */
	return threadspawn(child_fds, prog, argv);
}

static Waitmsg *
waitchild(int pid)
{
	return procwait(pid);
}

static Waitmsg *
waitpolled(void)
{
	return waitnohang();
}

#else

/*
 * execpath — try to exec prog, searching $path for bare names.
 *
 * Called only in the child after rfork; must not touch libthread state.
 * On success this function never returns.
 * On failure, returns (caller should _exits).
 */
static void
execpath(char *prog, char *argv[])
{
	char *path, *p, *q, buf[512];
	int n;

	/* absolute or relative path — try directly */
	if(strchr(prog, '/') != nil) {
		exec(prog, argv);
		return;
	}

	/* search each directory in $path (space-separated) */
	path = getenv("path");
	if(path == nil || path[0] == '\0')
		path = "/bin";

	p = path;
	for(;;) {
		/* skip spaces */
		while(*p == ' ') p++;
		if(*p == '\0') break;

		/* find end of this directory */
		q = p;
		while(*q != ' ' && *q != '\0') q++;

		n = (int)(q - p);
		if(n + 1 + (int)strlen(prog) + 1 < (int)sizeof buf) {
			memmove(buf, p, n);
			buf[n] = '/';
			strcpy(buf + n + 1, prog);
			exec(buf, argv);
			/* exec only returns on failure; try next dir */
		}
		p = q;
	}
	/* free(path) would call libc; just leave it — we're about to _exits */
}

static int
spawnchild(int child_fds[3], int close_fds[2], char *prog, char *argv[])
{
	int pid;

	/*
	 * RFFDG duplicates the parent's fd table into the child.
	 * child_fds[0/1/2] are the pipe ends we want on stdin/stdout/stderr.
	 * close_fds[0/1] are the parent-side pipe ends (stdin_pipe[1] and
	 * out_pipe[0]) that the child inherited but must not hold open —
	 * if either stays open in the child, the corresponding pipe never
	 * gets EOF and the parent deadlocks.
	 *
	 * We do NOT use RFNOWAIT: we need the kernel to queue the exit
	 * notification so waitchild() can collect the exit message and
	 * exit code.  Since our child is the only non-libthread waitable
	 * process, calling wait() directly in waitchild() is safe.
	 *
	 * The child signals exec failure via the sentinel exit string
	 * "__exec_failed__"; execrun() detects this and returns nil.
	 */
	pid = rfork(RFPROC|RFFDG);
	if(pid < 0) {
		werrstr("spawnchild: rfork: %r");
		return -1;
	}
	if(pid == 0) {
		/* child: close the parent-side pipe ends we must not keep */
		close(close_fds[0]);
		close(close_fds[1]);
		/* dup child pipe ends into 0/1/2 */
		dup(child_fds[0], 0);
		dup(child_fds[1], 1);
		dup(child_fds[2], 2);
		if(child_fds[0] > 2) close(child_fds[0]);
		if(child_fds[1] > 2) close(child_fds[1]);
		if(child_fds[2] > 2 && child_fds[2] != child_fds[1]) close(child_fds[2]);
		execpath(prog, argv);
		_exits("__exec_failed__");
	}
	/* parent: close the child-side pipe ends we handed over */
	close(child_fds[0]);
	close(child_fds[1]);
	if(child_fds[2] != child_fds[1])
		close(child_fds[2]);
	return pid;
}

static Waitmsg *
waitchild(int pid)
{
	Waitmsg *w;

	for(;;) {
		w = wait();
		if(w == nil)
			return nil;
		if(w->pid == pid)
			return w;
		free(w);
	}
}

static Waitmsg *
waitpolled(void)
{
	return nbrecvp(threadwaitchan());
}

#endif

/* ── execparse ──────────────────────────────────────────────────────── */

int
execparse(const char *args_json, int args_len,
          char *argv[], int maxargv, char **stdin_out)
{
	enum { MAXTOK = 512 };
	jsmn_parser  p;
	jsmntok_t   *toks;
	int          ntoks;
	int          argv_i, stdin_i;
	int          argc, elem, i;
	char        *buf;

	/*
	 * Heap-allocate toks[] and buf to avoid overflowing the small
	 * (8 KB default) libthread stack: toks[512] alone is 8 KB on
	 * amd64, and buf adds another 4 KB on top.
	 */
	toks = malloc(MAXTOK * sizeof(jsmntok_t));
	if(toks == nil) {
		werrstr("exec: malloc toks: %r");
		return -1;
	}
	buf = malloc(EXEC_MAXARG);
	if(buf == nil) {
		free(toks);
		werrstr("exec: malloc buf: %r");
		return -1;
	}

	jsmn_init(&p);
	ntoks = jsmn_parse(&p, args_json, args_len, toks, MAXTOK);
	if(ntoks < 1 || toks[0].type != JSMN_OBJECT) {
		werrstr("exec: bad tool args JSON (ntoks=%d)", ntoks);
		free(toks); free(buf);
		return -1;
	}

	/* find "argv" array */
	argv_i = jsonget(args_json, toks, ntoks, 0, "argv");
	if(argv_i < 0 || toks[argv_i].type != JSMN_ARRAY) {
		werrstr("exec: tool call is missing the required 'argv' field. "
		        "Expected: {\"argv\":[\"program\",\"arg1\",...],\"stdin\":\"optional\"}");
		free(toks); free(buf);
		return -1;
	}
	argc = toks[argv_i].size;
	if(argc <= 0) {
		werrstr("exec: 'argv' must be a non-empty array. "
		        "Expected: {\"argv\":[\"program\",\"arg1\",...],\"stdin\":\"optional\"}");
		free(toks); free(buf);
		return -1;
	}
	if(argc >= maxargv - 1) {
		werrstr("exec: too many argv elements (%d)", argc);
		free(toks); free(buf);
		return -1;
	}

	/* copy each argv element */
	elem = argv_i + 1;
	for(i = 0; i < argc; i++) {
		if(toks[elem].type != JSMN_STRING) {
			werrstr("exec: argv[%d] is not a string", i);
			while(--i >= 0) free(argv[i]);
			free(toks); free(buf);
			return -1;
		}
		if(jsonstr(args_json, &toks[elem], buf, EXEC_MAXARG) < 0) {
			werrstr("exec: argv[%d] too long or decode error", i);
			while(--i >= 0) free(argv[i]);
			free(toks); free(buf);
			return -1;
		}
		argv[i] = strdup(buf);
		if(argv[i] == nil) {
			werrstr("exec: strdup argv[%d]: %r", i);
			while(--i >= 0) free(argv[i]);
			free(toks); free(buf);
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
			free(toks); free(buf);
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

	free(toks);
	free(buf);
	return argc;
}

/* ── collectoutput ──────────────────────────────────────────────────── */

/*
 * collectoutput — drain fd into a ring buffer capped at cap bytes.
 *
 * Uses a ring buffer: once the buffer is full, new bytes overwrite the
 * oldest (wpos wraps to 0).  At EOF the ring is linearised so the result
 * is the most recent cap bytes, with "[...truncated...]\n" prepended to
 * mark the loss.  Keeping the tail is preferable to the head: the end of
 * tool output is where conclusions, error messages, and final state live.
 *
 * Returns a malloc'd nil-terminated string.
 * Sets *lenp and *truncated.  Returns nil only on allocation failure.
 */
static char *
collectoutput(int fd, long cap, long *lenp, int *truncated)
{
	static const char trunc_marker[] = "[...truncated...]\n";
	enum { TRUNC_LEN = sizeof(trunc_marker) - 1 };
	enum { CHUNK = 8192 };

	char *buf;
	long  wpos;     /* next write position in ring */
	long  total;    /* total bytes received */
	char  tmp[CHUNK];
	long  n;

	*truncated = 0;
	*lenp      = 0;

	buf = malloc(cap + 1);
	if(buf == nil)
		return nil;

	wpos  = 0;
	total = 0;
	for(;;) {
		n = read(fd, tmp, sizeof tmp);
		if(n <= 0)
			break;
		total += n;
		/* write tmp[0..n) into the ring */
		{
			long i;
			for(i = 0; i < n; i++) {
				buf[wpos] = tmp[i];
				wpos++;
				if(wpos >= cap)
					wpos = 0;
			}
		}
	}

	if(total <= cap) {
		/* no wrap: buf[0..total) is the complete output */
		buf[total] = '\0';
		*lenp = total;
		return buf;
	}

	/*
	 * Ring wrapped: buf[wpos..cap) holds the oldest bytes,
	 * buf[0..wpos) holds the newest.  Linearise into a fresh buffer
	 * with the truncation marker prepended.
	 */
	*truncated = 1;
	{
		char *out;
		long  marklen, taillen, headlen, outlen;

		marklen = TRUNC_LEN;
		taillen = cap - wpos;   /* bytes from wpos to end of ring   */
		headlen = wpos;         /* bytes from start of ring to wpos */
		outlen  = marklen + taillen + headlen;  /* == marklen + cap */

		out = malloc(outlen + 1);
		if(out == nil) {
			free(buf);
			return nil;
		}
		memmove(out,                   trunc_marker, marklen);
		memmove(out + marklen,         buf + wpos,   taillen);
		memmove(out + marklen + taillen, buf,         headlen);
		out[outlen] = '\0';
		free(buf);
		*lenp = outlen;
		return out;
	}
}

/* ── execrun ────────────────────────────────────────────────────────── */

ExecResult *
execrun(const char *args_json, int args_len, long maxout)
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

	/* Hand child-side fds to spawnchild.
	 * On plan9port threadspawn closes child_fds[0..2] in the parent;
	 * on 9front spawnchild does it explicitly after rfork. */
	child_fds[0] = stdin_pipe[0];
	child_fds[1] = out_pipe[1];
	child_fds[2] = out_pipe[1];   /* merge stderr into stdout */

	{
		int close_fds[2];
		close_fds[0] = stdin_pipe[1];  /* parent write end — child must not keep */
		close_fds[1] = out_pipe[0];    /* parent read end  — child must not keep */
		pid = spawnchild(child_fds, close_fds, argv[0], argv);
	}
	if(pid < 0) {
		werrstr("exec: spawnchild %s: %r", argv[0]);
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

#ifdef PLAN9PORT
	/* write stdin then close — plan9port pipes are large enough or
	 * threadspawn runs the child in a real thread so no deadlock. */
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
#else
	/*
	 * On 9front, writing and reading must be concurrent: if stdin is
	 * larger than the pipe buffer, the parent blocks writing while the
	 * child blocks writing output — deadlock.
	 *
	 * Spawn a writer proc with rfork(RFPROC|RFFDG|RFNOWAIT).
	 * RFFDG gives the child a separate fd table (CoW copy of the address
	 * space) so it can read stdin_str directly without needing RFMEM.
	 * We do NOT use RFMEM: that would share libthread's internal state
	 * (privalloc'd Proc*, note handlers, etc.) causing corruption.
	 * RFNOWAIT makes the kernel discard the writer's exit status instead
	 * of queuing it — we never need to call wait() for it, which avoids
	 * a race where the writer exits before the main child and its
	 * Waitmsg is consumed inside the main waitchild() loop, causing a
	 * subsequent wait() for the writer to block forever.
	 *
	 * The writer closes out_pipe[0] (it must not hold the output pipe's
	 * read end open), writes stdin_str to stdin_pipe[1], then exits.
	 */
	if(rfork(RFPROC|RFFDG|RFNOWAIT) == 0) {
		/* writer child: close out_pipe[0] so we don't hold it open */
		close(out_pipe[0]);
		if(stdin_str[0] != '\0') {
			long slen    = strlen(stdin_str);
			long written = 0, n;
			while(written < slen) {
				n = write(stdin_pipe[1],
				          stdin_str + written,
				          slen - written);
				if(n <= 0)
					break;
				written += n;
			}
		}
		close(stdin_pipe[1]);
		_exits("");
	}
	/* parent: close our copy of stdin_pipe[1] now that the writer owns it */
	close(stdin_pipe[1]);
#endif

	/* allocate result */
	r = mallocz(sizeof *r, 1);
	if(r == nil) {
		werrstr("exec: mallocz result: %r");
		close(out_pipe[0]);
		goto err_free_argv;
	}
	r->pid = pid;

	/* collect combined output */
	r->output = collectoutput(out_pipe[0], maxout, &r->outputlen, &r->truncated);
	close(out_pipe[0]);

	if(r->output == nil) {
		werrstr("exec: collectoutput: %r");
		execresultfree(r);
		goto err_free_argv;
	}

	/* wait for child */
	w = waitchild(pid);
	if(w != nil) {
		/*
		 * On 9front, the kernel formats Waitmsg.msg as:
		 *   ""                            → success (exitcode 0)
		 *   "<progname> <pid>: <exitstr>" → non-zero exit
		 *
		 * So our sentinel "__exec_failed__" appears as a substring,
		 * not the full msg — use strstr, not strcmp.
		 * For ordinary failures we scan backward for trailing digits
		 * to extract the exit code.
		 *
		 * On plan9port, msg looks like "exit status N" for non-zero exits.
		 * The trailing-number scan handles both formats.
		 */
		if(w->msg != nil && w->msg[0] != '\0') {
			if(strstr(w->msg, "__exec_failed__") != nil) {
				werrstr("exec: command not found: %s", argv[0]);
				free(w);
				execresultfree(r);
				for(i = 0; i < argc; i++)
					free(argv[i]);
				free(stdin_str);
				return nil;
			}
			/* scan backward for trailing decimal digits */
			{
				char *p = w->msg + strlen(w->msg);
				while(p > w->msg && p[-1] >= '0' && p[-1] <= '9')
					p--;
				if(*p >= '1' && *p <= '9')
					r->exitcode = atoi(p);
				else
					r->exitcode = 1;
			}
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
		w = waitpolled();
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
execresultstr(ExecResult *r, char *buf, long n)
{
	long outlen;

	if(r == nil || buf == nil || n <= 0) {
		if(buf && n > 0) buf[0] = '\0';
		return buf;
	}

	/*
	 * Reserve 32 bytes for "\nexited N\0" suffix.
	 * Copy output up to that limit.
	 */
	outlen = r->outputlen;
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
