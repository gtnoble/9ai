/*
 * render.c — format 9ai event records for display in the acme window
 */

#include <u.h>
#include <libc.h>
#include "render.h"

/* ── tool_start ── */

/*
 * render_tool_start — format a tool_start record for display.
 *
 * Record layout (after splitrec):
 *   fields[0] = "tool_start"
 *   fields[1] = tool name
 *   fields[2] = tool id (unused in display)
 *   fields[3..nf-2] = argv elements
 *   fields[nf-1] = stdin (empty string if none; absent in old records)
 *
 * Output format:
 *
 *   ┌ ⚙ cmd arg1 arg2 …
 *   │ stdin line 1        ← only when stdin is non-empty
 *   │ stdin line 2
 *
 * The trailing newline after the last stdin line is the separator before
 * the tool_end line; render_tool_end always starts with └.
 */
char *
render_tool_start(char **fields, int nf)
{
	/*
	 * Determine the argv range and whether a stdin field is present.
	 *
	 * New records: fields[3..nf-2] = argv, fields[nf-1] = stdin.
	 * Old records (no stdin field): fields[3..nf-1] = argv.
	 *
	 * We treat nf >= 4 as "new" (has stdin) and nf < 4 as "old"
	 * (no argv at all, so certainly no stdin field either).
	 * When nf == 3 there is no argv and no stdin; nf >= 4 means the
	 * last field is stdin and fields[3..nf-2] are argv.
	 */
	int   argv_first = 3;
	int   argv_last;   /* inclusive */
	char *stdin_str;
	int   has_stdin;

	if(nf >= 4) {
		argv_last  = nf - 2;   /* fields[3..nf-2] */
		stdin_str  = fields[nf - 1];
		has_stdin  = (stdin_str != nil && stdin_str[0] != '\0');
	} else {
		argv_last  = nf - 1;   /* fields[3..nf-1] (may be < argv_first) */
		stdin_str  = nil;
		has_stdin  = 0;
	}

	/* ── build "cmd arg1 arg2 …" string ── */
	{
		int    i;
		long   cmdlen = 0;
		char  *cmd;
		char  *p;

		/* measure */
		for(i = argv_first; i <= argv_last; i++) {
			if(i > argv_first) cmdlen++;   /* space */
			cmdlen += strlen(fields[i]);
		}

		cmd = malloc(cmdlen + 1);
		if(cmd == nil)
			return nil;

		p = cmd;
		for(i = argv_first; i <= argv_last; i++) {
			int slen = strlen(fields[i]);
			if(i > argv_first) *p++ = ' ';
			memmove(p, fields[i], slen);
			p += slen;
		}
		*p = '\0';

		/* ── no stdin: single header line ── */
		if(!has_stdin) {
			char *out = smprint("\n\xe2\x94\x8c \xe2\x9a\x99 %s %s\n",
			                    nf > 1 ? fields[1] : "?", cmd);
			free(cmd);
			return out;
		}

		/* ── with stdin: header + prefixed stdin lines ── */
		{
			/*
			 * Prefix each line of stdin_str with "│ " (U+2502 + space).
			 * Last line gets a trailing newline even if stdin_str lacks one.
			 *
			 * BAR = UTF-8 for │ (U+2502): e2 94 82 (3 bytes)
			 * Worst case: every byte is '\n' → 4 bytes out per byte in.
			 * Add room for header line and leading/trailing newlines.
			 */
			char *tool = nf > 1 ? fields[1] : "?";
			int   sinlen  = strlen(stdin_str);
			int   stdinbuf_sz = sinlen * 4 + 8;
			char *hdr;
			char *stdinbuf;
			char *out;
			int   olen, j;

			hdr = smprint("\n\xe2\x94\x8c \xe2\x9a\x99 %s %s\n", tool, cmd);
			free(cmd);
			if(hdr == nil)
				return nil;

			stdinbuf = malloc(stdinbuf_sz);
			if(stdinbuf == nil) {
				free(hdr);
				return nil;
			}

			olen = 0;
			/* first line prefix */
			stdinbuf[olen++] = (char)0xe2;
			stdinbuf[olen++] = (char)0x94;
			stdinbuf[olen++] = (char)0x82;
			stdinbuf[olen++] = ' ';
			for(j = 0; stdin_str[j]; j++) {
				stdinbuf[olen++] = stdin_str[j];
				if(stdin_str[j] == '\n' && stdin_str[j+1] != '\0') {
					stdinbuf[olen++] = (char)0xe2;
					stdinbuf[olen++] = (char)0x94;
					stdinbuf[olen++] = (char)0x82;
					stdinbuf[olen++] = ' ';
				}
			}
			/* ensure trailing newline */
			if(olen == 0 || stdinbuf[olen-1] != '\n')
				stdinbuf[olen++] = '\n';
			stdinbuf[olen] = '\0';

			out = smprint("%s%s", hdr, stdinbuf);
			free(hdr);
			free(stdinbuf);
			return out;
		}
	}
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
