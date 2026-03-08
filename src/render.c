/*
 * render.c — format 9ai event records for display in the acme window
 */

#include <u.h>
#include <libc.h>
#include "render.h"

/* ── tool_start ── */

char *
render_tool_start(char **fields, int nf)
{
	char args[512];
	int  i, alen = 0;
	int  slen;

	args[0] = '\0';
	for(i = 3; i < nf; i++) {
		if(i > 3 && alen + 1 < (int)sizeof args - 1)
			args[alen++] = ' ';
		slen = strlen(fields[i]);
		if(alen + slen < (int)sizeof args - 1) {
			memmove(args + alen, fields[i], slen);
			alen += slen;
			args[alen] = '\0';
		}
	}
	if(alen > 200) {
		memmove(args + 197, "...", 4);
		alen = 200;
	}

	return smprint("\n\xe2\x94\x8c \xe2\x9a\x99 %s %s\n",
	               nf > 1 ? fields[1] : "?",
	               args);
}

/* ── tool_end ── */

char *
render_tool_end(char **fields, int nf)
{
	char *out;
	char  first[80];
	int   i;
	int   is_err;

	is_err = (nf > 1 && strcmp(fields[1], "err") == 0);
	if(is_err) {
		out = (nf > 2 && fields[2][0]) ? fields[2] : "";
		for(i = 0; i < 79 && out[i] && out[i] != '\n'; i++)
			first[i] = out[i];
		first[i] = '\0';
		return smprint("\xe2\x94\x94 \xe2\x9c\x97 %s\n\n", first);
	}
	return strdup("\xe2\x94\x94 \xe2\x9c\x93\n\n");
}

/* ── thinking ── */

/*
 * render_thinking — prefix each line of chunk with "│ " (U+2502 + space).
 * An initial "\n│ " is prepended so the block starts on a new line.
 */
char *
render_thinking(char *chunk)
{
	/*
	 * Worst case: every byte of chunk is '\n', each replaced by
	 * '\n' + 3 bytes (│) + ' ' = 4 bytes output per input byte.
	 * Add 5 for the leading "\n│ " and the NUL.
	 *
	 * BAR = UTF-8 for │ (U+2502): e2 94 82 (3 bytes)
	 */
	int   inlen = strlen(chunk);
	int   outsz = 5 + inlen * 4 + 1;
	int   i;
	char *out   = malloc(outsz);
	int   olen  = 0;

	if(out == nil)
		return nil;

	/* leading prefix */
	out[olen++] = '\n';
	out[olen++] = (char)0xe2; out[olen++] = (char)0x94; out[olen++] = (char)0x82;
	out[olen++] = ' ';

	for(i = 0; chunk[i]; i++) {
		out[olen++] = chunk[i];
		if(chunk[i] == '\n' && chunk[i+1] != '\0') {
			out[olen++] = (char)0xe2;
			out[olen++] = (char)0x94;
			out[olen++] = (char)0x82;
			out[olen++] = ' ';
		}
	}
	out[olen] = '\0';
	return out;
}
