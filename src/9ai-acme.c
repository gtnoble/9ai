/*
 * 9ai-acme — acme integration for 9ai
 *
 * Usage:
 *   9ai-acme [-a addr]
 *
 * Creates a +9ai window in the current directory.  Connects to the 9ai
 * 9P service (posted in $NAMESPACE as "9ai") and to acme's 9P service.
 *
 * Tag commands:
 *   Send     — send the last paragraph as a prompt
 *   Stop     — abort the current turn (writes "abort" to 9ai/ctl)
 *   Steer    — redirect the running turn with the last paragraph
 *   New      — start a fresh session (writes to 9ai/session/new)
 *   Clear    — clear the in-memory history (writes "clear" to 9ai/ctl)
 *   Models   — open a model picker window (button-3 to switch)
 *   Sessions — open a session picker window (button-3 to load)
 *
 * ── Architecture ──────────────────────────────────────────────────────
 *
 * Three procs communicate via channels to a display proc:
 *
 *   eventproc   — reads acme's window event file; dispatches commands
 *   outproc     — reads 9ai /output per turn; appends text to window
 *   aiproc      — reads 9ai /event per turn; parses structured records
 *                 (turn_start, thinking, tool_start, tool_end, turn_end,
 *                  model, session_new, error) → appends formatted lines
 *
 * All window writes go through winappend(), which holds a QLock so
 * that the three procs don't interleave partial lines.
 *
 * ── 9ai /event record format ──────────────────────────────────────────
 *
 * Records are RS-terminated (0x1E), fields FS-separated (0x1F).
 * Field 0 is the type.  A single read(2) on /event returns exactly one
 * RS-terminated record (or 0 bytes for EOF at turn end).
 *
 * We read into a buffer, scanning for the RS byte to frame records.
 * On EOF (0-byte read), the turn is done; we re-open /event for the next.
 *
 * ── Session file format ───────────────────────────────────────────────
 *
 * ~/.cache/9ai/sessions/<uuid>  — one record per RS terminator.
 * First record: session FS uuid FS model FS timestamp.
 * First prompt record: prompt FS text.
 * We read only the first two relevant records for the Sessions picker.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>
#include <9pclient.h>

#include "9ai.h"   /* homedir() */

/* ── Constants ──────────────────────────────────────────────────────── */

enum {
	STACK      = 65536,
	EVBUF      = 8192,   /* acme event read buffer */
	RECBUF     = 131072, /* 9ai event record buffer */
	TEXTBUF    = 131072, /* /output streaming buffer */
	MAXFIELDS  = 64,
	SNIPPET    = 80,     /* max session snippet length */
};

#define TAG_EXTRA  " | Send Stop Steer New Clear Models Sessions"
#define SEPARATOR  "\n\n════════════════════════════════════════════════════════════\n\n"

/* ── Global acme connection ─────────────────────────────────────────── */

static CFsys *acmefs;
static QLock  winlk;   /* serialises all writes to the +9ai window */

/* ── Forward declarations ────────────────────────────────────────────── */

static void modelswinproc(void *v);
static void sessionswinproc(void *v);

/* ── Window helpers ─────────────────────────────────────────────────── */

typedef struct Win Win;
struct Win {
	int   id;
	CFid *ctl;
	CFid *event;
	CFid *addr;
	CFid *data;
	CFid *body;
};

static Win *mainwin;  /* the +9ai window */

static CFid *
wopenfile(Win *w, char *name, int mode)
{
	char path[64];
	snprint(path, sizeof path, "%d/%s", w->id, name);
	CFid *f = fsopen(acmefs, path, mode);
	if(f == nil)
		sysfatal("fsopen %s: %r", path);
	return f;
}

static Win *
newwin(char *name)
{
	char buf[32];
	Win *w;

	w = mallocz(sizeof *w, 1);
	if(w == nil)
		sysfatal("mallocz Win: %r");

	w->ctl = fsopen(acmefs, "new/ctl", ORDWR);
	if(w->ctl == nil)
		sysfatal("open new/ctl: %r");
	if(fsread(w->ctl, buf, sizeof buf) < 1)
		sysfatal("read new/ctl: %r");
	w->id = atoi(buf);

	/* name the window */
	fsprint(w->ctl, "name %s\n", name);

	/* set up tag */
	fsprint(w->ctl, "cleartag\n");
	{
		char tagpath[64];
		snprint(tagpath, sizeof tagpath, "%d/tag", w->id);
		CFid *tag = fsopen(acmefs, tagpath, OWRITE);
		if(tag != nil) {
			fswrite(tag, TAG_EXTRA, strlen(TAG_EXTRA));
			fsclose(tag);
		}
	}

	w->event = wopenfile(w, "event", ORDWR);
	w->body  = wopenfile(w, "body",  OWRITE);
	/* addr and data opened on demand */
	w->addr  = nil;
	w->data  = nil;

	return w;
}

/*
 * winappend — append text to the window body.
 * Holds winlk so concurrent procs don't interleave output.
 */
static void
winappend(Win *w, char *text, int len)
{
	if(len < 0)
		len = strlen(text);
	if(len == 0)
		return;

	qlock(&winlk);
	if(w->addr == nil) {
		char p[64];
		snprint(p, sizeof p, "%d/addr", w->id);
		w->addr = fsopen(acmefs, p, OWRITE);
		if(w->addr == nil)
			sysfatal("open addr: %r");
	}
	if(w->data == nil) {
		char p[64];
		snprint(p, sizeof p, "%d/data", w->id);
		w->data = fsopen(acmefs, p, ORDWR);
		if(w->data == nil)
			sysfatal("open data: %r");
	}
	/* append at end */
	fswrite(w->addr, "$", 1);
	fswrite(w->data, text, len);
	qunlock(&winlk);
}

static void
winappendstr(Win *w, char *s)
{
	winappend(w, s, -1);
}

/*
 * winclean — mark window clean so it can be closed without a save prompt.
 */
static void
winclean(Win *w)
{
	fsprint(w->ctl, "clean\n");
}

/*
 * windelete — force-close a window.
 */
static void
windelete(Win *w)
{
	fsprint(w->ctl, "delete\n");
}

/*
 * winbody — read the entire window body.  Caller must free.
 */
static char *
winbody(Win *w, int *np)
{
	char *s;
	int   n, na, m;
	char  path[64];
	CFid *body;

	snprint(path, sizeof path, "%d/body", w->id);
	body = fsopen(acmefs, path, OREAD);
	if(body == nil)
		return nil;

	s  = nil;
	na = 0;
	n  = 0;
	for(;;) {
		if(na < n + 512) {
			na += 4096;
			s = realloc(s, na + 1);
		}
		m = fsread(body, s + n, na - n);
		if(m <= 0)
			break;
		n += m;
	}
	fsclose(body);
	if(s == nil)
		s = strdup("");
	else
		s[n] = '\0';
	if(np) *np = n;
	return s;
}

/* ── Status line (line 1 of the body) ──────────────────────────────── */

/*
 * status format: "● <state> [<model>] session:<uuid8>\n"
 *
 * We maintain current model and session uuid so status() can format them.
 */
static char curmodel[128];
static char cursession[37];
static QLock statelk;

static void
setstatus(char *state)
{
	char buf[256];
	char modstr[144];
	char sesstr[16];

	qlock(&statelk);
	if(curmodel[0])
		snprint(modstr, sizeof modstr, " [%s]", curmodel);
	else
		modstr[0] = '\0';
	if(cursession[0])
		snprint(sesstr, sizeof sesstr, " %.8s", cursession);
	else
		sesstr[0] = '\0';
	qunlock(&statelk);

	snprint(buf, sizeof buf, "● %s%s%s\n", state, modstr, sesstr);

	/* replace line 1 */
	qlock(&winlk);
	if(mainwin->addr == nil) {
		char p[64];
		snprint(p, sizeof p, "%d/addr", mainwin->id);
		mainwin->addr = fsopen(acmefs, p, OWRITE);
	}
	if(mainwin->data == nil) {
		char p[64];
		snprint(p, sizeof p, "%d/data", mainwin->id);
		mainwin->data = fsopen(acmefs, p, ORDWR);
	}
	/* address line 1 and replace */
	fswrite(mainwin->addr, "1", 1);
	fswrite(mainwin->data, buf, strlen(buf));
	qunlock(&winlk);
}

/* ── 9ai service connection ─────────────────────────────────────────── */

/*
 * aifsys — shared connection used by command handlers (Send, Stop, etc.)
 * outproc and aiproc create their own private connections to avoid
 * interfering with each other or with command handlers.
 */
static CFsys *aifsys;
static QLock  aifslk;

static CFsys *
aifs_get(void)
{
	CFsys *fs;

	qlock(&aifslk);
	fs = aifsys;
	qunlock(&aifslk);
	return fs;
}

static CFid *
aiopen(char *path, int mode)
{
	CFsys *fs = aifs_get();
	if(fs == nil) {
		werrstr("9ai not connected");
		return nil;
	}
	return fsopen(fs, path, mode);
}

/* ainewconn — open a fresh private connection to the 9ai service */
static CFsys *
ainewconn(void)
{
	return nsmount("9ai", nil);
}

/* ── RS/FS record parser ────────────────────────────────────────────── */

/*
 * splitrec — split a RS-terminated record (0x1E) into FS-separated
 * (0x1F) fields.  Handles doubled FS/RS as literal characters.
 *
 * Returns number of fields in fields[]; each is a nul-terminated
 * string pointing into rec (fields are NUL-patched in place).
 *
 * The record must be writable.
 */
static int
splitrec(char *rec, int reclen, char **fields, int maxfields)
{
	char *p, *end, *start;
	int   nf = 0;

	end = rec + reclen;
	/* strip trailing RS */
	if(end > rec && (uchar)*(end-1) == 0x1e)
		end--;

	p = rec;
	while(p < end && nf < maxfields) {
		start = p;
		while(p < end && (uchar)*p != 0x1f)
			p++;
		/* NUL-terminate the field in place */
		if(p < end)
			*p++ = '\0';
		else
			*p = '\0';  /* end of record */
		fields[nf++] = start;
	}
	return nf;
}

/* ── 9ai /event reader proc ─────────────────────────────────────────── */

/*
 * Format a tool_start record for display:
 *   "┌ ⚙ <name> <argv[0]> <argv[1]> ..."
 *   "│ argv: <argv joined>"
 *
 * fields[0] = "tool_start"
 * fields[1] = name
 * fields[2] = id
 * fields[3..nf-1] = argv
 */
static void
display_tool_start(char **fields, int nf)
{
	char buf[1024];
	char args[512];
	int i, alen = 0;

	args[0] = '\0';
	for(i = 3; i < nf; i++) {
		if(alen + 2 < (int)sizeof args) {
			if(i > 3) { args[alen++] = ' '; args[alen] = '\0'; }
			int slen = strlen(fields[i]);
			if(alen + slen < (int)sizeof args - 1) {
				memmove(args + alen, fields[i], slen);
				alen += slen;
				args[alen] = '\0';
			}
		}
	}
	/* truncate args if too long */
	if(alen > 200) {
		memmove(args + 197, "...", 4);
		alen = 200;
	}

	snprint(buf, sizeof buf, "\n┌ ⚙ %s %s\n",
	        nf > 1 ? fields[1] : "?",
	        args);
	winappendstr(mainwin, buf);
}

static void
display_tool_end(char **fields, int nf)
{
	int is_err = (nf > 1 && strcmp(fields[1], "err") == 0);
	if(is_err) {
		char buf[128];
		char *out = (nf > 2 && fields[2][0]) ? fields[2] : "";
		/* show first line of error */
		char first[80];
		int i;
		for(i = 0; i < 79 && out[i] && out[i] != '\n'; i++)
			first[i] = out[i];
		first[i] = '\0';
		snprint(buf, sizeof buf, "└ ✗ %s\n\n", first);
		winappendstr(mainwin, buf);
	} else {
		winappendstr(mainwin, "└ ✓\n\n");
	}
}

static void
aiproc(void *v)
{
	CFsys *fs;
	CFid  *evfd;
	char   buf[RECBUF];
	int    n;

	USED(v);

	fs = nil;
	for(;;) {
		/* (re)connect if needed */
		if(fs == nil) {
			fs = ainewconn();
			if(fs == nil) {
				sleep(2000);
				continue;
			}
		}

		evfd = fsopen(fs, "event", OREAD);
		if(evfd == nil) {
			/* likely "concurrent reads" from stale conn — reconnect */
			fsunmount(fs);
			fs = nil;
			sleep(500);
			continue;
		}

		/* read records until EOF (turn done) */
		for(;;) {
			n = fsread(evfd, buf, sizeof buf - 1);
			if(n <= 0)
				break;   /* EOF = turn end */
			buf[n] = '\0';

			/*
			 * buf contains one RS-terminated record.
			 * Parse fields in a copy because splitrec patches NULs.
			 */
			char rec[RECBUF];
			char *fields[MAXFIELDS];
			int   nf;

			memmove(rec, buf, n + 1);
			nf = splitrec(rec, n, fields, MAXFIELDS);
			if(nf == 0)
				continue;

			char *type = fields[0];

			if(strcmp(type, "turn_start") == 0) {
				/* fields: turn_start FS uuid FS model */
				if(nf >= 3) {
					qlock(&statelk);
					strlcpy(curmodel, fields[2], sizeof curmodel);
					qunlock(&statelk);
				}
				setstatus("running");

			} else if(strcmp(type, "thinking") == 0) {
				/* fields: thinking FS chunk */
				if(nf >= 2) {
					/* render thinking inline with bar prefix on each line */
					/* U+2502 BOX DRAWINGS LIGHT VERTICAL = UTF-8 e2 94 82 */
					char *chunk = fields[1];
					char  out[RECBUF + 16];
					int   olen = 0;
					out[olen++] = '\n';
					out[olen++] = (char)0xe2; out[olen++] = (char)0x94; out[olen++] = (char)0x82;
					out[olen++] = ' ';
					for(int i = 0; chunk[i] && olen < (int)sizeof out - 8; i++) {
						out[olen++] = chunk[i];
						if(chunk[i] == '\n' && chunk[i+1]) {
							out[olen++] = (char)0xe2; out[olen++] = (char)0x94; out[olen++] = (char)0x82;
							out[olen++] = ' ';
						}
					}
					out[olen] = '\0';
					winappend(mainwin, out, olen);
				}

			} else if(strcmp(type, "tool_start") == 0) {
				display_tool_start(fields, nf);

			} else if(strcmp(type, "tool_end") == 0) {
				display_tool_end(fields, nf);

			} else if(strcmp(type, "turn_end") == 0) {
				/*
				 * fields: turn_end FS end_turn|aborted|error
				 * The /output proc emits the separator after [done].
				 * We just update status here.
				 */
				char *reason = (nf >= 2) ? fields[1] : "end_turn";
				if(strcmp(reason, "aborted") == 0)
					winappendstr(mainwin, "\n⛔ Aborted.\n");
				else if(strcmp(reason, "error") == 0)
					winappendstr(mainwin, "\n⚠ Agent error.\n");
				setstatus("ready");

			} else if(strcmp(type, "model") == 0) {
				/* fields: model FS name */
				if(nf >= 2) {
					qlock(&statelk);
					strlcpy(curmodel, fields[1], sizeof curmodel);
					qunlock(&statelk);
				}
				setstatus("ready");

			} else if(strcmp(type, "session_new") == 0) {
				/* fields: session_new FS uuid */
				if(nf >= 2) {
					qlock(&statelk);
					strlcpy(cursession, fields[1], sizeof cursession);
					qunlock(&statelk);
				}
				setstatus("ready");

			} else if(strcmp(type, "error") == 0) {
				if(nf >= 2) {
					char errbuf[256];
					snprint(errbuf, sizeof errbuf, "\n⚠ %s\n", fields[1]);
					winappendstr(mainwin, errbuf);
				}
				setstatus("error");
			}
		}

		fsclose(evfd);
		/* turn ended — loop to open /event again for the next turn */
	}
}

/* ── 9ai /output reader proc ────────────────────────────────────────── */

static void
outproc(void *v)
{
	CFsys *fs;
	CFid  *outfd;
	char   buf[TEXTBUF];
	int    n;

	USED(v);

	fs = nil;
	for(;;) {
		/* (re)connect if needed */
		if(fs == nil) {
			fs = ainewconn();
			if(fs == nil) {
				sleep(2000);
				continue;
			}
		}

		outfd = fsopen(fs, "output", OREAD);
		if(outfd == nil) {
			/* likely "concurrent reads" from stale conn — reconnect */
			fsunmount(fs);
			fs = nil;
			sleep(500);
			continue;
		}

		for(;;) {
			n = fsread(outfd, buf, sizeof buf - 1);
			if(n < 0) {
				/* error on an open fid — close, reconnect */
				break;
			}
			if(n == 0)
				break;
			buf[n] = '\0';

			/* "[done]\n" is the end-of-turn sentinel from /output */
			if(strcmp(buf, "[done]\n") == 0) {
				winappendstr(mainwin, SEPARATOR);
				winclean(mainwin);
				break;
			}

			/* stream text directly to the window */
			winappend(mainwin, buf, n);
		}

		fsclose(outfd);
		/* re-open for next turn */
	}
}

/* ── Acme event reader ──────────────────────────────────────────────── */

/*
 * acme event format (raw):
 *   c1 c2 q0SP q1SP flagSP nrSP text '\n'
 *
 * We parse this from the event file using the same technique as
 * acmeevent.c (getn, getrune).
 */

typedef struct AcmeEvent AcmeEvent;
struct AcmeEvent {
	int  c1, c2;
	int  q0, q1;
	int  eq0, eq1;  /* expanded range (from flag & 2 follow-up) */
	int  flag, nr;
	char text[512];
};

static int
evgetc(CFid *f, char *buf, int *bufp, int *nbuf)
{
	if(*bufp >= *nbuf) {
		*nbuf = fsread(f, buf, EVBUF);
		*bufp = 0;
		if(*nbuf <= 0)
			return -1;
	}
	return (uchar)buf[(*bufp)++];
}

static int
evgetn(CFid *f, char *buf, int *bufp, int *nbuf)
{
	int c, n = 0;
	while((c = evgetc(f, buf, bufp, nbuf)) >= '0' && c <= '9')
		n = n * 10 + c - '0';
	if(c != ' ')
		return -1;  /* parse error */
	return n;
}

/*
 * evgetrune — read one UTF-8 rune from the event stream.
 * Returns the rune, or -1 on error.
 * Appends the bytes to dst[*dlen]; dlen must not exceed dsz-4.
 */
static Rune
evgetrune(CFid *f, char *buf, int *bufp, int *nbuf, char *dst, int *dlen, int dsz)
{
	Rune r;
	int  c, nb;

	if(*dlen >= dsz - 4)
		return -1;

	c = evgetc(f, buf, bufp, nbuf);
	if(c < 0)
		return -1;
	dst[*dlen] = c;
	nb = 1;
	if(c >= Runeself) {
		while(!fullrune(dst + *dlen, nb)) {
			c = evgetc(f, buf, bufp, nbuf);
			if(c < 0)
				return -1;
			dst[*dlen + nb++] = c;
		}
	}
	chartorune(&r, dst + *dlen);
	*dlen += nb;
	return r;
}

/*
 * getevent — read one event from the acme event file.
 * Returns 1 on success, 0 on EOF/error.
 *
 * Also reads the flag-2 expansion event (if present) and the
 * flag-8 chord events (if present), populating e->eq0/eq1 and
 * ignoring the chord text.
 */
static int
getevent(CFid *f, char *buf, int *bufp, int *nbuf, AcmeEvent *e)
{
	int i, c;
	char tbuf[512];
	int  tlen;

	e->c1 = evgetc(f, buf, bufp, nbuf);
	if(e->c1 < 0)
		return 0;
	e->c2 = evgetc(f, buf, bufp, nbuf);
	if(e->c2 < 0)
		return 0;

	e->q0   = evgetn(f, buf, bufp, nbuf);
	e->q1   = evgetn(f, buf, bufp, nbuf);
	e->flag = evgetn(f, buf, bufp, nbuf);
	e->nr   = evgetn(f, buf, bufp, nbuf);

	if(e->q0 < 0 || e->q1 < 0 || e->flag < 0 || e->nr < 0)
		return 0;

	e->eq0 = e->q0;
	e->eq1 = e->q1;

	if(e->nr > 256)
		e->nr = 256;

	/* read text runes */
	tlen = 0;
	for(i = 0; i < e->nr; i++) {
		if(evgetrune(f, buf, bufp, nbuf, e->text, &tlen,
		             sizeof e->text) < 0)
			return 0;
	}
	e->text[tlen] = '\0';

	/* consume trailing newline */
	c = evgetc(f, buf, bufp, nbuf);
	if(c != '\n')
		return 0;

	/* flag & 2: null string with non-null expansion — read follow-up event.
	 * The expansion is a full event record: c1 c2 q0 q1 flag nr text \n
	 * We capture the expansion text into e->text (replacing the empty original). */
	if(e->flag & 2) {
		int xq0, xq1, xflag, xnr;
		evgetc(f, buf, bufp, nbuf);  /* c1 of expansion (discard) */
		evgetc(f, buf, bufp, nbuf);  /* c2 of expansion (discard) */
		xq0   = evgetn(f, buf, bufp, nbuf);
		xq1   = evgetn(f, buf, bufp, nbuf);
		xflag = evgetn(f, buf, bufp, nbuf);
		xnr   = evgetn(f, buf, bufp, nbuf);
		e->eq0 = xq0;
		e->eq1 = xq1;
		USED(xflag);
		/* read expansion text into e->text */
		tlen = 0;
		if(xnr > 256) xnr = 256;
		for(i = 0; i < xnr; i++)
			evgetrune(f, buf, bufp, nbuf, e->text, &tlen, sizeof e->text);
		e->text[tlen] = '\0';
		evgetc(f, buf, bufp, nbuf);   /* newline */
	}

	/* flag & 8: chorded argument — read and discard two follow-up events */
	if(e->flag & 8) {
		int pass;
		for(pass = 0; pass < 2; pass++) {
			evgetc(f, buf, bufp, nbuf);  /* c1 */
			evgetc(f, buf, bufp, nbuf);  /* c2 */
			evgetn(f, buf, bufp, nbuf);  /* q0 */
			evgetn(f, buf, bufp, nbuf);  /* q1 */
			evgetn(f, buf, bufp, nbuf);  /* flag */
			int nr2 = evgetn(f, buf, bufp, nbuf);
			if(nr2 > 256) nr2 = 256;
			tlen = 0;
			for(i = 0; i < nr2; i++)
				evgetrune(f, buf, bufp, nbuf, tbuf, &tlen, sizeof tbuf);
			evgetc(f, buf, bufp, nbuf);  /* newline */
		}
	}

	return 1;
}

/*
 * writeevent — write an event back to acme so it handles it natively.
 */
static void
writeevent(CFid *evfd, AcmeEvent *e)
{
	fsprint(evfd, "%c%c%d %d\n", e->c1, e->c2, e->q0, e->q1);
}

/* ── Prompt text extraction ─────────────────────────────────────────── */

/*
 * prompttext — extract the text the user wants to send.
 *
 * Strategy (in order):
 *   1. If acme's current selection (dot) is non-empty, use that.
 *   2. Otherwise use the text from after the last separator (═══…) or
 *      the last ▶ echo line, to end-of-body — trimmed of leading/trailing
 *      whitespace.
 *
 * Separators are the ════…════ lines written by outproc after each turn.
 * ▶ lines are the prompt-echo lines written by cmd_send.
 *
 * Returns a malloc'd nul-terminated string, or nil if nothing useful.
 */

/* issepchr — true if byte is part of a ═ (U+2550, UTF-8 e2 95 90) */
static int
issepchr(uchar a, uchar b, uchar c)
{
	return (a == 0xe2 && b == 0x95 && c == 0x90);
}

static char *
prompttext(Win *w)
{
	char *body;
	int   bodylen;
	char *after;   /* pointer into body: start of candidate text */
	char *p;
	char *result;
	int   rlen;

	/* ── try dot selection first ── */
	{
		char addrpath[64], datapath[64];
		CFid *af, *df;
		char  buf[8192];
		int   n;

		snprint(addrpath, sizeof addrpath, "%d/addr", w->id);
		snprint(datapath, sizeof datapath, "%d/data", w->id);

		af = fsopen(acmefs, addrpath, ORDWR);
		df = fsopen(acmefs, datapath, ORDWR);
		if(af != nil && df != nil) {
			/* set addr to dot */
			fswrite(af, ".", 1);
			n = fsread(df, buf, sizeof buf - 1);
			if(n > 0) {
				buf[n] = '\0';
				/* strip leading/trailing whitespace */
				char *s = buf, *e = buf + n - 1;
				while(*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
				while(e > s && (*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r')) e--;
				*(e+1) = '\0';
				if(*s != '\0') {
					if(af) fsclose(af);
					if(df) fsclose(df);
					return strdup(s);
				}
			}
		}
		if(af) fsclose(af);
		if(df) fsclose(df);
	}

	/* ── fall back: text after last separator/echo line ── */
	body = winbody(w, &bodylen);
	if(body == nil)
		return nil;

	after = nil;
	p     = body;

	/*
	 * Walk line by line.  Record the position after any line that is:
	 *   - a separator (starts with ═ / 0xe2 0x95 0x90)
	 *   - a prompt echo (starts with ▶ / 0xe2 0x96 0xb6)
	 *   - the status line (starts with ● / 0xe2 0x97 0x8f)
	 * The last such position becomes `after`.
	 */
	while(*p) {
		char *eol = strchr(p, '\n');
		if(eol == nil) eol = p + strlen(p);

		int linelen = eol - p;
		if(linelen >= 3) {
			uchar a = p[0], b = p[1], c = p[2];
			int is_sep    = issepchr(a, b, c);
			int is_echo   = (a == 0xe2 && b == 0x96 && c == 0xb6); /* ▶ */
			int is_status = (a == 0xe2 && b == 0x97 && c == 0x8f); /* ● */
			int is_tool   = (a == 0xe2 && b == 0x94 && c == 0x8c); /* ┌ */
			int is_steer  = (a == 0xe2 && b == 0x86 && c == 0xa9); /* ↩ */
			if(is_sep || is_echo || is_status || is_tool || is_steer)
				after = (*eol) ? eol + 1 : eol;
		} else if(linelen == 0) {
			/* blank line — could be a paragraph break, but don't reset after */
		}
		p = (*eol) ? eol + 1 : eol;
	}

	if(after == nil) {
		/* nothing found — skip only the status line */
		after = strchr(body, '\n');
		after = after ? after + 1 : body;
	}

	/* trim leading whitespace */
	while(*after == '\n' || *after == '\r' || *after == ' ' || *after == '\t')
		after++;

	/* trim trailing whitespace */
	rlen = strlen(after);
	while(rlen > 0 && (after[rlen-1] == '\n' || after[rlen-1] == '\r' ||
	                   after[rlen-1] == ' '  || after[rlen-1] == '\t'))
		rlen--;

	if(rlen <= 0) {
		free(body);
		return nil;
	}

	result = mallocz(rlen + 1, 1);
	memmove(result, after, rlen);
	result[rlen] = '\0';
	free(body);
	return result;
}

/* ── Tag command implementations ────────────────────────────────────── */

static void
cmd_send(void)
{
	char *text;
	CFid *msgfd;

	text = prompttext(mainwin);
	if(text == nil || text[0] == '\0') {
		free(text);
		return;
	}

	msgfd = aiopen("message", OWRITE);
	if(msgfd == nil) {
		char errbuf[128];
		snprint(errbuf, sizeof errbuf, "\n⚠ 9ai not available: %r\n");
		winappendstr(mainwin, errbuf);
		free(text);
		return;
	}

	/* display the prompt in the window */
	{
		char *display = smprint("\n▶ %s\n", text);
		winappendstr(mainwin, display);
		free(display);
	}

	fswrite(msgfd, text, strlen(text));
	fsclose(msgfd);   /* clunk triggers the agent turn */
	setstatus("running");

	free(text);
}

static void
cmd_stop(void)
{
	CFid *ctlfd;

	ctlfd = aiopen("ctl", OWRITE);
	if(ctlfd == nil) {
		winappendstr(mainwin, "\n⚠ 9ai not available\n");
		return;
	}
	fswrite(ctlfd, "abort\n", 6);
	fsclose(ctlfd);
	setstatus("aborting");
}

static void
cmd_steer(void)
{
	char *text;
	CFid *steerfd;

	text = prompttext(mainwin);
	if(text == nil || text[0] == '\0') {
		free(text);
		return;
	}

	steerfd = aiopen("steer", OWRITE);
	if(steerfd == nil) {
		winappendstr(mainwin, "\n⚠ 9ai not available\n");
		free(text);
		return;
	}

	{
		char *display = smprint("\n↩ Steer: %s\n", text);
		winappendstr(mainwin, display);
		free(display);
	}

	fswrite(steerfd, text, strlen(text));
	fsclose(steerfd);
	free(text);
}

static void
cmd_new(void)
{
	CFid *newfd;

	newfd = aiopen("session/new", OWRITE);
	if(newfd == nil) {
		winappendstr(mainwin, "\n⚠ 9ai not available\n");
		return;
	}
	fswrite(newfd, "new", 3);
	fsclose(newfd);
	winappendstr(mainwin, "\n── New session ──\n");
}

static void
cmd_clear(void)
{
	CFid *ctlfd;

	ctlfd = aiopen("ctl", OWRITE);
	if(ctlfd == nil) {
		winappendstr(mainwin, "\n⚠ 9ai not available\n");
		return;
	}
	fswrite(ctlfd, "clear\n", 6);
	fsclose(ctlfd);

	/* also clear the window body, keeping the status line */
	qlock(&winlk);
	if(mainwin->addr == nil) {
		char p[64];
		snprint(p, sizeof p, "%d/addr", mainwin->id);
		mainwin->addr = fsopen(acmefs, p, OWRITE);
	}
	if(mainwin->data == nil) {
		char p[64];
		snprint(p, sizeof p, "%d/data", mainwin->id);
		mainwin->data = fsopen(acmefs, p, ORDWR);
	}
	fswrite(mainwin->addr, "2,$", 3);
	fswrite(mainwin->data, "", 0);
	qunlock(&winlk);

	winclean(mainwin);
	setstatus("ready");
}

/*
 * cmd_models — open a +9ai/+models window with the current model list.
 * Each line is:  id TAB format TAB ctx TAB maxout TAB tools TAB name
 * Button-3 on the id field to switch model.
 */
static void
cmd_models(void)
{
	Win  *w;
	CFid *modelsfd;
	char  buf[4096];
	int   n;
	char  name[256];

	snprint(name, sizeof name, "%s/+9ai/+models", getenv("cwd") ? getenv("cwd") : ".");

	w = newwin(name);

	winappendstr(w, "# Middle-click a model id to switch model.\n\n");

	modelsfd = aiopen("models", OREAD);
	if(modelsfd == nil) {
		winappendstr(w, "⚠ Cannot read 9ai/models\n");
	} else {
		while((n = fsread(modelsfd, buf, sizeof buf - 1)) > 0) {
			buf[n] = '\0';
			winappendstr(w, buf);
		}
		fsclose(modelsfd);
	}

	winclean(w);
	/* open event file on the models window so we can handle button-2 clicks */
	threadcreate(modelswinproc, w, STACK);
}

/*
 * cmd_sessions — open a +9ai/+sessions window listing available sessions.
 * Reads ~/.cache/9ai/sessions/ and formats one line per session.
 */
static void
cmd_sessions(void);   /* forward */

/* ── Sessions window ────────────────────────────────────────────────── */

/*
 * parsesessfile — read the first session and first prompt record from a
 * session file.  Populates model, ts, snippet (all static buffers).
 * Returns 0 on success, -1 if not a session file.
 */
static int
parsesessfile(char *path, char *uuid, char *model, char *ts, char *snippet)
{
	int     fd;
	Biobuf  b;
	char   *line;
	long    linelen;
	int     gotsess = 0, gotprompt = 0;

	uuid[0] = model[0] = ts[0] = snippet[0] = '\0';

	fd = open(path, OREAD);
	if(fd < 0)
		return -1;
	Binit(&b, fd, OREAD);

	while((!gotsess || !gotprompt) &&
	      (line = Brdline(&b, 0x1e)) != nil) {
		linelen = Blinelen(&b);
		char rec[512];
		char *fields[8];
		int   nf;
		long  cplen = linelen < (long)sizeof rec - 1 ? linelen : (long)sizeof rec - 1;
		memmove(rec, line, cplen);
		rec[cplen] = '\0';
		nf = splitrec(rec, cplen, fields, 8);
		if(nf < 1)
			continue;

		if(!gotsess && strcmp(fields[0], "session") == 0) {
			if(nf >= 2) strlcpy(uuid,  fields[1], 37);
			if(nf >= 3) strlcpy(model, fields[2], 64);
			if(nf >= 4) {
				/* unix timestamp → date string */
				long  t = atol(fields[3]);
				Tm   *tm = localtime(t);
				if(tm)
					snprint(ts, 32, "%04d-%02d-%02d %02d:%02d",
					        tm->year+1900, tm->mon+1, tm->mday,
					        tm->hour, tm->min);
				else
					strlcpy(ts, fields[3], 32);
			}
			gotsess = 1;

		} else if(gotsess && !gotprompt && strcmp(fields[0], "prompt") == 0) {
			if(nf >= 2) {
				/* first SNIPPET chars, collapsing whitespace */
				char *src = fields[1];
				int   dlen = 0;
				int   inspace = 0;
				while(*src && dlen < SNIPPET - 1) {
					if(*src == '\n' || *src == '\r' || *src == '\t')
						*src = ' ';
					if(*src == ' ' && inspace) { src++; continue; }
					inspace = (*src == ' ');
					snippet[dlen++] = *src++;
				}
				snippet[dlen] = '\0';
				if(*src) {
					/* truncated */
					if(dlen > 3) {
						snippet[dlen-1] = '.';
						snippet[dlen-2] = '.';
						snippet[dlen-3] = '.';
					}
				}
			}
			gotprompt = 1;
		}
	}

	Bterm(&b);
	close(fd);
	return gotsess ? 0 : -1;
}

static void
cmd_sessions(void)
{
	char  *sessdir;
	char  *home;
	Dir   *dirs;
	long   ndirs, i;
	Win   *w;
	char   name[256];
	int    dirfd;

	home = homedir();
	sessdir = smprint("%s/.cache/9ai/sessions", home);
	free(home);

	snprint(name, sizeof name, "%s/+9ai/+sessions", getenv("cwd") ? getenv("cwd") : ".");
	w = newwin(name);
	winappendstr(w, "# Middle-click a session UUID to load it.\n\n");

	dirfd = open(sessdir, OREAD);
	if(dirfd < 0) {
		winappendstr(w, "(no sessions found)\n");
		winclean(w);
		free(sessdir);
		threadcreate(sessionswinproc, w, STACK);
		return;
	}

	ndirs = dirreadall(dirfd, &dirs);
	close(dirfd);

	/* sort by mtime descending — newest first */
	/* simple insertion sort: fine for a few dozen sessions */
	for(i = 1; i < ndirs; i++) {
		Dir  tmp = dirs[i];
		long j   = i - 1;
		while(j >= 0 && dirs[j].mtime < tmp.mtime) {
			dirs[j+1] = dirs[j];
			j--;
		}
		dirs[j+1] = tmp;
	}

	for(i = 0; i < ndirs; i++) {
		if(dirs[i].name[0] == '.')
			continue;

		char path[512];
		char uuid[37], model[64], ts[32], snippet[SNIPPET + 4];

		snprint(path, sizeof path, "%s/%s", sessdir, dirs[i].name);
		if(parsesessfile(path, uuid, model, ts, snippet) < 0)
			continue;
		if(uuid[0] == '\0')
			continue;

		char line[256];
		snprint(line, sizeof line, "%s\t%s\t%s\t%s\n",
		        uuid, model, ts, snippet);
		winappendstr(w, line);
	}

	free(dirs);
	free(sessdir);

	winclean(w);
	threadcreate(sessionswinproc, w, STACK);
}

/* ── Models window event proc ───────────────────────────────────────── */

/*
 * modelswinproc — handle button-2 clicks in the +models window.
 *
 * Middle-clicking a word that looks like a model id switches the model.
 * Non-command events are passed back.
 */
static void
modelswinproc(void *v)
{
	Win       *w = v;
	char       buf[EVBUF];
	int        bufp = 0, nbuf = 0;
	AcmeEvent  e;

	for(;;) {
		if(!getevent(w->event, buf, &bufp, &nbuf, &e))
			break;

		if(e.c1 == 'M' && (e.c2 == 'x' || e.c2 == 'X')) {
			/* button-2 execute: treat text as model id */
			char *text = e.text;
			/* skip empty */
			while(*text == ' ') text++;
			if(*text == '\0') {
				if(e.flag & 1)
					writeevent(w->event, &e);
				continue;
			}
			/* looks like a model id if it contains no spaces and no # */
			int ismodel = 1;
			for(char *p = text; *p; p++)
				if(*p == ' ' || *p == '#' || *p == '\t') { ismodel = 0; break; }

			if(ismodel && e.c2 != 'X') {  /* Mx = user execute; MX = acme built-in */
				/* switch model: write to 9ai/model */
				CFid *modfd = aiopen("model", OWRITE);
				if(modfd != nil) {
					fswrite(modfd, text, strlen(text));
					fswrite(modfd, "\n", 1);
					fsclose(modfd);
					qlock(&statelk);
					strlcpy(curmodel, text, sizeof curmodel);
					qunlock(&statelk);
					setstatus("ready");
					char msg[128];
					snprint(msg, sizeof msg, "\n[Model → %s]\n", text);
					winappendstr(mainwin, msg);
				}
				windelete(w);
				break;
			} else {
				if(e.flag & 1)
					writeevent(w->event, &e);
			}
		} else if(e.c2 == 'L' || e.c2 == 'l') {
			if(e.flag & 1)
				writeevent(w->event, &e);
		} else {
			if(e.flag & 1)
				writeevent(w->event, &e);
		}
	}

	fsclose(w->event);
	fsclose(w->ctl);
	free(w);
}

/* ── Sessions window event proc ─────────────────────────────────────── */

/*
 * sessionswinproc — handle button-2 clicks in the +sessions window.
 *
 * Middle-clicking a UUID (36-char hex-and-dash string) loads that session.
 */
static void
sessionswinproc(void *v)
{
	Win       *w = v;
	char       buf[EVBUF];
	int        bufp = 0, nbuf = 0;
	AcmeEvent  e;

	for(;;) {
		if(!getevent(w->event, buf, &bufp, &nbuf, &e))
			break;

		if(e.c1 == 'M' && (e.c2 == 'x' || e.c2 == 'X')) {
			char *text = e.text;
			while(*text == ' ') text++;

			/* a UUID is 36 chars: 8-4-4-4-12 */
			int isUUID = (strlen(text) == 36 &&
			              text[8] == '-' && text[13] == '-' &&
			              text[18] == '-' && text[23] == '-');

			if(isUUID && e.c2 != 'X') {
				/* load session: write path to 9ai/session/load */
				char *home   = homedir();
				char *path   = smprint("%s/.cache/9ai/sessions/%s", home, text);
				free(home);

				CFid *loadfd = aiopen("session/load", OWRITE);
				if(loadfd != nil) {
					fswrite(loadfd, path, strlen(path));
					fsclose(loadfd);
					char msg[128];
					snprint(msg, sizeof msg, "\n[Loading session %.8s…]\n", text);
					winappendstr(mainwin, msg);
				}
				free(path);
				windelete(w);
				break;
			} else {
				if(e.flag & 1)
					writeevent(w->event, &e);
			}
		} else if(e.c2 == 'L' || e.c2 == 'l') {
			if(e.flag & 1)
				writeevent(w->event, &e);
		} else {
			if(e.flag & 1)
				writeevent(w->event, &e);
		}
	}

	fsclose(w->event);
	fsclose(w->ctl);
	free(w);
}

/* ── Main event proc ────────────────────────────────────────────────── */

static void
eventproc(void *v)
{
	Win       *w = v;
	char       buf[EVBUF];
	int        bufp = 0, nbuf = 0;
	AcmeEvent  e;

	for(;;) {
		if(!getevent(w->event, buf, &bufp, &nbuf, &e))
			break;

		if(e.c1 == 'M' && (e.c2 == 'x' || e.c2 == 'X')) {
			char *text = e.text;
			while(*text == ' ') text++;


			/* dispatch tag commands */
			if(strcmp(text, "Send")     == 0) { cmd_send();     continue; }
			if(strcmp(text, "Stop")     == 0) { cmd_stop();     continue; }
			if(strcmp(text, "Steer")    == 0) { cmd_steer();    continue; }
			if(strcmp(text, "New")      == 0) { cmd_new();      continue; }
			if(strcmp(text, "Clear")    == 0) { cmd_clear();    continue; }
			if(strcmp(text, "Models")   == 0) { cmd_models();   continue; }
			if(strcmp(text, "Sessions") == 0) { cmd_sessions(); continue; }

			/* unrecognised: pass back if acme can handle it */
			if(e.flag & 1)
				writeevent(w->event, &e);

		} else if(e.c2 == 'L' || e.c2 == 'l') {
			/* look-up: pass back for normal plumbing */
			if(e.flag & 1)
				writeevent(w->event, &e);
		} else {
			/* all other events: pass back if acme can handle */
			if(e.flag & 1)
				writeevent(w->event, &e);
		}
	}

	/* window was closed */
	threadexitsall(nil);
}

/* ── threadmain ─────────────────────────────────────────────────────── */

static void
readinitialstate(void)
{
	CFid *ctlfd;
	char  buf[512];
	int   n;

	ctlfd = aiopen("ctl", OREAD);
	if(ctlfd == nil)
		return;

	n = fsread(ctlfd, buf, sizeof buf - 1);
	fsclose(ctlfd);
	if(n <= 0)
		return;
	buf[n] = '\0';

	/*
	 * Parse:
	 *   model <id>
	 *   status <state>
	 *   session <uuid>
	 */
	char *p = buf;
	while(*p) {
		char *eol = strchr(p, '\n');
		if(eol) *eol = '\0';

		if(strncmp(p, "model ", 6) == 0) {
			qlock(&statelk);
			strlcpy(curmodel, p + 6, sizeof curmodel);
			qunlock(&statelk);
		} else if(strncmp(p, "session ", 8) == 0) {
			char *sid = p + 8;
			if(strcmp(sid, "none") != 0) {
				qlock(&statelk);
				strlcpy(cursession, sid, sizeof cursession);
				qunlock(&statelk);
			}
		}

		if(eol) p = eol + 1;
		else     break;
	}
}

/* ── threadmain ─────────────────────────────────────────────────────── */

static void
usage(void)
{
	fprint(2, "usage: 9ai-acme\n");
	threadexitsall("usage");
}

void
threadmain(int argc, char *argv[])
{
	char *cwd;
	char  winname[512];

	ARGBEGIN {
	default:
		usage();
	} ARGEND

	USED(argc); USED(argv);

	/* connect to acme */
	acmefs = nsmount("acme", nil);
	if(acmefs == nil)
		sysfatal("cannot connect to acme: %r");

	/* connect to 9ai */
	aifsys = nsmount("9ai", nil);
	if(aifsys == nil)
		sysfatal("cannot connect to 9ai: %r\n"
		         "(start 9ai first: 9ai -m /mnt/9ai)");

	/* bootstrap model/session state from ctl */
	readinitialstate();

	/* create the +9ai window */
	cwd = getenv("cwd");
	if(cwd == nil) {
		cwd = malloc(512);
		if(cwd == nil) sysfatal("malloc: %r");
		if(getwd(cwd, 512) == nil)
			strlcpy(cwd, ".", 2);
	}
	snprint(winname, sizeof winname, "%s/+9ai", cwd);

	mainwin = newwin(winname);

	/* write initial status line */
	winappendstr(mainwin, "● ready\n");
	setstatus("ready");
	winclean(mainwin);

	/* start reader procs */
	threadcreate(aiproc,  nil, STACK);
	threadcreate(outproc, nil, STACK);

	/* run event loop in this thread */
	eventproc(mainwin);
}
