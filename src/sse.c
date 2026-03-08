/*
 * sse.c — Server-Sent Events line parser
 *
 * See sse.h for the interface and usage.
 *
 * Wire format observed from the Copilot API:
 *
 *   OpenAI Completions (/chat/completions):
 *     data: <JSON>\r\n   (or \n only)
 *     \r\n
 *     ...
 *     data: [DONE]\r\n
 *     \r\n
 *
 *   Anthropic Messages (/v1/messages):
 *     event: <name>\r\n
 *     data: <JSON>\r\n
 *     \r\n
 *     ...
 *     data: [DONE]\r\n
 *     \r\n
 *
 * httpreadline() already strips trailing \r\n, so we receive clean
 * lines with no line-ending characters.
 *
 * Parser state machine (per ssestep call):
 *   Loop reading lines until a "data:" line is found:
 *     blank line          → skip (event separator)
 *     "event: <name>"     → save name in p->evbuf, set have_event
 *     "data: [DONE]"      → return SSE_DONE
 *     "data: <payload>"   → copy payload to p->databuf, fill *ev, return SSE_OK
 *     nil (EOF)           → return SSE_EOF
 *     anything else       → skip (comments, unknown fields)
 *   After delivering a data line: clear have_event for the next event.
 *
 * Buffers:
 *   evbuf and databuf are heap-allocated and grow as needed.
 *   Initial size is SSE_INITBUFSZ; doubled on each resize.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>

#include "http.h"
#include "sse.h"

enum {
	SSE_INITBUFSZ = 256,
};

/*
 * growbuf — ensure *buf has room for at least need bytes (including NUL).
 * Returns 0 on success, -1 on allocation failure.
 */
static int
growbuf(char **buf, long *sz, long need)
{
	long newsz;
	char *p;

	if(need <= *sz)
		return 0;
	newsz = *sz;
	if(newsz == 0)
		newsz = SSE_INITBUFSZ;
	while(newsz < need)
		newsz *= 2;
	p = realloc(*buf, newsz);
	if(p == nil)
		return -1;
	*buf = p;
	*sz  = newsz;
	return 0;
}

void
sseinit(SSEParser *p, HTTPResp *resp)
{
	memset(p, 0, sizeof *p);
	p->resp = resp;
}

void
sseterm(SSEParser *p)
{
	free(p->evbuf);
	free(p->databuf);
	p->evbuf   = nil;
	p->databuf = nil;
	p->evbufsz   = 0;
	p->databufsz = 0;
}

int
ssestep(SSEParser *p, SSEEvent *ev)
{
	char *line, *payload;
	long  n;

	for(;;) {
		line = httpreadline(p->resp);
		if(line == nil)
			return SSE_EOF;

		if(line[0] == '\0') {
			/* blank line — event separator; skip */
			continue;
		}

		if(strncmp(line, "event: ", 7) == 0) {
			/* save event name; data line follows */
			payload = line + 7;
			n = strlen(payload) + 1;
			if(growbuf(&p->evbuf, &p->evbufsz, n) < 0)
				return SSE_EOF;
			memmove(p->evbuf, payload, n);
			p->have_event = 1;
			continue;
		}

		if(strncmp(line, "data: ", 6) == 0) {
			payload = line + 6;

			if(strcmp(payload, "[DONE]") == 0)
				return SSE_DONE;

			n = strlen(payload) + 1;
			if(growbuf(&p->databuf, &p->databufsz, n) < 0)
				return SSE_EOF;
			memmove(p->databuf, payload, n);

			ev->data  = p->databuf;
			ev->event = p->have_event ? p->evbuf : nil;

			/* reset for next event; evbuf contents remain valid
			 * until the next ssestep() or sseterm() call */
			p->have_event = 0;

			return SSE_OK;
		}

		/* comment (":...") or unknown field — skip */
	}
}
