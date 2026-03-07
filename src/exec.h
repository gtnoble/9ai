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
 *   {"argv":["program","arg1","arg2"],"stdin":"optional text"}
 *
 * "argv" is required; "stdin" is optional (empty string if absent).
 * Arguments must be complete strings — no shell expansion, no glob.
 *
 * ── Usage ─────────────────────────────────────────────────────────────
 *
 *   ExecResult *r = execrun(args_json, args_json_len);
 *   if(r == nil) {
 *       // parse error or exec failure — check errstr
 *   } else {
 *       // r->output: combined stdout+stderr (nil-terminated)
 *       // r->exitcode: process exit status (0 = success)
 *       // r->truncated: 1 if output was capped at EXEC_MAXOUT
 *   }
 *   execresultfree(r);
 *
 * ── Output cap ────────────────────────────────────────────────────────
 *
 * stdout and stderr are merged and capped at EXEC_MAXOUT bytes.  On
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
	EXEC_MAXOUT  = 512 * 1024,  /* 512KB output cap            */
	EXEC_MAXARGV = 256,         /* max argv elements           */
	EXEC_MAXARG  = 4096,        /* max single argument length  */
};

typedef struct ExecResult ExecResult;

struct ExecResult {
	int    pid;        /* process id (set before output collection)  */
	int    exitcode;   /* exit status; 0 on success                  */
	char  *output;     /* combined stdout+stderr, nil-terminated      */
	long   outputlen;  /* length of output in bytes                   */
	int    truncated;  /* 1 if output was capped at EXEC_MAXOUT       */
};

/*
 * execparse — parse tool-call JSON into argv[] and stdin string.
 *
 * args_json: accumulated input_json_delta string (the full JSON object).
 * args_len:  byte length of args_json.
 *
 * On success: argv[] is populated (nil-terminated), *stdin_out points
 * to a malloc'd copy of the stdin string (may be empty), returns the
 * argc count.  Caller must free argv[0..argc-1] and *stdin_out.
 *
 * On error: returns -1, sets errstr.
 */
int execparse(const char *args_json, int args_len,
              char *argv[], int maxargv, char **stdin_out);

/*
 * execrun — parse args_json and run the program.
 *
 * Returns a heap-allocated ExecResult on success or failure (even if
 * the program exits non-zero).  Returns nil only on internal error
 * (parse failure, pipe creation failure, etc.).
 *
 * Caller must free with execresultfree().
 */
ExecResult *execrun(const char *args_json, int args_len);

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
char *execresultstr(ExecResult *r, char *buf, int n);

/*
 * execresultfree — free an ExecResult.
 */
void execresultfree(ExecResult *r);
