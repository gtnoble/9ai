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
 */

#include <u.h>
#include <libc.h>
#include <bio.h>

#include "http.h"
#include "sse.h"

void
sseinit(SSEParser *p, HTTPResp *resp)
{
	memset(p, 0, sizeof *p);
	p->resp = resp;
}

int
ssestep(SSEParser *p, SSEEvent *ev)
{
	char *line;
	int   n;

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
			n = strlen(line + 7);
			if(n >= (int)sizeof p->evbuf)
				n = (int)sizeof p->evbuf - 1;
			memmove(p->evbuf, line + 7, n);
			p->evbuf[n] = '\0';
			p->have_event = 1;
			continue;
		}

		if(strncmp(line, "data: ", 6) == 0) {
			char *payload = line + 6;

			if(strcmp(payload, "[DONE]") == 0)
				return SSE_DONE;

			/* copy payload into databuf */
			n = strlen(payload);
			if(n >= SSE_BUFSZ)
				n = SSE_BUFSZ - 1;
			memmove(p->databuf, payload, n);
			p->databuf[n] = '\0';

			ev->data  = p->databuf;
			ev->event = p->have_event ? p->evbuf : nil;

			/* reset for next event; leave evbuf contents intact —
			 * caller may read ev->event until the next ssestep() call */
			p->have_event = 0;

			return SSE_OK;
		}

		/* comment (":...") or unknown field — skip */
	}
}
