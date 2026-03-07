/*
 * sse.h — Server-Sent Events line parser for 9ai
 *
 * Reads an SSE stream from an open HTTPResp using httpreadline() and
 * delivers one (event, data) pair per call to ssestep().
 *
 * Handles both wire formats used by the Copilot API:
 *
 *   OpenAI Completions — no "event:" lines; each event is just:
 *     data: <JSON>\n
 *     \n
 *
 *   Anthropic Messages — event/data pairs:
 *     event: <name>\n
 *     data: <JSON>\n
 *     \n
 *
 * The final token in both formats is:
 *     data: [DONE]\n
 *     \n
 *
 * Usage:
 *
 *   SSEParser p;
 *   sseinit(&p, resp);
 *
 *   SSEEvent ev;
 *   int rc;
 *   while((rc = ssestep(&p, &ev)) == SSE_OK) {
 *       // ev.event is nil for OAI (no event: line), or e.g. "content_block_delta"
 *       // ev.data  is the JSON payload (pointer into internal buffer)
 *       // ev.event and ev.data are valid only until the next ssestep() call
 *       process(ev.event, ev.data);
 *   }
 *   // rc == SSE_DONE: [DONE] received — normal end of stream
 *   // rc == SSE_EOF:  connection closed without [DONE] (error/abort)
 *
 * Memory:
 *   SSEParser contains a fixed internal buffer (SSE_BUFSZ bytes) that
 *   holds the most recent event and data strings.  Callers must copy
 *   any strings they need to retain across calls to ssestep().
 *   SSEParser itself holds no heap allocations — it may be stack-allocated.
 */

enum {
	SSE_OK   = 0,   /* ev filled; call again                */
	SSE_DONE = 1,   /* [DONE] received; stream finished     */
	SSE_EOF  = 2,   /* EOF or read error before [DONE]      */

	SSE_BUFSZ = 65536,  /* max line length; Copilot data lines are < 4KB in practice */
};

typedef struct SSEParser SSEParser;
typedef struct SSEEvent  SSEEvent;

struct SSEEvent {
	char *event;   /* event name, or nil if no "event:" line preceded this data */
	char *data;    /* data payload (never nil when rc == SSE_OK) */
};

struct SSEParser {
	HTTPResp *resp;
	char      evbuf[128];           /* current event name from "event:" line */
	char      databuf[SSE_BUFSZ];   /* current data payload */
	int       have_event;           /* evbuf contains a pending event name */
};

/* sse.c */
void sseinit(SSEParser *p, HTTPResp *resp);
int  ssestep(SSEParser *p, SSEEvent *ev);
