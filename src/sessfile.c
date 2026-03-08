/*
 * sessfile.c — session file parser for 9ai-acme Sessions window
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include "record.h"
#include "sessfile.h"

int
parsesessfile(char *path, char *uuid, char *model, char *ts, char *snippet)
{
	Biobuf *b;
	char   *line;
	long    linelen;
	int     gotsess = 0, gotprompt = 0;
	char    rec[512];
	char   *fields[8];
	int     nf;
	long    cplen;
	long    t;
	Tm     *tm;
	char   *src;
	int     dlen, inspace;

	uuid[0] = model[0] = ts[0] = snippet[0] = '\0';

	b = Bopen(path, OREAD);
	if(b == nil)
		return -1;

	while((!gotsess || !gotprompt) &&
	      (line = Brdline(b, 0x1e)) != nil) {
		linelen = Blinelen(b);
		cplen = linelen < (long)sizeof rec - 1 ? linelen : (long)sizeof rec - 1;
		memmove(rec, line, cplen);
		rec[cplen] = '\0';
		nf = splitrec(rec, cplen, fields, 8);
		if(nf < 1)
			continue;

		if(!gotsess && strcmp(fields[0], "session") == 0) {
			if(nf >= 2) strecpy(uuid,  uuid+37,  fields[1]);
			if(nf >= 3) strecpy(model, model+64, fields[2]);
			if(nf >= 4) {
				t  = atol(fields[3]);
				tm = localtime(t);
				if(tm)
					snprint(ts, 32, "%04d-%02d-%02d %02d:%02d",
					        tm->year, tm->mon+1, tm->mday,
					        tm->hour, tm->min);
				else
					strecpy(ts, ts+32, fields[3]);
			}
			gotsess = 1;

		} else if(gotsess && !gotprompt && strcmp(fields[0], "prompt") == 0) {
			if(nf >= 2) {
				src = fields[1];
				dlen = 0; inspace = 0;
				while(*src && dlen < SESS_SNIPPET - 1) {
					if(*src == '\n' || *src == '\r' || *src == '\t')
						*src = ' ';
					if(*src == ' ' && inspace) { src++; continue; }
					inspace = (*src == ' ');
					snippet[dlen++] = *src++;
				}
				snippet[dlen] = '\0';
				if(*src && dlen > 3) {
					snippet[dlen-1] = '.';
					snippet[dlen-2] = '.';
					snippet[dlen-3] = '.';
				}
			}
			gotprompt = 1;
		}
	}

	Bterm(b);
	return gotsess ? 0 : -1;
}
