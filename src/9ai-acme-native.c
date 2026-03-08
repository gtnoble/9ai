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
#include "record.h"
#include "render.h"
#include "sessfile.h"
#include "acmeevent.h"
#include "prompt.h"

/* ── Constants ──────────────────────────────────────────────────────── */

enum {
	STACK      = 65536,
	RECBUF     = 131072,
	TEXTBUF    = 131072,
	MAXFIELDS  = 64,
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

/* ── strecpy-based safe copy helper ─────────────────────────────────── */

static void
scopy(char *dst, int dsz, char *src)
{
	strecpy(dst, dst + dsz, src);
}

/* ── 9ai /event reader proc ─────────────────────────────────────────── */

static void
aiproc(void *v)
{
	int   fd;
	char  *buf;
	char  *rec;
	char *fields[MAXFIELDS];
	int   nf;
	char *type;
	int   n;

	USED(v);

	buf = malloc(RECBUF);
	rec = malloc(RECBUF);
	if(buf == nil || rec == nil)
		sysfatal("aiproc: malloc: %r");

	for(;;) {
		fd = aiopen("event", OREAD);
		if(fd < 0) {
			sleep(2000);
			continue;
		}

		for(;;) {
			n = read(fd, buf, RECBUF - 1);
			if(n <= 0)
				break;
			buf[n] = '\0';

			memmove(rec, buf, n + 1);
			nf = splitrec(rec, n, fields, MAXFIELDS);
			if(nf == 0)
				continue;

			type = fields[0];

			if(strcmp(type, "turn_start") == 0) {
				if(nf >= 3) {
					qlock(&statelk);
					scopy(curmodel, sizeof curmodel, fields[2]);
					qunlock(&statelk);
				}
				setstatus("running");

			} else if(strcmp(type, "thinking") == 0) {
				if(nf >= 2) {
					char *out = render_thinking(fields[1]);
					if(out != nil) {
						winappendstr(mainwin, out);
						free(out);
					}
				}

			} else if(strcmp(type, "tool_start") == 0) {
				char *out = render_tool_start(fields, nf);
				if(out != nil) { winappendstr(mainwin, out); free(out); }

			} else if(strcmp(type, "tool_end") == 0) {
				char *out = render_tool_end(fields, nf);
				if(out != nil) { winappendstr(mainwin, out); free(out); }

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
	char *buf;
	int  n;

	USED(v);

	buf = malloc(TEXTBUF);
	if(buf == nil)
		sysfatal("outproc: malloc: %r");

	for(;;) {
		fd = aiopen("output", OREAD);
		if(fd < 0) {
			sleep(2000);
			continue;
		}

		for(;;) {
			n = read(fd, buf, TEXTBUF - 1);
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

/* ── Prompt text extraction ─────────────────────────────────────────── */

static char *
prompttext(Win *w)
{
	int   afd, dfd;
	char  buf[8192];
	int   n;
	char *s, *e2;
	char *body;
	int   bodylen;
	char *result;

	/* try dot selection first */
	afd = acmeopen(w->id, "addr", ORDWR);
	dfd = acmeopen(w->id, "data", ORDWR);
	if(afd >= 0 && dfd >= 0) {
		write(afd, ".", 1);
		n = read(dfd, buf, sizeof buf - 1);
		if(n > 0) {
			buf[n] = '\0';
			s  = buf;
			e2 = buf + n - 1;
			while(*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
			while(e2 > s && (*e2 == ' ' || *e2 == '\t' || *e2 == '\n' || *e2 == '\r')) e2--;
			*(e2+1) = '\0';
			if(*s != '\0') {
				close(afd);
				close(dfd);
				return strdup(s);
			}
		}
	}
	if(afd >= 0) close(afd);
	if(dfd >= 0) close(dfd);

	/* fall back to paragraph scan of body */
	body = winbody(w, &bodylen);
	if(body == nil)
		return nil;
	result = prompttext_body(body);
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

static void
cmd_sessions(void)
{
	char  *sessdir;
	int    dirfd;
	Dir   *dirs;
	long   ndirs, i, j;
	Dir    tmp;
	Win   *w;
	char  *cwd;
	char   name[256];
	char   path[512];
	char   uuid[37], model[64], ts[32], snippet[SESS_SNIPPET + 4];
	char   line[256];

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
		tmp = dirs[i];
		j   = i - 1;
		while(j >= 0 && dirs[j].mtime < tmp.mtime) {
			dirs[j+1] = dirs[j];
			j--;
		}
		dirs[j+1] = tmp;
	}

	for(i = 0; i < ndirs; i++) {
		if(dirs[i].name[0] == '.')
			continue;
		snprint(path, sizeof path, "%s/%s", sessdir, dirs[i].name);
		if(parsesessfile(path, uuid, model, ts, snippet) < 0)
			continue;
		if(uuid[0] == '\0')
			continue;

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
	char       buf[ACMEEVENT_BUFSZ];
	int        bufp = 0, nbuf = 0;
	AcmeEvent  e;
	char      *text;
	char       newmodel[128];
	char       msg[128];

	fd = acmeopen(w->id, "event", ORDWR);
	if(fd < 0) {
		free(w);
		return;
	}

	for(;;) {
		if(!getevent(fd, buf, &bufp, &nbuf, &e))
			break;

		if(e.c1 == 'M' && (e.c2 == 'x' || e.c2 == 'X')) {
			text = e.text;
			while(*text == ' ') text++;

			if(ismodelid(text) && e.c2 != 'X') {
				snprint(newmodel, sizeof newmodel, "%s\n", text);
				if(aiwrite("model", newmodel, -1) >= 0) {
					qlock(&statelk);
					scopy(curmodel, sizeof curmodel, text);
					qunlock(&statelk);
					setstatus("ready");
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
	char       buf[ACMEEVENT_BUFSZ];
	int        bufp = 0, nbuf = 0;
	AcmeEvent  e;
	char      *text;

	fd = acmeopen(w->id, "event", ORDWR);
	if(fd < 0) {
		free(w);
		return;
	}

	for(;;) {
		if(!getevent(fd, buf, &bufp, &nbuf, &e))
			break;

		if(e.c1 == 'M' && (e.c2 == 'x' || e.c2 == 'X')) {
			text = e.text;
			while(*text == ' ') text++;

			if(isuuid(text) && e.c2 != 'X') {
				char *path = smprint("%s/%s", AI9, text);
				char *sessdir = configpath("sessions/");
				int   lfd;
				char  msg[128];
				free(path);
				path = smprint("%s%s", sessdir, text);
				free(sessdir);

				lfd = aiopen("session/load", OWRITE);
				if(lfd >= 0) {
					write(lfd, path, strlen(path));
					close(lfd);
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
	char       buf[ACMEEVENT_BUFSZ];
	int        bufp = 0, nbuf = 0;
	AcmeEvent  e;
	char      *text;

	fd = acmeopen(w->id, "event", ORDWR);
	if(fd < 0)
		sysfatal("open event: %r");

	for(;;) {
		if(!getevent(fd, buf, &bufp, &nbuf, &e))
			break;

		if(e.c1 == 'M' && (e.c2 == 'x' || e.c2 == 'X')) {
			text = e.text;
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
