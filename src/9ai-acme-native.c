/*
 * 9ai-acme-native.c — acme integration for 9ai (9front native)
 *
 * 9front version: uses /mnt/acme and /mnt/9ai directly via open/read/write.
 * No lib9pclient, no CFsys/CFid.
 *
 * Usage:
 *   9ai-acme
 *
 * Creates a +9ai window in the current directory.  Requires:
 *   - 9ai running with -m /mnt/9ai
 *   - acme running (mounts itself at /mnt/acme)
 *
 * Tag commands:
 *   Send     — send the last paragraph as a prompt
 *   Stop     — abort the current turn (writes "abort" to /mnt/9ai/ctl)
 *   Steer    — redirect the running turn with the last paragraph
 *   New      — start a fresh session (writes to /mnt/9ai/session/new)
 *   Clear    — clear the in-memory history (writes "clear" to /mnt/9ai/ctl)
 *   Models   — open a model picker window
 *   Sessions — open a session picker window
 *
 * ── Architecture ──────────────────────────────────────────────────────
 *
 * The acme file API on native Plan 9 / 9front:
 *   /mnt/acme/new/ctl    — create a new window; read yields window id
 *   /mnt/acme/<id>/ctl   — control file (name, cleartag, clean, delete, ...)
 *   /mnt/acme/<id>/tag   — tag bar text
 *   /mnt/acme/<id>/body  — body text (read/write)
 *   /mnt/acme/<id>/addr  — address (set before data access)
 *   /mnt/acme/<id>/data  — body bytes at current address
 *   /mnt/acme/<id>/event — keyboard/mouse events (ORDWR)
 *
 * The 9ai API at /mnt/9ai/:
 *   ctl, message, steer, output, event, status, model, models,
 *   session/id, session/new, session/load, session/save
 *
 * Three procs run concurrently:
 *   outproc   — reads /mnt/9ai/output per turn; appends to window body
 *   aiproc    — reads /mnt/9ai/event per turn; appends formatted lines
 *   eventproc — reads /mnt/acme/<id>/event; dispatches tag commands
 *
 * All window writes are serialised with winlk.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>

#include "9ai.h"   /* homedir(), configpath() */

/* ── Constants ──────────────────────────────────────────────────────── */

enum {
	STACK      = 65536,
	EVBUF      = 8192,
	RECBUF     = 131072,
	TEXTBUF    = 131072,
	MAXFIELDS  = 64,
	SNIPPET    = 80,
};

#define TAG_EXTRA  " | Send Stop Steer New Clear Models Sessions"
#define SEPARATOR  "\n\n════════════════════════════════════════════════════════════\n\n"
#define ACME       "/mnt/acme"
#define AI9        "/mnt/9ai"

/* ── Window state ───────────────────────────────────────────────────── */

typedef struct Win Win;
struct Win {
	int id;
};

static Win  *mainwin;
static QLock winlk;

/* ── Forward declarations ───────────────────────────────────────────── */

static void modelswinproc(void *v);
static void sessionswinproc(void *v);

/* ── Low-level acme file access ─────────────────────────────────────── */

/*
 * acmepath — build an acme file path.
 */
static char *
acmepath(int id, char *file)
{
	if(id == 0)
		return smprint("%s/new/%s", ACME, file);
	return smprint("%s/%d/%s", ACME, id, file);
}

/*
 * acmeopen — open a window file.
 */
static int
acmeopen(int id, char *file, int mode)
{
	char *path = acmepath(id, file);
	int   fd   = open(path, mode);
	free(path);
	return fd;
}

/*
 * acmewrite — write to a window file, returning bytes written or -1.
 */
static int
acmewrite(int id, char *file, char *data, int len)
{
	int fd, n;

	if(len < 0)
		len = strlen(data);
	fd = acmeopen(id, file, OWRITE);
	if(fd < 0)
		return -1;
	n = write(fd, data, len);
	close(fd);
	return n;
}

/*
 * newwin — create a new acme window, set its name and tag.
 * Returns a new Win*.
 */
static Win *
newwin(char *name)
{
	Win *w;
	int  fd, n;
	char buf[32];

	w = mallocz(sizeof *w, 1);
	if(w == nil)
		sysfatal("mallocz Win: %r");

	fd = acmeopen(0, "ctl", ORDWR);
	if(fd < 0)
		sysfatal("open %s/new/ctl: %r", ACME);
	n = read(fd, buf, sizeof buf - 1);
	if(n < 1)
		sysfatal("read new/ctl: %r");
	buf[n < (int)sizeof buf ? n : (int)sizeof buf - 1] = '\0';
	close(fd);

	w->id = atoi(buf);

	/* name the window */
	acmewrite(w->id, "ctl", smprint("name %s\n", name), -1);

	/* set tag */
	acmewrite(w->id, "ctl", "cleartag\n", -1);
	acmewrite(w->id, "tag", TAG_EXTRA, strlen(TAG_EXTRA));

	return w;
}

/*
 * winappend — append text to the main window body.
 * Uses addr/data so we always append at end regardless of selection.
 */
static void
winappend(Win *w, char *text, int len)
{
	int afd, dfd;

	if(len < 0)
		len = strlen(text);
	if(len == 0)
		return;

	qlock(&winlk);
	afd = acmeopen(w->id, "addr", OWRITE);
	dfd = acmeopen(w->id, "data", ORDWR);
	if(afd >= 0 && dfd >= 0) {
		write(afd, "$", 1);
		write(dfd, text, len);
	}
	if(afd >= 0) close(afd);
	if(dfd >= 0) close(dfd);
	qunlock(&winlk);
}

static void
winappendstr(Win *w, char *s)
{
	winappend(w, s, -1);
}

static void
winclean(Win *w)
{
	acmewrite(w->id, "ctl", "clean\n", -1);
}

static void
windelete(Win *w)
{
	acmewrite(w->id, "ctl", "delete\n", -1);
}

/*
 * winbody — read the entire window body.  Caller must free.
 */
static char *
winbody(Win *w, int *np)
{
	int   fd;
	char *s;
	int   n, na, m;

	fd = acmeopen(w->id, "body", OREAD);
	if(fd < 0)
		return strdup("");

	s  = nil;
	na = 0;
	n  = 0;
	for(;;) {
		if(na < n + 512) {
			na += 4096;
			s = realloc(s, na + 1);
			if(s == nil) sysfatal("realloc winbody: %r");
		}
		m = read(fd, s + n, na - n);
		if(m <= 0)
			break;
		n += m;
	}
	close(fd);
	if(s == nil)
		s = strdup("");
	else
		s[n] = '\0';
	if(np) *np = n;
	return s;
}

/* ── Status line ────────────────────────────────────────────────────── */

static char curmodel[128];
static char cursession[37];
static QLock statelk;

static void
setstatus(char *state)
{
	char buf[256];
	char modstr[144];
	char sesstr[16];
	int  afd, dfd;

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

	qlock(&winlk);
	afd = acmeopen(mainwin->id, "addr", OWRITE);
	dfd = acmeopen(mainwin->id, "data", ORDWR);
	if(afd >= 0 && dfd >= 0) {
		write(afd, "1", 1);
		write(dfd, buf, strlen(buf));
	}
	if(afd >= 0) close(afd);
	if(dfd >= 0) close(dfd);
	qunlock(&winlk);
}

/* ── 9ai file access ────────────────────────────────────────────────── */

static int
aiopen(char *file, int mode)
{
	char path[256];
	snprint(path, sizeof path, "%s/%s", AI9, file);
	return open(path, mode);
}

static int
aiwrite(char *file, char *data, int len)
{
	int fd, n;

	if(len < 0)
		len = strlen(data);
	fd = aiopen(file, OWRITE);
	if(fd < 0)
		return -1;
	n = write(fd, data, len);
	close(fd);
	return n;
}

/* ── RS/FS record parser ────────────────────────────────────────────── */

/*
 * splitrec — NUL-patches buf in place, returns pointers into it.
 * reclen includes the trailing RS (0x1E) if present.
 */
static int
splitrec(char *rec, int reclen, char **fields, int maxfields)
{
	char *p, *end, *start;
	int   nf = 0;

	end = rec + reclen;
	if(end > rec && (uchar)*(end-1) == 0x1e)
		end--;
	*end = '\0';

	p = rec;
	while(p <= end && nf < maxfields) {
		start = p;
		while(p < end && (uchar)*p != 0x1f)
			p++;
		if(p < end)
			*p++ = '\0';
		else
			p++;
		fields[nf++] = start;
	}
	return nf;
}

/* ── strecpy-based safe copy helper ─────────────────────────────────── */

static void
scopy(char *dst, int dsz, char *src)
{
	strecpy(dst, dst + dsz, src);
}

/* ── 9ai /event reader proc ─────────────────────────────────────────── */

static void
display_tool_start(char **fields, int nf)
{
	char buf[1024];
	char args[512];
	int  i, alen = 0;

	args[0] = '\0';
	for(i = 3; i < nf; i++) {
		int slen = strlen(fields[i]);
		if(i > 3 && alen + 1 < (int)sizeof args - 1) {
			args[alen++] = ' ';
			args[alen] = '\0';
		}
		if(alen + slen < (int)sizeof args - 1) {
			memmove(args + alen, fields[i], slen);
			alen += slen;
			args[alen] = '\0';
		}
	}
	if(alen > 200)
		memmove(args + 197, "...\0", 4);

	snprint(buf, sizeof buf, "\n┌ ⚙ %s %s\n",
	        nf > 1 ? fields[1] : "?", args);
	winappendstr(mainwin, buf);
}

static void
display_tool_end(char **fields, int nf)
{
	int is_err = (nf > 1 && strcmp(fields[1], "err") == 0);
	if(is_err) {
		char buf[128];
		char *out = (nf > 2 && fields[2][0]) ? fields[2] : "";
		char  first[80];
		int   i;
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
	int   fd;
	char  buf[RECBUF];
	int   n;

	USED(v);

	for(;;) {
		fd = aiopen("event", OREAD);
		if(fd < 0) {
			sleep(2000);
			continue;
		}

		for(;;) {
			n = read(fd, buf, sizeof buf - 1);
			if(n <= 0)
				break;
			buf[n] = '\0';

			char rec[RECBUF];
			char *fields[MAXFIELDS];
			int   nf;

			memmove(rec, buf, n + 1);
			nf = splitrec(rec, n, fields, MAXFIELDS);
			if(nf == 0)
				continue;

			char *type = fields[0];

			if(strcmp(type, "turn_start") == 0) {
				if(nf >= 3) {
					qlock(&statelk);
					scopy(curmodel, sizeof curmodel, fields[2]);
					qunlock(&statelk);
				}
				setstatus("running");

			} else if(strcmp(type, "thinking") == 0) {
				if(nf >= 2) {
					char *chunk = fields[1];
					char  out[RECBUF + 16];
					int   olen = 0;
					out[olen++] = '\n';
					/* U+2502 BOX DRAWINGS LIGHT VERTICAL */
					out[olen++] = (char)0xe2;
					out[olen++] = (char)0x94;
					out[olen++] = (char)0x82;
					out[olen++] = ' ';
					for(int i = 0; chunk[i] && olen < (int)sizeof out - 8; i++) {
						out[olen++] = chunk[i];
						if(chunk[i] == '\n' && chunk[i+1]) {
							out[olen++] = (char)0xe2;
							out[olen++] = (char)0x94;
							out[olen++] = (char)0x82;
							out[olen++] = ' ';
						}
					}
					winappend(mainwin, out, olen);
				}

			} else if(strcmp(type, "tool_start") == 0) {
				display_tool_start(fields, nf);

			} else if(strcmp(type, "tool_end") == 0) {
				display_tool_end(fields, nf);

			} else if(strcmp(type, "turn_end") == 0) {
				char *reason = (nf >= 2) ? fields[1] : "end_turn";
				if(strcmp(reason, "aborted") == 0)
					winappendstr(mainwin, "\n⛔ Aborted.\n");
				else if(strcmp(reason, "error") == 0)
					winappendstr(mainwin, "\n⚠ Agent error.\n");
				setstatus("ready");

			} else if(strcmp(type, "model") == 0) {
				if(nf >= 2) {
					qlock(&statelk);
					scopy(curmodel, sizeof curmodel, fields[1]);
					qunlock(&statelk);
				}
				setstatus("ready");

			} else if(strcmp(type, "session_new") == 0) {
				if(nf >= 2) {
					qlock(&statelk);
					scopy(cursession, sizeof cursession, fields[1]);
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

		close(fd);
		/* re-open for next turn */
	}
}

/* ── 9ai /output reader proc ────────────────────────────────────────── */

static void
outproc(void *v)
{
	int  fd;
	char buf[TEXTBUF];
	int  n;

	USED(v);

	for(;;) {
		fd = aiopen("output", OREAD);
		if(fd < 0) {
			sleep(2000);
			continue;
		}

		for(;;) {
			n = read(fd, buf, sizeof buf - 1);
			if(n < 0)
				break;
			if(n == 0)
				break;
			buf[n] = '\0';

			if(strcmp(buf, "[done]\n") == 0) {
				winappendstr(mainwin, SEPARATOR);
				winclean(mainwin);
				break;
			}
			winappend(mainwin, buf, n);
		}

		close(fd);
	}
}

/* ── Acme event reader ──────────────────────────────────────────────── */

typedef struct AcmeEvent AcmeEvent;
struct AcmeEvent {
	int  c1, c2;
	int  q0, q1;
	int  eq0, eq1;
	int  flag, nr;
	char text[512];
};

static int
evgetc(int fd, char *buf, int *bufp, int *nbuf)
{
	if(*bufp >= *nbuf) {
		*nbuf = read(fd, buf, EVBUF);
		*bufp = 0;
		if(*nbuf <= 0)
			return -1;
	}
	return (uchar)buf[(*bufp)++];
}

static int
evgetn(int fd, char *buf, int *bufp, int *nbuf)
{
	int c, n = 0;
	while((c = evgetc(fd, buf, bufp, nbuf)) >= '0' && c <= '9')
		n = n * 10 + c - '0';
	if(c != ' ')
		return -1;
	return n;
}

static Rune
evgetrune(int fd, char *buf, int *bufp, int *nbuf, char *dst, int *dlen, int dsz)
{
	Rune r;
	int  c, nb;

	if(*dlen >= dsz - 4)
		return -1;

	c = evgetc(fd, buf, bufp, nbuf);
	if(c < 0)
		return -1;
	dst[*dlen] = c;
	nb = 1;
	if(c >= Runeself) {
		while(!fullrune(dst + *dlen, nb)) {
			c = evgetc(fd, buf, bufp, nbuf);
			if(c < 0)
				return -1;
			dst[*dlen + nb++] = c;
		}
	}
	chartorune(&r, dst + *dlen);
	*dlen += nb;
	return r;
}

static int
getevent(int fd, char *buf, int *bufp, int *nbuf, AcmeEvent *e)
{
	int  i, c;
	char tbuf[512];
	int  tlen;

	e->c1 = evgetc(fd, buf, bufp, nbuf);
	if(e->c1 < 0)
		return 0;
	e->c2 = evgetc(fd, buf, bufp, nbuf);
	if(e->c2 < 0)
		return 0;

	e->q0   = evgetn(fd, buf, bufp, nbuf);
	e->q1   = evgetn(fd, buf, bufp, nbuf);
	e->flag = evgetn(fd, buf, bufp, nbuf);
	e->nr   = evgetn(fd, buf, bufp, nbuf);

	if(e->q0 < 0 || e->q1 < 0 || e->flag < 0 || e->nr < 0)
		return 0;

	e->eq0 = e->q0;
	e->eq1 = e->q1;

	if(e->nr > 256)
		e->nr = 256;

	tlen = 0;
	for(i = 0; i < e->nr; i++) {
		if(evgetrune(fd, buf, bufp, nbuf, e->text, &tlen, sizeof e->text) < 0)
			return 0;
	}
	e->text[tlen] = '\0';

	c = evgetc(fd, buf, bufp, nbuf);
	if(c != '\n')
		return 0;

	if(e->flag & 2) {
		int xq0, xq1, xflag, xnr;
		evgetc(fd, buf, bufp, nbuf);
		evgetc(fd, buf, bufp, nbuf);
		xq0   = evgetn(fd, buf, bufp, nbuf);
		xq1   = evgetn(fd, buf, bufp, nbuf);
		xflag = evgetn(fd, buf, bufp, nbuf);
		xnr   = evgetn(fd, buf, bufp, nbuf);
		e->eq0 = xq0;
		e->eq1 = xq1;
		USED(xflag);
		tlen = 0;
		if(xnr > 256) xnr = 256;
		for(i = 0; i < xnr; i++)
			evgetrune(fd, buf, bufp, nbuf, e->text, &tlen, sizeof e->text);
		e->text[tlen] = '\0';
		evgetc(fd, buf, bufp, nbuf);
	}

	if(e->flag & 8) {
		int pass;
		for(pass = 0; pass < 2; pass++) {
			evgetc(fd, buf, bufp, nbuf);
			evgetc(fd, buf, bufp, nbuf);
			evgetn(fd, buf, bufp, nbuf);
			evgetn(fd, buf, bufp, nbuf);
			evgetn(fd, buf, bufp, nbuf);
			int nr2 = evgetn(fd, buf, bufp, nbuf);
			if(nr2 > 256) nr2 = 256;
			tlen = 0;
			for(i = 0; i < nr2; i++)
				evgetrune(fd, buf, bufp, nbuf, tbuf, &tlen, sizeof tbuf);
			evgetc(fd, buf, bufp, nbuf);
		}
	}

	return 1;
}

static void
writeevent(int evfd, AcmeEvent *e)
{
	char buf[64];
	int  n;
	n = snprint(buf, sizeof buf, "%c%c%d %d\n", e->c1, e->c2, e->q0, e->q1);
	write(evfd, buf, n);
}

/* ── Prompt text extraction ─────────────────────────────────────────── */

static char *
prompttext(Win *w)
{
	char *body;
	int   bodylen;
	char *after;
	char *p;
	char *result;
	int   rlen;

	/* try dot selection first */
	{
		int   afd, dfd;
		char  buf[8192];
		int   n;

		afd = acmeopen(w->id, "addr", ORDWR);
		dfd = acmeopen(w->id, "data", ORDWR);
		if(afd >= 0 && dfd >= 0) {
			write(afd, ".", 1);
			n = read(dfd, buf, sizeof buf - 1);
			if(n > 0) {
				buf[n] = '\0';
				char *s = buf, *e2 = buf + n - 1;
				while(*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
				while(e2 > s && (*e2 == ' ' || *e2 == '\t' || *e2 == '\n' || *e2 == '\r')) e2--;
				*(e2+1) = '\0';
				if(*s != '\0') {
					if(afd >= 0) close(afd);
					if(dfd >= 0) close(dfd);
					return strdup(s);
				}
			}
		}
		if(afd >= 0) close(afd);
		if(dfd >= 0) close(dfd);
	}

	body = winbody(w, &bodylen);
	if(body == nil)
		return nil;

	after = nil;
	p     = body;

	while(*p) {
		char *eol = strchr(p, '\n');
		if(eol == nil) eol = p + strlen(p);
		int linelen = eol - p;
		if(linelen >= 3) {
			uchar a = (uchar)p[0], b = (uchar)p[1], c = (uchar)p[2];
			int is_sep    = (a == 0xe2 && b == 0x95 && c == 0x90); /* ═ */
			int is_echo   = (a == 0xe2 && b == 0x96 && c == 0xb6); /* ▶ */
			int is_status = (a == 0xe2 && b == 0x97 && c == 0x8f); /* ● */
			int is_tool   = (a == 0xe2 && b == 0x94 && c == 0x8c); /* ┌ */
			int is_steer  = (a == 0xe2 && b == 0x86 && c == 0xa9); /* ↩ */
			if(is_sep || is_echo || is_status || is_tool || is_steer)
				after = (*eol) ? eol + 1 : eol;
		}
		p = (*eol) ? eol + 1 : eol;
	}

	if(after == nil) {
		after = strchr(body, '\n');
		after = after ? after + 1 : body;
	}
	while(*after == '\n' || *after == '\r' || *after == ' ' || *after == '\t')
		after++;

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
	int   fd;
	int   n;

	text = prompttext(mainwin);
	if(text == nil || text[0] == '\0') {
		free(text);
		return;
	}

	fd = aiopen("message", OWRITE);
	if(fd < 0) {
		winappendstr(mainwin, "\n⚠ 9ai not available: cannot open /mnt/9ai/message\n");
		free(text);
		return;
	}

	{
		char *display = smprint("\n▶ %s\n", text);
		winappendstr(mainwin, display);
		free(display);
	}

	n = strlen(text);
	write(fd, text, n);
	close(fd);   /* clunk triggers agent turn */
	setstatus("running");

	free(text);
}

static void
cmd_stop(void)
{
	if(aiwrite("ctl", "abort\n", -1) < 0)
		winappendstr(mainwin, "\n⚠ 9ai not available\n");
	else
		setstatus("aborting");
}

static void
cmd_steer(void)
{
	char *text;
	int   fd;

	text = prompttext(mainwin);
	if(text == nil || text[0] == '\0') {
		free(text);
		return;
	}

	fd = aiopen("steer", OWRITE);
	if(fd < 0) {
		winappendstr(mainwin, "\n⚠ 9ai not available\n");
		free(text);
		return;
	}
	{
		char *display = smprint("\n↩ Steer: %s\n", text);
		winappendstr(mainwin, display);
		free(display);
	}
	write(fd, text, strlen(text));
	close(fd);
	free(text);
}

static void
cmd_new(void)
{
	if(aiwrite("session/new", "new", -1) < 0)
		winappendstr(mainwin, "\n⚠ 9ai not available\n");
	else
		winappendstr(mainwin, "\n── New session ──\n");
}

static void
cmd_clear(void)
{
	int  afd, dfd;

	if(aiwrite("ctl", "clear\n", -1) < 0) {
		winappendstr(mainwin, "\n⚠ 9ai not available\n");
		return;
	}

	/* erase body lines 2..$ */
	qlock(&winlk);
	afd = acmeopen(mainwin->id, "addr", OWRITE);
	dfd = acmeopen(mainwin->id, "data", ORDWR);
	if(afd >= 0 && dfd >= 0) {
		write(afd, "2,$", 3);
		write(dfd, "", 0);
	}
	if(afd >= 0) close(afd);
	if(dfd >= 0) close(dfd);
	qunlock(&winlk);

	winclean(mainwin);
	setstatus("ready");
}

static void
cmd_models(void)
{
	Win  *w;
	int   fd;
	char  buf[4096];
	int   n;
	char *cwd;
	char  name[512];

	cwd = getenv("cwd");
	snprint(name, sizeof name, "%s/+9ai/+models", cwd ? cwd : ".");

	w = newwin(name);
	winappendstr(w, "# Middle-click a model id to switch model.\n\n");

	fd = aiopen("models", OREAD);
	if(fd < 0) {
		winappendstr(w, "⚠ Cannot read /mnt/9ai/models\n");
	} else {
		while((n = read(fd, buf, sizeof buf - 1)) > 0) {
			buf[n] = '\0';
			winappendstr(w, buf);
		}
		close(fd);
	}

	winclean(w);
	threadcreate(modelswinproc, w, STACK);
}

/* ── Session listing ────────────────────────────────────────────────── */

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
		char  rec[512];
		char *fields[8];
		int   nf;
		long  cplen = linelen < (long)sizeof rec - 1 ? linelen : (long)sizeof rec - 1;
		memmove(rec, line, cplen);
		rec[cplen] = '\0';
		nf = splitrec(rec, cplen, fields, 8);
		if(nf < 1)
			continue;

		if(!gotsess && strcmp(fields[0], "session") == 0) {
			if(nf >= 2) scopy(uuid,  37,  fields[1]);
			if(nf >= 3) scopy(model, 64,  fields[2]);
			if(nf >= 4) {
				long  t  = atol(fields[3]);
				Tm   *tm = localtime(t);
				if(tm)
					snprint(ts, 32, "%04d-%02d-%02d %02d:%02d",
					        tm->year+1900, tm->mon+1, tm->mday,
					        tm->hour, tm->min);
				else
					scopy(ts, 32, fields[3]);
			}
			gotsess = 1;

		} else if(gotsess && !gotprompt && strcmp(fields[0], "prompt") == 0) {
			if(nf >= 2) {
				char *src  = fields[1];
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
				if(*src && dlen > 3) {
					snippet[dlen-1] = '.';
					snippet[dlen-2] = '.';
					snippet[dlen-3] = '.';
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
	int    dirfd;
	Dir   *dirs;
	long   ndirs, i;
	Win   *w;
	char  *cwd;
	char   name[256];

	sessdir = configpath("sessions");

	cwd = getenv("cwd");
	snprint(name, sizeof name, "%s/+9ai/+sessions", cwd ? cwd : ".");
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

	/* sort by mtime descending */
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
		char path[512];
		char uuid[37], model[64], ts[32], snippet[SNIPPET + 4];

		if(dirs[i].name[0] == '.')
			continue;
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

/* ── Models window proc ─────────────────────────────────────────────── */

static void
modelswinproc(void *v)
{
	Win       *w = v;
	int        fd;
	char       buf[EVBUF];
	int        bufp = 0, nbuf = 0;
	AcmeEvent  e;

	fd = acmeopen(w->id, "event", ORDWR);
	if(fd < 0) {
		free(w);
		return;
	}

	for(;;) {
		if(!getevent(fd, buf, &bufp, &nbuf, &e))
			break;

		if(e.c1 == 'M' && (e.c2 == 'x' || e.c2 == 'X')) {
			char *text = e.text;
			while(*text == ' ') text++;

			int ismodel = (*text != '\0' && *text != '#');
			if(ismodel) {
				for(char *p = text; *p; p++)
					if(*p == ' ' || *p == '\t') { ismodel = 0; break; }
			}

			if(ismodel && e.c2 != 'X') {
				char newmodel[128];
				snprint(newmodel, sizeof newmodel, "%s\n", text);
				if(aiwrite("model", newmodel, -1) >= 0) {
					qlock(&statelk);
					scopy(curmodel, sizeof curmodel, text);
					qunlock(&statelk);
					setstatus("ready");
					char msg[128];
					snprint(msg, sizeof msg, "\n[Model → %s]\n", text);
					winappendstr(mainwin, msg);
				}
				windelete(w);
				close(fd);
				free(w);
				return;
			} else {
				if(e.flag & 1) writeevent(fd, &e);
			}
		} else {
			if(e.flag & 1) writeevent(fd, &e);
		}
	}

	close(fd);
	free(w);
}

/* ── Sessions window proc ───────────────────────────────────────────── */

static void
sessionswinproc(void *v)
{
	Win       *w = v;
	int        fd;
	char       buf[EVBUF];
	int        bufp = 0, nbuf = 0;
	AcmeEvent  e;

	fd = acmeopen(w->id, "event", ORDWR);
	if(fd < 0) {
		free(w);
		return;
	}

	for(;;) {
		if(!getevent(fd, buf, &bufp, &nbuf, &e))
			break;

		if(e.c1 == 'M' && (e.c2 == 'x' || e.c2 == 'X')) {
			char *text = e.text;
			while(*text == ' ') text++;

			int isUUID = (strlen(text) == 36 &&
			              text[8] == '-' && text[13] == '-' &&
			              text[18] == '-' && text[23] == '-');

			if(isUUID && e.c2 != 'X') {
				char *path = smprint("%s/%s", AI9, text);
				char *sessdir = configpath("sessions/");
				free(path);
				path = smprint("%s%s", sessdir, text);
				free(sessdir);

				int lfd = aiopen("session/load", OWRITE);
				if(lfd >= 0) {
					write(lfd, path, strlen(path));
					close(lfd);
					char msg[128];
					snprint(msg, sizeof msg, "\n[Loading session %.8s…]\n", text);
					winappendstr(mainwin, msg);
				}
				free(path);
				windelete(w);
				close(fd);
				free(w);
				return;
			} else {
				if(e.flag & 1) writeevent(fd, &e);
			}
		} else {
			if(e.flag & 1) writeevent(fd, &e);
		}
	}

	close(fd);
	free(w);
}

/* ── Main event proc ────────────────────────────────────────────────── */

static void
eventproc(void *v)
{
	Win       *w = v;
	int        fd;
	char       buf[EVBUF];
	int        bufp = 0, nbuf = 0;
	AcmeEvent  e;

	fd = acmeopen(w->id, "event", ORDWR);
	if(fd < 0)
		sysfatal("open event: %r");

	for(;;) {
		if(!getevent(fd, buf, &bufp, &nbuf, &e))
			break;

		if(e.c1 == 'M' && (e.c2 == 'x' || e.c2 == 'X')) {
			char *text = e.text;
			while(*text == ' ') text++;

			if(strcmp(text, "Send")     == 0) { cmd_send();     continue; }
			if(strcmp(text, "Stop")     == 0) { cmd_stop();     continue; }
			if(strcmp(text, "Steer")    == 0) { cmd_steer();    continue; }
			if(strcmp(text, "New")      == 0) { cmd_new();      continue; }
			if(strcmp(text, "Clear")    == 0) { cmd_clear();    continue; }
			if(strcmp(text, "Models")   == 0) { cmd_models();   continue; }
			if(strcmp(text, "Sessions") == 0) { cmd_sessions(); continue; }

			if(e.flag & 1) writeevent(fd, &e);

		} else if(e.c2 == 'L' || e.c2 == 'l') {
			if(e.flag & 1) writeevent(fd, &e);
		} else {
			if(e.flag & 1) writeevent(fd, &e);
		}
	}

	close(fd);
	threadexitsall(nil);
}

/* ── Initial state fetch ─────────────────────────────────────────────── */

static void
readinitialstate(void)
{
	int  fd;
	char buf[512];
	int  n;
	char *p;

	fd = aiopen("ctl", OREAD);
	if(fd < 0)
		return;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		return;
	buf[n] = '\0';

	p = buf;
	while(*p) {
		char *eol = strchr(p, '\n');
		if(eol) *eol = '\0';

		if(strncmp(p, "model ", 6) == 0) {
			qlock(&statelk);
			scopy(curmodel, sizeof curmodel, p + 6);
			qunlock(&statelk);
		} else if(strncmp(p, "session ", 8) == 0) {
			char *sid = p + 8;
			if(strcmp(sid, "none") != 0) {
				qlock(&statelk);
				scopy(cursession, sizeof cursession, sid);
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

	/* verify /mnt/acme is reachable */
	{
		int fd = open(ACME "/index", OREAD);
		if(fd < 0)
			sysfatal("cannot reach acme at %s: %r", ACME);
		close(fd);
	}

	/* verify /mnt/9ai is reachable */
	{
		int fd = open(AI9 "/ctl", OREAD);
		if(fd < 0)
			sysfatal("cannot reach 9ai at %s: %r\n"
			         "(start 9ai first: 9ai -m /mnt/9ai)", AI9);
		close(fd);
	}

	readinitialstate();

	cwd = getenv("cwd");
	if(cwd == nil)
		cwd = ".";
	snprint(winname, sizeof winname, "%s/+9ai", cwd);

	mainwin = newwin(winname);
	winappendstr(mainwin, "● ready\n");
	setstatus("ready");
	winclean(mainwin);

	threadcreate(aiproc,  nil, STACK);
	threadcreate(outproc, nil, STACK);

	eventproc(mainwin);
}
