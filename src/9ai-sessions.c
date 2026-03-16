/*
 * 9ai-sessions — list 9ai session files
 *
 * Usage:
 *   9ai-sessions [dir]
 *
 * Reads the session directory (default: ~/.cache/9ai/sessions/).
 * For each file, reads the first "session" record and the first "prompt"
 * record, then prints one tab-separated line:
 *
 *   <uuid>\t<model>\t<date>\t<first-prompt-snippet>
 *
 * Sessions are printed newest-first (sorted by file mtime).
 *
 * The output format is used by 9ai-acme's Sessions window: each UUID
 * can be middle-clicked to load that session.
 *
 * ── Record format ───────────────────────────────────────────────────
 *
 *   Each record: field₀ FS field₁ … FS fieldₙ RS
 *     FS  = 0x1F (field separator)
 *     RS  = 0x1E (record separator)
 *     ESC = 0x1B (escape: next byte is literal within a field)
 *
 *   splitrec() decodes ESC-encoding and NUL-patches FS bytes in place,
 *   filling a fields[] pointer array.  Empty fields are supported.
 *
 * ── Session file structure ───────────────────────────────────────────
 *
 *   First record:
 *     session FS <uuid> FS <model> FS <unix-timestamp> RS
 *
 *   First prompt record (may not be the second record):
 *     prompt FS <text> RS
 *
 * ── Date formatting ─────────────────────────────────────────────────
 *
 *   The unix timestamp in the session record is formatted as
 *   "YYYY-MM-DD HH:MM" in local time using Plan 9 localtime().
 *
 * ── Snippet ─────────────────────────────────────────────────────────
 *
 *   The first prompt text is truncated to SNIPPET characters.
 *   Newlines and tabs are collapsed to single spaces.
 *   If truncated, the last three characters are replaced with "...".
 */

#include <u.h>
#include <libc.h>
#include <bio.h>

#include "9ai.h"
#include "record.h"
#define SNIPPET  80
#define MAXFIELDS 16
#define RECBUF   4096

/*
 * parsesessfile — extract uuid, model, date-string, and prompt snippet
 * from a session file.
 *
 * Reads only up to the first "session" and first "prompt" records.
 * Populates the four output buffers (all must be pre-allocated by caller).
 *
 * Returns 0 on success (session record found), -1 if not a valid session file.
 */
static int
parsesessfile(char *path,
              char *uuid,   /* ≥37 bytes  */
              char *model,  /* ≥64 bytes  */
              char *ts,     /* ≥32 bytes  */
              char *snip)   /* ≥SNIPPET+4 bytes */
{
	Biobuf *b;
	char  *line;
	long   linelen;
	int    gotsess   = 0;
	int    gotprompt = 0;

	uuid[0] = model[0] = ts[0] = snip[0] = '\0';

	b = Bopen(path, OREAD);
	if(b == nil)
		return -1;

	while((!gotsess || !gotprompt) &&
	      (line = Brdline(b, 0x1e)) != nil) {
		char   rec[RECBUF];
		char  *fields[MAXFIELDS];
		int    nf;
		long   cplen;

		linelen = Blinelen(b);
		cplen   = linelen < (long)sizeof rec - 1 ? linelen : (long)sizeof rec - 1;
		memmove(rec, line, cplen);
		rec[cplen] = '\0';

		nf = splitrec(rec, cplen, fields, MAXFIELDS);
		if(nf < 1)
			continue;

		if(!gotsess && strcmp(fields[0], "session") == 0) {
			if(nf >= 2) strecpy(uuid,  uuid+37,  fields[1]);
			if(nf >= 3) strecpy(model, model+64, fields[2]);
			if(nf >= 4) {
				long t  = atol(fields[3]);
				Tm  *tm = localtime(t);
				if(tm != nil)
					snprint(ts, 32, "%04d-%02d-%02d %02d:%02d",
					        tm->year, tm->mon + 1, tm->mday,
					        tm->hour, tm->min);
				else
					strecpy(ts, ts+32, fields[3]);
			}
			gotsess = 1;

		} else if(gotsess && !gotprompt && strcmp(fields[0], "prompt") == 0) {
			if(nf >= 2) {
				char *src  = fields[1];
				int   dlen = 0;
				int   insp = 0;

				while(*src && dlen < SNIPPET - 1) {
					char c = *src++;
					/* collapse whitespace */
					if(c == '\n' || c == '\r' || c == '\t')
						c = ' ';
					if(c == ' ' && insp)
						continue;
					insp = (c == ' ');
					snip[dlen++] = c;
				}
				snip[dlen] = '\0';

				/* if truncated, show ellipsis */
				if(*src != '\0' && dlen >= 3) {
					snip[dlen-1] = '.';
					snip[dlen-2] = '.';
					snip[dlen-3] = '.';
				}
			}
			gotprompt = 1;
		}
	}

	Bterm(b);

	return gotsess ? 0 : -1;
}

/*
 * mtime_cmp — Dir comparison for sorting by mtime descending (newest first).
 * Used with qsort().
 */
static int
mtime_cmp(const void *a, const void *b)
{
	const Dir *da = (const Dir *)a;
	const Dir *db = (const Dir *)b;
	if(db->mtime > da->mtime) return  1;
	if(db->mtime < da->mtime) return -1;
	return 0;
}

void
main(int argc, char *argv[])
{
	char *sessdir;
#ifdef PLAN9PORT
	char *home;
#endif
	int   dirfd;
	Dir  *dirs;
	long  ndirs, i;

	if(argc >= 2) {
		sessdir = strdup(argv[1]);
	} else {
#ifdef PLAN9PORT
		home    = getenv("HOME");
		if(home == nil || home[0] == '\0')
			home = smprint("/home/%s", getuser());
		sessdir = smprint("%s/.cache/9ai/sessions", home);
#else
		sessdir = configpath("sessions");
#endif
	}

	dirfd = open(sessdir, OREAD);
	if(dirfd < 0) {
		fprint(2, "9ai-sessions: open %s: %r\n", sessdir);
		free(sessdir);
		exits("open");
	}

	ndirs = dirreadall(dirfd, &dirs);
	close(dirfd);

	if(ndirs < 0) {
		fprint(2, "9ai-sessions: dirreadall %s: %r\n", sessdir);
		free(sessdir);
		exits("dirread");
	}

	/* sort newest-first */
	qsort(dirs, ndirs, sizeof dirs[0], mtime_cmp);

	for(i = 0; i < ndirs; i++) {
		char path[512];
		char uuid[37], model[64], ts[32], snip[SNIPPET + 4];

		if(dirs[i].name[0] == '.')
			continue;
		/* skip subdirectories */
		if(dirs[i].qid.type & QTDIR)
			continue;

		snprint(path, sizeof path, "%s/%s", sessdir, dirs[i].name);
		if(parsesessfile(path, uuid, model, ts, snip) < 0)
			continue;
		if(uuid[0] == '\0')
			continue;

		print("%s\t%s\t%s\t%s\n", uuid, model, ts, snip);
	}

	free(dirs);
	free(sessdir);
	exits(nil);
}
