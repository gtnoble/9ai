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
 *   Send     — send the selection, or last paragraph if nothing is selected
 *   Stop     — abort the current turn (writes "abort" to 9ai/ctl)
 *   Steer    — redirect the running turn with the selection, or last paragraph
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
#include "prompt.h"
#include "record.h"
#include "render.h"
#include "sessfile.h"
#include "acmeevent.h"

/* ── Constants ──────────────────────────────────────────────────────── */

enum {
	STACK      = 65536,
	EVBUF      = 8192,   /* acme event read buffer */
	RECBUF     = 131072, /* 9ai event record buffer */
	TEXTBUF    = 131072, /* /output streaming buffer */
	MAXFIELDS  = 64,
	SNIPPET    = 80,     /* max session snippet length */
};

#define TAG_EXTRA  " | Send Stop Steer New Clear Models Sessions Login"
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
	int   eventfd;  /* plain fd for event file — readable and writable */
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

	{
		char evpath[64];
		snprint(evpath, sizeof evpath, "%d/event", w->id);
		w->eventfd = fsopenfd(acmefs, evpath, ORDWR);
		if(w->eventfd < 0)
			sysfatal("fsopenfd event: %r");
	}
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
	winappend(mainwin, buf, -1);
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

/* ── 9ai /event reader proc ─────────────────────────────────────────── */

static void
aiproc(void *v)
{
	CFsys *fs;
	CFid  *evfd;
	char  *buf;
	char  *rec;
	int    n;

	USED(v);

	buf = malloc(RECBUF);
	rec = malloc(RECBUF);
	if(buf == nil || rec == nil)
		sysfatal("aiproc: malloc: %r");

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
			n = fsread(evfd, buf, RECBUF - 1);
			if(n <= 0)
				break;   /* EOF = turn end */
			buf[n] = '\0';

			/*
			 * buf contains one RS-terminated record.
			 * Parse fields in a copy because splitrec patches NULs.
			 */
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

			} else if(strcmp(type, "auth_ok") == 0) {
				winappendstr(mainwin, "\n✓ Logged in to GitHub Copilot.\n");
				setstatus("ready");

			} else if(strcmp(type, "auth_err") == 0) {
				if(nf >= 2) {
					char errbuf[256];
					snprint(errbuf, sizeof errbuf, "\n⚠ Login failed: %s\n", fields[1]);
					winappendstr(mainwin, errbuf);
				}
				setstatus("login required");
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
	char  *buf;
	int    n;

	USED(v);

	buf = malloc(TEXTBUF);
	if(buf == nil)
		sysfatal("outproc: malloc: %r");

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
			n = fsread(outfd, buf, TEXTBUF - 1);
			if(n < 0)
				break;   /* error — close, reconnect */
			if(n == 0) {
				/* nil EOF = turn done */
				winappendstr(mainwin, SEPARATOR);
				winclean(mainwin);
				break;
			}
			buf[n] = '\0';

			/* stream text directly to the window */
			winappend(mainwin, buf, n);
		}

		fsclose(outfd);
		/* re-open for next turn */
	}
}

/* ── Prompt text extraction ─────────────────────────────────────────── */

/*
 * winrdsel — read the current dot (selection) from the window.
 * Opens <id>/rdsel, which acme snapshots atomically at open-time.
 * Returns a malloc'd, NUL-terminated, whitespace-trimmed string,
 * or nil if the selection is empty or unavailable.
 * Caller must free.
 */
static char *
winrdsel(Win *w)
{
	char  path[64];
	CFid *f;
	char *buf;
	int   n, na, m;
	char *s, *e;

	snprint(path, sizeof path, "%d/rdsel", w->id);
	f = fsopen(acmefs, path, OREAD);
	if(f == nil)
		return nil;

	buf = nil;
	na  = 0;
	n   = 0;
	for(;;) {
		if(na < n + 512) {
			na += 4096;
			buf = realloc(buf, na + 1);
		}
		m = fsread(f, buf + n, na - n);
		if(m <= 0)
			break;
		n += m;
	}
	fsclose(f);

	if(buf == nil || n == 0) {
		free(buf);
		return nil;
	}
	buf[n] = '\0';

	/* trim leading whitespace */
	s = buf;
	while(*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	/* trim trailing whitespace */
	e = buf + n;
	while(e > s && (*(e-1) == ' ' || *(e-1) == '\t' || *(e-1) == '\n' || *(e-1) == '\r'))
		e--;

	if(e <= s) {
		free(buf);
		return nil;
	}

	*e = '\0';
	if(s > buf)
		memmove(buf, s, e - s + 1);
	return buf;
}

/*
 * prompttext — extract the text the user wants to send.
 * First tries the current selection via rdsel; if empty, falls back
 * to reading the whole body and finding the last paragraph.
 * Returns a malloc'd nul-terminated string, or nil if nothing useful.
 */
static char *
prompttext(Win *w)
{
	char *sel;
	char *body;
	int   bodylen;
	char *result;

	sel = winrdsel(w);
	if(sel != nil)
		return sel;

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

	fprint(2, "cmd_send: sending %d bytes: «%s»\n", (int)strlen(text), text);
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

	/* clear the window body */
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
	fswrite(mainwin->addr, "1,$", 3);
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
		char uuid[37], model[64], ts[32], snippet[SESS_SNIPPET + 4];

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
		if(!getevent(w->eventfd, buf, &bufp, &nbuf, &e))
			break;

		if(e.c1 == 'M' && (e.c2 == 'x' || e.c2 == 'X')) {
			/* button-2 execute: treat text as model id */
			char *text = e.text;
			/* skip empty */
			while(*text == ' ') text++;
			if(*text == '\0') {
				if(e.flag & 1)
					writeevent(w->eventfd, &e);
				continue;
			}
			/* looks like a model id if it contains no spaces and no # */
			if(ismodelid(text) && e.c2 != 'X') {  /* Mx = user execute; MX = acme built-in */
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
					writeevent(w->eventfd, &e);
			}
		} else if(e.c2 == 'L' || e.c2 == 'l') {
			if(e.flag & 1)
				writeevent(w->eventfd, &e);
		} else {
			if(e.flag & 1)
				writeevent(w->eventfd, &e);
		}
	}

	close(w->eventfd);
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
		if(!getevent(w->eventfd, buf, &bufp, &nbuf, &e))
			break;

		if(e.c1 == 'M' && (e.c2 == 'x' || e.c2 == 'X')) {
			char *text = e.text;
			while(*text == ' ') text++;

			if(isuuid(text) && e.c2 != 'X') {
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
					writeevent(w->eventfd, &e);
			}
		} else if(e.c2 == 'L' || e.c2 == 'l') {
			if(e.flag & 1)
				writeevent(w->eventfd, &e);
		} else {
			if(e.flag & 1)
				writeevent(w->eventfd, &e);
		}
	}

	close(w->eventfd);
	fsclose(w->ctl);
	free(w);
}

/* ── Login command and proc ─────────────────────────────────────────── */

/*
 * loginproc — opened in a separate thread by cmd_login.
 *
 * Flow:
 *   1. Write to /auth/poll to trigger the device-code flow in authproc.
 *   2. Read /auth/device (blocks until authproc has the device code).
 *   3. Display URL and user code in the main window.
 *   4. Return (authproc is now polling in the background; aiproc will
 *      display the auth_ok or auth_err event when the flow completes).
 */
static void
loginproc(void *v)
{
	CFsys *fs;
	CFid  *pollfd, *devfd;
	char   buf[512];
	int    n;

	USED(v);

	fs = ainewconn();
	if(fs == nil) {
		winappendstr(mainwin, "\n⚠ Cannot connect to 9ai\n");
		return;
	}

	/* trigger the device flow */
	pollfd = fsopen(fs, "auth/poll", OWRITE);
	if(pollfd == nil) {
		winappendstr(mainwin, "\n⚠ Cannot open auth/poll\n");
		fsunmount(fs);
		return;
	}
	fswrite(pollfd, "start", 5);
	fsclose(pollfd);

	/* read the device code (blocks until authproc calls oauthdevicestart) */
	devfd = fsopen(fs, "auth/device", OREAD);
	if(devfd == nil) {
		winappendstr(mainwin, "\n⚠ Cannot open auth/device\n");
		fsunmount(fs);
		return;
	}

	n = fsread(devfd, buf, sizeof buf - 1);
	fsclose(devfd);
	fsunmount(fs);

	if(n <= 0) {
		/* flow already done or error — aiproc will show the result */
		return;
	}
	buf[n] = '\0';

	/*
	 * buf = "https://github.com/login/device\nABCD-1234\n"
	 * Display it clearly in the window.
	 */
	{
		/* parse the two lines */
		char *nl = strchr(buf, '\n');
		char  url[256], code[64];
		if(nl != nil) {
			int ulen = nl - buf;
			if(ulen >= (int)sizeof url) ulen = sizeof url - 1;
			memmove(url, buf, ulen);
			url[ulen] = '\0';
			char *codep = nl + 1;
			char *nl2   = strchr(codep, '\n');
			int   clen  = nl2 ? nl2 - codep : (int)strlen(codep);
			if(clen >= (int)sizeof code) clen = sizeof code - 1;
			memmove(code, codep, clen);
			code[clen] = '\0';

			char msg[512];
			snprint(msg, sizeof msg,
			        "\n── Login to GitHub Copilot ──\n"
			        "Open:  %s\n"
			        "Code:  %s\n"
			        "Waiting for authorization…\n",
			        url, code);
			winappendstr(mainwin, msg);
		} else {
			winappendstr(mainwin, buf);
		}
	}
	setstatus("login pending");
}

static void
cmd_login(void)
{
	/* check current auth status first */
	CFid *statfd;
	char  buf[64];
	int   n;

	statfd = aiopen("auth/status", OREAD);
	if(statfd != nil) {
		n = fsread(statfd, buf, sizeof buf - 1);
		fsclose(statfd);
		if(n > 0) {
			buf[n] = '\0';
			if(strncmp(buf, "pending", 7) == 0) {
				winappendstr(mainwin, "\n[Login already in progress]\n");
				return;
			}
			if(strncmp(buf, "logged_in", 9) == 0) {
				winappendstr(mainwin, "\n[Already logged in. Use -L flag to force re-login.]\n");
				return;
			}
		}
	}

	/* spawn loginproc so we don't block the event loop */
	threadcreate(loginproc, nil, STACK);
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
		if(!getevent(w->eventfd, buf, &bufp, &nbuf, &e))
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
			if(strcmp(text, "Login")    == 0) { cmd_login();    continue; }

			/* unrecognised: pass back if acme can handle it */
			if(e.flag & 1)
				writeevent(w->eventfd, &e);

		} else if(e.c2 == 'L' || e.c2 == 'l') {
			/* look-up: pass back for normal plumbing */
			if(e.flag & 1)
				writeevent(w->eventfd, &e);
		} else {
			/* all other events: pass back if acme can handle */
			if(e.flag & 1)
				writeevent(w->eventfd, &e);
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
	setstatus("ready");

	/* check auth status and show login prompt if needed */
	{
		CFid *statfd = aiopen("auth/status", OREAD);
		if(statfd != nil) {
			char buf[64];
			int  n = fsread(statfd, buf, sizeof buf - 1);
			fsclose(statfd);
			if(n > 0) {
				buf[n] = '\0';
				if(strncmp(buf, "logged_out", 10) == 0) {
					winappendstr(mainwin,
					    "⚠ Not logged in to GitHub Copilot.\n"
					    "  Middle-click Login in the tag, or run: 9ai -L\n");
					setstatus("login required");
				} else if(strncmp(buf, "pending", 7) == 0) {
					winappendstr(mainwin,
					    "Login in progress — check the terminal for the device code.\n");
					setstatus("login pending");
				}
			}
		}
	}

	winclean(mainwin);

	/* start reader procs */
	threadcreate(aiproc,  nil, STACK);
	threadcreate(outproc, nil, STACK);

	/* run event loop in this thread */
	eventproc(mainwin);
}
