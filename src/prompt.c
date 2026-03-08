/*
 * prompt.c — prompt text extraction for 9ai-acme
 *
 * prompttext_body() mirrors pi-acme's last_paragraph():
 *   1. Skip the first line (status line).
 *   2. Split the remainder on blank lines into paragraphs.
 *   3. Scan paragraphs in reverse; return the last one that is
 *      non-empty, not a separator (═…), and doesn't start with ▶ or ↩.
 *
 * ▶ U+25B6  UTF-8: e2 96 b6
 * ↩ U+21A9  UTF-8: e2 86 a9
 * ═ U+2550  UTF-8: e2 95 90
 */

#include <u.h>
#include <libc.h>
#include "prompt.h"

char *
prompttext_body(char *body)
{
	char  *start;
	char  *result;
	int    len, i, np, pi2;
	int    b;
	char  *ps, *ts, *te;
	int    plen, tlen, is_sep;
	char  *sp;
	uchar  a, b2, c;

	enum { MAXPARA = 512 };
	int pb[MAXPARA], pe[MAXPARA];

	if(body == nil || *body == '\0')
		return nil;

	/* skip status line (first \n) */
	start = strchr(body, '\n');
	start = start ? start + 1 : body;

	/* collect paragraph [begin,end) pairs by scanning for \n\n boundaries */
	len = strlen(start);
	np = 0;
	i = 0;
	while(i <= len && np < MAXPARA) {
		/* skip leading blank lines */
		while(i < len && start[i] == '\n') i++;
		if(i >= len) break;
		b = i;
		/* advance until \n\n or end */
		while(i < len) {
			if(start[i] == '\n' && (i+1 >= len || start[i+1] == '\n'))
				break;
			i++;
		}
		pb[np] = b;
		pe[np] = i;
		np++;
	}

	/* scan paragraphs in reverse for the last usable one */
	result = nil;
	for(pi2 = np - 1; pi2 >= 0; pi2--) {
		ps   = start + pb[pi2];
		plen = pe[pi2] - pb[pi2];

		/* trim leading/trailing whitespace */
		ts = ps;
		te = ps + plen;
		while(ts < te && (*ts == ' ' || *ts == '\t' || *ts == '\n' || *ts == '\r')) ts++;
		while(te > ts && (*(te-1) == ' ' || *(te-1) == '\t' || *(te-1) == '\n' || *(te-1) == '\r')) te--;
		tlen = te - ts;
		if(tlen <= 0) continue;

		/* skip separator lines: all ═ (0xe2 0x95 0x90) and whitespace */
		is_sep = 1;
		for(sp = ts; sp < te; ) {
			if((uchar)sp[0] == 0xe2 && sp+2 < te &&
			   (uchar)sp[1] == 0x95 && (uchar)sp[2] == 0x90) {
				sp += 3;
			} else if(*sp == ' ' || *sp == '\t' || *sp == '\n') {
				sp++;
			} else {
				is_sep = 0; break;
			}
		}
		if(is_sep) continue;

		/* skip echo (▶) and steer (↩) lines */
		if(tlen >= 3) {
			a  = (uchar)ts[0];
			b2 = (uchar)ts[1];
			c  = (uchar)ts[2];
			if(a == 0xe2 && b2 == 0x96 && c == 0xb6) continue; /* ▶ */
			if(a == 0xe2 && b2 == 0x86 && c == 0xa9) continue; /* ↩ */
		}

		result = mallocz(tlen + 1, 1);
		memmove(result, ts, tlen);
		result[tlen] = '\0';
		break;
	}

	return result;
}
