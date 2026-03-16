/*
 * exec.h — exec tool for 9ai
 *
 * Implements the single tool available to the LLM: direct program execution
 * with no shell involvement.  Input arrives as an accumulated JSON string
 * (the concatenation of all OAIDToolArg chunks for one tool call).  Output
 * is collected into a capped buffer and returned as a plain string.
 *
 * ── Tool input format ─────────────────────────────────────────────────
 *
 *   {"argv":["program","arg1","arg2"],"stdin":"optional text","timeout":30}
 *
 * "argv" is required; "stdin" and "timeout" are optional.
 * "timeout" is an integer number of seconds; defaults to
 * EXEC_DEFAULT_TIMEOUT_S (30) when absent or zero.
 * Arguments must be complete strings — no shell expansion, no glob.
 *
 * ── Usage ─────────────────────────────────────────────────────────────
 *
 *   ExecResult *r = execrun(args_json, args_json_len, maxout);
 *   if(r == nil) {
 *       // parse error or exec failure — check errstr
 *   } else {
 *       // r->output: combined stdout+stderr (nil-terminated)
 *       // r->exitcode: process exit status (0 = success)
 *       // r->truncated: 1 if output was capped at maxout
 *   }
 *   execresultfree(r);
 *
 * ── Output cap ────────────────────────────────────────────────────────
 *
 * stdout and stderr are merged and capped at maxout bytes.  On
 * overflow the tail is kept: the oldest data is discarded and
 * "[...truncated...]\n" is prepended to the retained tail.
 *
 * ── Abort ─────────────────────────────────────────────────────────────
 *
 * execabort(pid) sends SIGTERM to the process, then SIGKILL after 2s
 * if it has not exited.  Safe to call from any thread after execrun()
 * has returned the pid via r->pid.  (The agent loop calls execabort
 * when it receives an abort request on abortchan while a tool is running.)
 *
 * ── Tool result string ────────────────────────────────────────────────
 *
 * execresultstr(r, buf, n) formats the tool result into buf:
 *   - output text (up to n-64 bytes)
 *   - if exit code != 0: appends "\nexited <N>"
 * Returns buf.
 */

enum {
	EXEC_MAXARGV            = 256,  /* max argv elements           */
	EXEC_MAXARG             = 4096, /* max single argument length  */
	EXEC_DEFAULT_TIMEOUT_S  = 30,   /* default tool timeout (seconds) */
};

/*
 * ExecOpts — optional per-call settings for execrun().
 *
 * unmount_mtpt: if non-nil, unmount this path from the child's namespace
 *               before exec.  On 9front this uses rfork(RFNAMEG) to give
 *               the child a private namespace copy, then unmount(nil, path).
 *               On plan9port/Linux this uses unshare(CLONE_NEWNS) followed
 *               by umount2(path, MNT_DETACH) so the parent's namespace is
 *               unaffected.
 *               Use this to remove the 9ai file system from the exec
 *               child's view for security — prevents the tool from accessing
 *               /message, /ctl, etc.
 *
 * timeout_ms:   wall-clock timeout in milliseconds.  0 means use the default
 *               (EXEC_DEFAULT_TIMEOUT_S * 1000).  If the child has not exited
 *               within this window, execabort() is called automatically and
 *               ExecResult.timed_out is set to 1.
 */
typedef struct ExecOpts ExecOpts;

struct ExecOpts {
	char *unmount_mtpt;   /* nil → no unmount; non-nil → path to unmount */
	long  timeout_ms;     /* 0 → use default (EXEC_DEFAULT_TIMEOUT_S*1000) */
};

typedef struct ExecResult ExecResult;

struct ExecResult {
	int    pid;        /* process id (set before output collection)  */
	int    exitcode;   /* exit status; 0 on success                  */
	char  *output;     /* combined stdout+stderr, nil-terminated      */
	long   outputlen;  /* length of output in bytes                   */
	int    truncated;  /* 1 if output was capped at maxout            */
	int    timed_out;  /* 1 if the child was killed by the timeout    */
};

/*
 * execparse — parse tool-call JSON into argv[], stdin string, and timeout.
 *
 * args_json: accumulated input_json_delta string (the full JSON object).
 * args_len:  byte length of args_json.
 *
 * On success: argv[] is populated (nil-terminated), *stdin_out points
 * to a malloc'd copy of the stdin string (may be empty), *timeout_s_out
 * is set to the "timeout" integer from JSON (0 if absent), returns the
 * argc count.  Caller must free argv[0..argc-1] and *stdin_out.
 *
 * On error: returns -1, sets errstr.
 */
int execparse(const char *args_json, int args_len,
              char *argv[], int maxargv, char **stdin_out, int *timeout_s_out);

/*
 * execrun — parse args_json and run the program.
 *
 * maxout: output cap in bytes; the tail of output is kept on overflow.
 * opts:   optional settings (may be nil for defaults).
 *         opts->unmount_mtpt: if non-nil, unmount this path from the
 *         child's namespace before exec.  See ExecOpts above.
 *
 * Returns a heap-allocated ExecResult on success or failure (even if
 * the program exits non-zero).  Returns nil only on internal error
 * (parse failure, pipe creation failure, etc.) or if the program could
 * not be found/executed, in which case errstr is set to
 * "exec: command not found: <argv[0]>" or similar.
 *
 * Caller must free with execresultfree().
 */
ExecResult *execrun(const char *args_json, int args_len, long maxout,
                    ExecOpts *opts);

/*
 * execabort — terminate a running exec child.
 *
 * Sends SIGTERM; if the process has not exited within 2 seconds,
 * sends SIGKILL.  Safe to call from any thread.
 * pid must be the value from ExecResult.pid.
 */
void execabort(int pid);

/*
 * execresultstr — format result into buf for use as a tool result.
 *
 * Writes output text, then appends "\nexited N" if exitcode != 0.
 * buf is nil-terminated.  Returns buf.
 */
char *execresultstr(ExecResult *r, char *buf, long n);

/*
 * execresultfree — free an ExecResult.
 */
void execresultfree(ExecResult *r);
