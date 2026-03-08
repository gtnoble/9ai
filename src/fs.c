/*
 * fs.c — 9P file server for 9ai
 *
 * See fs.h for the interface and design notes.
 *
 * ── File tree ─────────────────────────────────────────────────────────
 *
 * Files are identified by QID path constants (Qxxx).  The root and
 * session/ directories use synthetic directory reads.
 *
 * ── /message and /steer ───────────────────────────────────────────────
 *
 * Each fid that opens one of these files gets a private WriteBuf in
 * fid->aux.  Writes append to the buf.  On clunk with OWRITE, the
 * accumulated text is sent as an AgentReq on reqchan, and the agent
 * proc takes ownership of the text.
 *
 * ── /output and /event ────────────────────────────────────────────────
 *
 * Each read parks a Req* in ai->pending_output or ai->pending_event.
 * Only one pending read per file is allowed (concurrent reads return error).
 * The agent proc sends chunks on outchan/eventchan; the srv loop picks
 * them up via a channel watcher thread and responds to the parked Req*.
 *
 * Turn end is signalled by the agent sending nil on the channel.  On nil,
 * we respond with 0 bytes (EOF) to the parked Req*, if any.
 *
 * ── /ctl ──────────────────────────────────────────────────────────────
 *
 * Read: "model <id>\nstatus <s>\nsession <uuid>\n"
 * Write: "abort", "clear", "model <id>"
 *
 * ── /status ───────────────────────────────────────────────────────────
 *
 * Non-blocking read: "idle\n", "running\n", "tool\n", "error: <msg>\n"
 *
 * ── /model ────────────────────────────────────────────────────────────
 *
 * Read: current model name + "\n"
 * Write: new model name (switches for next turn)
 *
 * ── /models ───────────────────────────────────────────────────────────
 *
 * Non-blocking read.  Fetches GET /models live via modelsfetch().
 * Returns tab-separated lines.
 *
 * ── /session/new ──────────────────────────────────────────────────────
 *
 * Write anything → close current session, open a fresh one.
 * Emits session_new event on eventchan if agent is idle.
 *
 * ── /session/load, /session/save ──────────────────────────────────────
 *
 * Deferred to phase 12.  Currently accept writes and succeed silently
 * (stubs) so the file tree is complete.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#ifndef PLAN9PORT
#define chansendp(c, v)  sendp(c, v)
#define chanrecvp(c)     recvp(c)
#endif

#include "9ai.h"
#include "http.h"
#include "json.h"
#include "oauth.h"
#include "models.h"
#include "sse.h"
#include "oai.h"
#include "ant.h"
#include "exec.h"
#include "agent.h"
#include "fs.h"

/* ── System prompt ─────────────────────────────────────────────────────── */

/*
 * Default system prompt for 9ai.
 *
 * Derived from the pi coding-agent system prompt, adapted for 9ai's
 * environment: single exec tool (direct execve, no shell), Plan 9 /
 * plan9port conventions, and no TUI or markdown rendering.
 *
 * Structure mirrors pi's buildSystemPrompt():
 *   1. Role / identity
 *   2. Available tool
 *   3. Guidelines (adapted to exec-only environment)
 *   4. Formatting rules
 */
static const char defaultsystem[] =
	"You are an expert coding assistant operating inside 9ai, a Plan 9 "
	"coding agent. You help users by executing programs, reading files, "
	"editing code, and building projects.\n"
	"\n"
	"Available tool:\n"
	"- exec: Execute a program directly (no shell). Supply argv[] and "
	"optional stdin.\n"
	"\n"
	"Use standard Unix programs for all file operations:\n"
	"- Read files: cat, head, tail\n"
	"- Search: grep, find\n"
	"- Edit: ed, patch, awk, sed\n"
	"- Build: cc, mk, make\n"
	"- List: ls\n"
	"\n"
	"Guidelines:\n"
	"- exec runs programs directly — no shell, no pipes, no redirection. "
	"Pass stdin text for input; chain operations by running programs "
	"sequentially and passing prior output as stdin.\n"
	"- Read files before editing. Use cat or head to examine content, "
	"then ed or patch for precise changes.\n"
	"- For surgical edits, use ed(1): write an ed script to stdin "
	"(e.g. \"/pattern/s/old/new/\\nw\\nq\") and pass the file as argv.\n"
	"- Use mk or make to build; read the mkfile or Makefile first.\n"
	"- Be concise in your responses. Output plain text — no markdown "
	"formatting, no code fences. This is a plain-text terminal.\n"
	"- Show file paths clearly when working with files.\n"
	"- When summarizing actions, write plain prose — do not re-run cat "
	"or echo to display what you did.\n";

/* ── QID path constants ────────────────────────────────────────────────── */

enum {
	Qroot        = 0,
	Qctl         = 1,
	Qmessage     = 2,
	Qsteer       = 3,
	Qoutput      = 4,
	Qevent       = 5,
	Qstatus      = 6,
	Qmodel       = 7,
	Qmodels      = 8,
	Qsession     = 9,   /* session/ dir */
	Qsession_id  = 10,
	Qsession_new = 11,
	Qsession_load= 12,
	Qsession_save= 13,
	Qauth        = 14,  /* auth/ dir */
	Qauth_status = 15,
	Qauth_device = 16,
	Qauth_poll   = 17,
	NQID,
};

/* ── File table ────────────────────────────────────────────────────────── */

typedef struct FileEntry FileEntry;
struct FileEntry {
	char *name;
	ulong qpath;
	ulong perm;     /* includes DMDIR for directories */
};

static FileEntry filetab[] = {
	/* root dir entries (index by Qxxx - 1 for root children) */
	{"ctl",         Qctl,          0600},
	{"message",     Qmessage,      0200},
	{"steer",       Qsteer,        0200},
	{"output",      Qoutput,       0400},
	{"event",       Qevent,        0400},
	{"status",      Qstatus,       0444},
	{"model",       Qmodel,        0600},
	{"models",      Qmodels,       0444},
	{"session",     Qsession,      DMDIR|0555},
	{"auth",        Qauth,         DMDIR|0555},
	{nil, 0, 0},
};

static FileEntry sessiontab[] = {
	{"id",   Qsession_id,   0444},
	{"new",  Qsession_new,  0200},
	{"load", Qsession_load, 0200},
	{"save", Qsession_save, 0200},
	{nil, 0, 0},
};

static FileEntry authtab[] = {
	{"status", Qauth_status, 0444},
	{"device", Qauth_device, 0400},
	{"poll",   Qauth_poll,   0200},
	{nil, 0, 0},
};

/* ── Global state pointer ──────────────────────────────────────────────── */

static AiState *g;

/* ── Forward declarations for buffer helpers (defined near watchers) ───── */
static int outbuf_pop(char **chunkp);
static int evbuf_pop(char **recp);

/* ── WriteBuf — accumulates writes to /message, /steer, /session/load etc ─ */

typedef struct WriteBuf WriteBuf;
struct WriteBuf {
	char *buf;
	long  size;
	long  cap;
};

static WriteBuf *
wbufalloc(void)
{
	WriteBuf *w;

	w = mallocz(sizeof *w, 1);
	if(w == nil)
		sysfatal("mallocz WriteBuf: %r");
	w->cap = 4096;
	w->buf = malloc(w->cap);
	if(w->buf == nil)
		sysfatal("malloc WriteBuf.buf: %r");
	w->buf[0] = '\0';
	w->size = 0;
	return w;
}

static void
wbufappend(WriteBuf *w, void *data, long n)
{
	if(w->size + n + 1 > w->cap) {
		long newcap = w->cap * 2 + n + 1;
		char *tmp = realloc(w->buf, newcap);
		if(tmp == nil)
			sysfatal("realloc WriteBuf: %r");
		w->buf = tmp;
		w->cap = newcap;
	}
	memmove(w->buf + w->size, data, n);
	w->size += n;
	w->buf[w->size] = '\0';
}

static void
wbuffree(WriteBuf *w)
{
	if(w == nil)
		return;
	free(w->buf);
	free(w);
}

/* ── QID helpers ───────────────────────────────────────────────────────── */

static Qid
mkqid(ulong path, int isdir)
{
	Qid q;
	q.path = path;
	q.vers = 0;
	q.type = isdir ? QTDIR : QTFILE;
	return q;
}

/* ── Directory generation ──────────────────────────────────────────────── */

/*
 * filldir — fill a Dir from a FileEntry.
 */
static void
filldir(Dir *d, FileEntry *fe)
{
	memset(d, 0, sizeof *d);
	d->name  = strdup(fe->name);
	d->uid   = strdup("9ai");
	d->gid   = strdup("9ai");
	d->muid  = strdup("9ai");
	d->qid   = mkqid(fe->qpath, (fe->perm & DMDIR) != 0);
	d->mode  = fe->perm;
	d->atime = 0;
	d->mtime = 0;
	d->length = 0;
}

/*
 * fsdirgen — dirgen callback for dirread9p.
 * n is the index of the entry to return.
 * Returns 1 on success, -1 at end.
 */
static int
fsdirgen(int n, Dir *d, void *aux)
{
	FileEntry *tab = aux;
	int i;

	for(i = 0; i <= n; i++)
		if(tab[i].name == nil)
			return -1;

	filldir(d, &tab[n]);
	return 1;
}

/* ── 9P operation handlers ─────────────────────────────────────────────── */

static void
fsattach(Req *r)
{
	r->fid->qid = mkqid(Qroot, 1);
	r->ofcall.qid = r->fid->qid;
	respond(r, nil);
}

static char *
fswalk1(Fid *fid, char *name, void *v)
{
	FileEntry *tab;
	int i;
	USED(v);

	if(fid->qid.path == Qroot) {
		tab = filetab;
	} else if(fid->qid.path == Qsession) {
		tab = sessiontab;
	} else if(fid->qid.path == Qauth) {
		tab = authtab;
	} else {
		return "file not found";
	}

	for(i = 0; tab[i].name != nil; i++) {
		if(strcmp(tab[i].name, name) == 0) {
			fid->qid = mkqid(tab[i].qpath, (tab[i].perm & DMDIR) != 0);
			return nil;
		}
	}
	return "file not found";
}

static char *
fsclone(Fid *old, Fid *new, void *v)
{
	USED(old); USED(new); USED(v);
	return nil;
}

static void
fswalk(Req *r)
{
	walkandclone(r, fswalk1, fsclone, nil);
}

static void
fsopen(Req *r)
{
	ulong path;
	int   mode;

	path = r->fid->qid.path;
	mode = r->ifcall.mode & 3;  /* low 2 bits: OREAD, OWRITE, ORDWR */

	/* permission check */
	switch(path) {
	case Qmessage:
	case Qsteer:
	case Qsession_new:
	case Qsession_load:
	case Qsession_save:
	case Qauth_poll:
		if(mode != OWRITE) {
			respond(r, "permission denied");
			return;
		}
		break;
	case Qoutput:
	case Qevent:
	case Qstatus:
	case Qmodels:
	case Qsession_id:
	case Qauth_status:
	case Qauth_device:
		if(mode != OREAD) {
			respond(r, "permission denied");
			return;
		}
		break;
	}

	/* /message: reject if agent is busy */
	if(path == Qmessage) {
		qlock(&g->lk);
		if(g->busy) {
			qunlock(&g->lk);
			respond(r, "agent busy");
			return;
		}
		qunlock(&g->lk);
	}

	/* allocate write buffer for write-only files */
	if(path == Qmessage || path == Qsteer ||
	   path == Qsession_new || path == Qsession_load || path == Qsession_save ||
	   path == Qauth_poll) {
		r->fid->aux = wbufalloc();
	}

	respond(r, nil);
}

static void
fsread(Req *r)
{
	ulong path;
	char  buf[1024];

	path = r->fid->qid.path;

	switch(path) {
	case Qroot:
		dirread9p(r, fsdirgen, filetab);
		respond(r, nil);
		return;

	case Qsession:
		dirread9p(r, fsdirgen, sessiontab);
		respond(r, nil);
		return;

	case Qauth:
		dirread9p(r, fsdirgen, authtab);
		respond(r, nil);
		return;

	case Qstatus:
		qlock(&g->lk);
		if(g->errmsg[0] != '\0')
			snprint(buf, sizeof buf, "error: %s\n", g->errmsg);
		else if(g->toolbusy)
			snprint(buf, sizeof buf, "tool\n");
		else if(g->busy)
			snprint(buf, sizeof buf, "running\n");
		else
			snprint(buf, sizeof buf, "idle\n");
		qunlock(&g->lk);
		readstr(r, buf);
		respond(r, nil);
		return;

	case Qmodel:
		qlock(&g->lk);
		snprint(buf, sizeof buf, "%s\n", g->model);
		qunlock(&g->lk);
		readstr(r, buf);
		respond(r, nil);
		return;

	case Qctl: {
		char uuid[37];
		char model[128];
		char status[64];

		qlock(&g->lk);
		snprint(model, sizeof model, "%s", g->model);
		if(g->errmsg[0] != '\0')
			snprint(status, sizeof status, "error: %s", g->errmsg);
		else if(g->toolbusy)
			snprint(status, sizeof status, "tool");
		else if(g->busy)
			snprint(status, sizeof status, "running");
		else
			snprint(status, sizeof status, "idle");
		snprint(uuid, sizeof uuid, "%s", g->uuid[0] ? g->uuid : "none");
		qunlock(&g->lk);

		snprint(buf, sizeof buf, "model %s\nstatus %s\nsession %s\n",
		        model, status, uuid);
		readstr(r, buf);
		respond(r, nil);
		return;
	}

	case Qmodels: {
		/*
		 * Live fetch from Copilot API.
		 * Load refresh token, get session token, fetch model list.
		 * Format into a heap buffer and return it.
		 */
		int    fd;
		char   tokbuf[512];
		int    n;
		char  *refresh;
		OAuthToken *tok;
		Model *mlist;
		Biobuf bio;
		int   pfd[2];

		/* read refresh token */
		fd = open(g->tokpath, OREAD);
		if(fd < 0) {
			respond(r, "cannot read token");
			return;
		}
		n = read(fd, tokbuf, sizeof tokbuf - 1);
		close(fd);
		if(n <= 0) {
			respond(r, "empty token");
			return;
		}
		tokbuf[n] = '\0';
		while(n > 0 && (tokbuf[n-1]=='\n'||tokbuf[n-1]=='\r'||tokbuf[n-1]==' '))
			tokbuf[--n] = '\0';
		refresh = tokbuf;

		tok = oauthsession(refresh, g->sockpath);
		if(tok == nil) {
			respond(r, "oauthsession failed");
			return;
		}

		mlist = modelsfetch(tok->token, g->sockpath);
		oauthtokenfree(tok);
		if(mlist == nil) {
			respond(r, "modelsfetch failed");
			return;
		}

		/* write formatted model list into a pipe; read back as string */
		if(pipe(pfd) < 0) {
			modelsfree(mlist);
			respond(r, "pipe: %r");
			return;
		}
		Binit(&bio, pfd[1], OWRITE);
		modelsfmt(mlist, &bio);
		modelsfree(mlist);
		Bflush(&bio);
		Bterm(&bio);
		close(pfd[1]);

		{
			char *mbuf;
			long  mcap = 4096, mlen = 0;
			int   nr;

			mbuf = malloc(mcap);
			if(mbuf == nil) {
				close(pfd[0]);
				respond(r, "malloc");
				return;
			}
			while((nr = read(pfd[0], mbuf + mlen, mcap - mlen - 1)) > 0) {
				mlen += nr;
				if(mlen + 1 >= mcap) {
					mcap *= 2;
					char *tmp = realloc(mbuf, mcap);
					if(tmp == nil) {
						free(mbuf);
						close(pfd[0]);
						respond(r, "realloc");
						return;
					}
					mbuf = tmp;
				}
			}
			close(pfd[0]);
			mbuf[mlen] = '\0';
			readbuf(r, mbuf, mlen);
			free(mbuf);
		}
		respond(r, nil);
		return;
	}

	case Qsession_id:
		qlock(&g->lk);
		snprint(buf, sizeof buf, "%s\n", g->uuid[0] ? g->uuid : "");
		qunlock(&g->lk);
		readstr(r, buf);
		respond(r, nil);
		return;

	case Qauth_status: {
		/*
		 * Non-blocking read.
		 * Returns one of:
		 *   "logged_out\n"
		 *   "pending\n"
		 *   "logged_in\n"
		 *   "error: <msg>\n"
		 */
		qlock(&g->lk);
		if(g->autherr[0] != '\0')
			snprint(buf, sizeof buf, "error: %s\n", g->autherr);
		else if(g->authstatus == 2)
			snprint(buf, sizeof buf, "logged_in\n");
		else if(g->authstatus == 1)
			snprint(buf, sizeof buf, "pending\n");
		else
			snprint(buf, sizeof buf, "logged_out\n");
		qunlock(&g->lk);
		readstr(r, buf);
		respond(r, nil);
		return;
	}

	case Qauth_device: {
		/*
		 * Blocking read.
		 *
		 * If a device code is already available (authstatus == 1 and
		 * authuri is set), respond immediately with:
		 *   "<verification_uri>\n<user_code>\n"
		 *
		 * Otherwise park the request; authproc will respond when the
		 * device code arrives (via devchan).
		 *
		 * Returns 0 bytes (EOF) when the flow completes (success or error).
		 */
		qlock(&g->lk);
		if(g->authstatus == 1 && g->authuri[0] != '\0') {
			/* device code already available */
			snprint(buf, sizeof buf, "%s\n%s\n", g->authuri, g->authdev);
			qunlock(&g->lk);
			readstr(r, buf);
			respond(r, nil);
			return;
		}
		if(g->devpending != nil) {
			qunlock(&g->lk);
			respond(r, "concurrent reads on /auth/device not supported");
			return;
		}
		g->devpending = r;
		qunlock(&g->lk);
		/* devwatcher will respond when authproc sends on devchan */
		return;
	}

	case Qoutput: {
		char *chunk;
		/* if a chunk is already buffered, respond immediately */
		qlock(&g->lk);
		if(outbuf_pop(&chunk)) {
			qunlock(&g->lk);
			r->ifcall.offset = 0;
			if(chunk == nil) {
				r->ofcall.count = 0;
			} else {
				readstr(r, chunk);
				free(chunk);
			}
			respond(r, nil);
			return;
		}
		/* no buffered data — park the request */
		if(g->pending_output != nil) {
			qunlock(&g->lk);
			respond(r, "concurrent reads on /output not supported");
			return;
		}
		g->pending_output = r;
		qunlock(&g->lk);
		/* do not respond now; outwatcher will respond when data arrives */
		return;
	}

	case Qevent: {
		char *rec;
		qlock(&g->lk);
		if(evbuf_pop(&rec)) {
			qunlock(&g->lk);
			r->ifcall.offset = 0;
			if(rec == nil) {
				r->ofcall.count = 0;
			} else {
				readbuf(r, rec, strlen(rec));
				free(rec);
			}
			respond(r, nil);
			return;
		}
		if(g->pending_event != nil) {
			qunlock(&g->lk);
			respond(r, "concurrent reads on /event not supported");
			return;
		}
		g->pending_event = r;
		qunlock(&g->lk);
		return;
	}

	default:
		respond(r, "file not found");
		return;
	}
}

static void
fswrite(Req *r)
{
	ulong    path;
	WriteBuf *w;

	path = r->fid->qid.path;

	switch(path) {
	case Qmessage:
	case Qsteer:
	case Qsession_new:
	case Qsession_load:
	case Qsession_save:
	case Qauth_poll:
		w = r->fid->aux;
		if(w == nil) {
			respond(r, "not open for write");
			return;
		}
		wbufappend(w, r->ifcall.data, r->ifcall.count);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;

	case Qmodel: {
		/* strip trailing whitespace; update model */
		char newmodel[128];
		long n;

		n = r->ifcall.count;
		if(n >= (long)sizeof newmodel)
			n = sizeof newmodel - 1;
		memmove(newmodel, r->ifcall.data, n);
		newmodel[n] = '\0';
		while(n > 0 && (newmodel[n-1]=='\n'||newmodel[n-1]=='\r'||newmodel[n-1]==' '))
			newmodel[--n] = '\0';
		if(n == 0) {
			respond(r, "empty model name");
			return;
		}
		qlock(&g->lk);
		free(g->model);
		g->model = strdup(newmodel);
		/* update history model fields and wire format */
		if(g->oaireq != nil) {
			free(g->oaireq->model);
			g->oaireq->model = strdup(newmodel);
		}
		if(g->antreq != nil) {
			free(g->antreq->model);
			g->antreq->model = strdup(newmodel);
		}
		/* infer format: "claude-" prefix → Anthropic Messages */
		g->fmt = (strncmp(newmodel, "claude-", 7) == 0) ? Fmt_Ant : Fmt_Oai;
		qunlock(&g->lk);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}

	case Qctl: {
		char cmd[256];
		long n;

		n = r->ifcall.count;
		if(n >= (long)sizeof cmd)
			n = sizeof cmd - 1;
		memmove(cmd, r->ifcall.data, n);
		cmd[n] = '\0';
		while(n > 0 && (cmd[n-1]=='\n'||cmd[n-1]=='\r'||cmd[n-1]==' '))
			cmd[--n] = '\0';

		if(strcmp(cmd, "abort") == 0) {
			int one = 1;
			chansendp(g->abortchan, (void*)(uintptr)one);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		} else if(strcmp(cmd, "clear") == 0) {
			qlock(&g->lk);
			oaireqfree(g->oaireq);
			g->oaireq = oaireqnew(g->model);
			antreqfree(g->antreq);
			g->antreq = antreqnew(g->model);
			g->uuid[0] = '\0';
			if(g->sess_bio != nil) {
				int fd2 = g->sess_bio->fid;
				Bflush(g->sess_bio);
				Bterm(g->sess_bio);
				close(fd2);
				free(g->sess_bio);
				g->sess_bio = nil;
			}
			qunlock(&g->lk);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		} else if(strncmp(cmd, "model ", 6) == 0) {
			char *nm = cmd + 6;
			if(*nm == '\0') {
				respond(r, "missing model name");
				return;
			}
			qlock(&g->lk);
			free(g->model);
			g->model = strdup(nm);
			/* update OAIReq and ANTReq model too */
			if(g->oaireq != nil) {
				free(g->oaireq->model);
				g->oaireq->model = strdup(nm);
			}
			if(g->antreq != nil) {
				free(g->antreq->model);
				g->antreq->model = strdup(nm);
			}
			/* infer wire format from model id */
			g->fmt = (strncmp(nm, "claude-", 7) == 0) ? Fmt_Ant : Fmt_Oai;
			qunlock(&g->lk);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		} else {
			respond(r, "unknown ctl command");
		}
		return;
	}

	default:
		respond(r, "permission denied");
		return;
	}
}

/*
 * fsdestroyfid — called when a fid is clunked (closed).
 *
 * For /message and /steer: send the accumulated text as an AgentReq.
 * For /session/new: start a new session.
 * For /session/load, /session/save: stubs (phase 12).
 * For write buffers: free them.
 */
static void
fsdestroyfid(Fid *fid)
{
	ulong    path;
	WriteBuf *w;

	path = fid->qid.path;
	w    = fid->aux;

	switch(path) {
	case Qmessage:
	case Qsteer:
		if(w != nil && w->size > 0 && (fid->omode & 3) == OWRITE) {
			AgentReq *req;
			req = mallocz(sizeof *req, 1);
			if(req != nil) {
				req->type = (path == Qmessage) ? AgentReqPrompt : AgentReqSteer;
				req->text = strdup(w->buf);
				chansendp(g->reqchan, req);
			}
		}
		wbuffree(w);
		fid->aux = nil;
		break;

	case Qsession_new:
		if(w != nil) {
			/* start a fresh session */
			AgentCfg tmpcfg;
			memset(&tmpcfg, 0, sizeof tmpcfg);
			qlock(&g->lk);
			tmpcfg.model   = g->model;
			tmpcfg.sessdir = nil;
			tmpcfg.sess_bio = nil;
			/* close old session if open */
			if(g->sess_bio != nil) {
				int fd2 = g->sess_bio->fid;
				Bflush(g->sess_bio);
				Bterm(g->sess_bio);
				close(fd2);
				free(g->sess_bio);
				g->sess_bio = nil;
			}
			g->uuid[0] = '\0';
			if(agentsessopen(&tmpcfg) == 0) {
				memmove(g->uuid, tmpcfg.uuid, 37);
				g->sess_bio = tmpcfg.sess_bio;
				/* emit session_new event */
				char evbuf[64];
				snprint(evbuf, sizeof evbuf, "session_new\x1f%s\x1e", g->uuid);
				char *evcopy = strdup(evbuf);
				chansendp(g->eventchan, evcopy);
			}
			qunlock(&g->lk);
			wbuffree(w);
			fid->aux = nil;
		}
		break;

	case Qsession_load:
		/*
		 * Load a session from the path written by the client.
		 * Reject if the agent is busy.
		 * Clears current OAIReq history and replaces it with the
		 * session loaded from the given path.
		 * Updates g->uuid, g->model (from session header), g->sess_bio.
		 */
		if(w != nil && w->size > 0 && (fid->omode & 3) == OWRITE) {
			char  sesspath[512];
			int   n;

			/* trim whitespace from the path */
			n = w->size;
			while(n > 0 && (w->buf[n-1]=='\n'||w->buf[n-1]=='\r'||w->buf[n-1]==' '))
				w->buf[--n] = '\0';

			if(n == 0 || n >= (int)sizeof sesspath) {
				/* bad path — ignore silently */
			} else {
				memmove(sesspath, w->buf, n+1);

				qlock(&g->lk);

				/* reject if busy */
				if(g->busy) {
					qunlock(&g->lk);
					/* cannot respond with error from destroyfid; just skip */
				} else {
					/* close current session */
					if(g->sess_bio != nil) {
						int fd2 = g->sess_bio->fid;
						Bflush(g->sess_bio);
						Bterm(g->sess_bio);
						close(fd2);
						free(g->sess_bio);
						g->sess_bio = nil;
					}
					g->uuid[0] = '\0';

					/* build a fresh OAIReq to fill */
					OAIReq *newreq = oaireqnew(g->model);

					/* AgentCfg for the load call */
					AgentCfg tmpcfg;
					memset(&tmpcfg, 0, sizeof tmpcfg);
					tmpcfg.model   = strdup(g->model);
					tmpcfg.sessdir = nil;
					tmpcfg.sess_bio = nil;

					qunlock(&g->lk);

					if(agentsessload(sesspath, newreq, &tmpcfg) == 0) {
						qlock(&g->lk);
						/* replace model if session had one */
						if(tmpcfg.model != nil && tmpcfg.model[0] != '\0') {
							free(g->model);
							g->model = strdup(tmpcfg.model);
							if(g->oaireq != nil) {
								free(g->oaireq->model);
								g->oaireq->model = strdup(g->model);
							}
							if(g->antreq != nil) {
								free(g->antreq->model);
								g->antreq->model = strdup(g->model);
							}
							/* infer format from loaded model */
							g->fmt = (strncmp(g->model, "claude-", 7) == 0) ? Fmt_Ant : Fmt_Oai;
						}
						/* replace OAI history (session file uses OAI format for load) */
						oaireqfree(g->oaireq);
						newreq->model = strdup(g->model);
						g->oaireq = newreq;
						/* reset ANT history to empty for this model */
						antreqfree(g->antreq);
						g->antreq = antreqnew(g->model);
						memmove(g->uuid, tmpcfg.uuid, 37);
						g->sess_bio = tmpcfg.sess_bio;
						/* emit session_new event with loaded uuid */
						char evbuf2[64];
						snprint(evbuf2, sizeof evbuf2, "session_new\x1f%s\x1e", g->uuid);
						char *evcopy2 = strdup(evbuf2);
						chansendp(g->eventchan, evcopy2);
						qunlock(&g->lk);
					} else {
						/* load failed — restore a fresh state */
						oaireqfree(newreq);
						qlock(&g->lk);
						g->errmsg[0] = '\0';
						rerrstr(g->errmsg, sizeof g->errmsg);
						qunlock(&g->lk);
					}
					free(tmpcfg.model);
				}
			}
		}
		wbuffree(w);
		fid->aux = nil;
		break;

	case Qsession_save:
		/*
		 * Copy the live session file to the path written by the client.
		 * The live session file is at the path derived from g->uuid
		 * and the sessions directory (tokpath parent/sessions/<uuid>).
		 */
		if(w != nil && w->size > 0 && (fid->omode & 3) == OWRITE) {
			char  dstpath[512];
			int   n;

			n = w->size;
			while(n > 0 && (w->buf[n-1]=='\n'||w->buf[n-1]=='\r'||w->buf[n-1]==' '))
				w->buf[--n] = '\0';

			if(n > 0 && n < (int)sizeof dstpath) {
				memmove(dstpath, w->buf, n+1);

				qlock(&g->lk);
				if(g->sess_bio != nil) Bflush(g->sess_bio);
				char uuid_copy[37];
				memmove(uuid_copy, g->uuid, 37);
				char *tp = strdup(g->tokpath);
				qunlock(&g->lk);

				if(uuid_copy[0] != '\0') {
					char *slash = strrchr(tp, '/');
					if(slash != nil) *slash = '\0';
					char *srcpath = smprint("%s/sessions/%s", tp, uuid_copy);

					/* open src, open dst, copy */
					int   sfd = open(srcpath, OREAD);
					if(sfd >= 0) {
						int dfd = create(dstpath, OWRITE, 0600);
						if(dfd >= 0) {
							char copybuf[4096];
							int  nr;
							while((nr = read(sfd, copybuf, sizeof copybuf)) > 0)
								write(dfd, copybuf, nr);
							close(dfd);
						}
						close(sfd);
					}
					free(srcpath);
				}
				free(tp);
			}
		}
		wbuffree(w);
		fid->aux = nil;
		break;

	case Qauth_poll:
		/*
		 * Writing anything to /auth/poll starts (or restarts) the device
		 * code flow.  We send a signal to authproc via loginreqchan.
		 * authproc handles the actual OAuth work.
		 */
		if(w != nil && (fid->omode & 3) == OWRITE) {
			int one = 1;
			chansendp(g->loginreqchan, (void*)(uintptr)one);
		}
		wbuffree(w);
		fid->aux = nil;
		break;

	default:
		break;
	}
}

static void
fsstat(Req *r)
{
	ulong path;
	FileEntry *tab, *fe;
	int i;

	path = r->fid->qid.path;

	if(path == Qroot) {
		Dir d;
		memset(&d, 0, sizeof d);
		d.name  = strdup("/");
		d.uid   = strdup("9ai");
		d.gid   = strdup("9ai");
		d.muid  = strdup("9ai");
		d.qid   = mkqid(Qroot, 1);
		d.mode  = DMDIR | 0555;
		r->d = d;
		respond(r, nil);
		return;
	}

	/* look up in both tables */
	fe = nil;
	for(tab = filetab; tab->name != nil; tab++) {
		if(tab->qpath == path) { fe = tab; break; }
	}
	if(fe == nil) {
		for(tab = sessiontab; tab->name != nil; tab++) {
			if(tab->qpath == path) { fe = tab; break; }
		}
	}
	if(fe == nil) {
		for(tab = authtab; tab->name != nil; tab++) {
			if(tab->qpath == path) { fe = tab; break; }
		}
	}
	if(fe == nil) {
		respond(r, "file not found");
		return;
	}
	USED(i);
	filldir(&r->d, fe);
	respond(r, nil);
}

/* ── Srv struct ────────────────────────────────────────────────────────── */

static Srv fs = {
	.attach      = fsattach,
	.walk        = fswalk,
	.open        = fsopen,
	.read        = fsread,
	.write       = fswrite,
	.stat        = fsstat,
	.destroyfid  = fsdestroyfid,
};

/* ── Channel watcher procs ─────────────────────────────────────────────── */

/*
 * outwatcher — runs in its own proc; relays chunks from outchan to
 * the parked pending_output Req*.  nil chunk = EOF (turn done).
 *
 * If no reader is parked when a chunk arrives, the chunk is buffered
 * in outbuf_head/tail.  When a reader arrives (fsread on /output),
 * it checks outbuf_head first and responds immediately if non-empty.
 */
typedef struct OutBuf OutBuf;
struct OutBuf {
	char   *chunk;   /* nil = EOF sentinel */
	OutBuf *next;
};

static OutBuf *outbuf_head = nil;
static OutBuf *outbuf_tail = nil;

/* call with g->lk held */
static void
outbuf_push(char *chunk)
{
	OutBuf *b = mallocz(sizeof *b, 1);
	if(b == nil) { free(chunk); return; }
	b->chunk = chunk;
	b->next  = nil;
	if(outbuf_tail != nil) outbuf_tail->next = b;
	else                   outbuf_head = b;
	outbuf_tail = b;
}

/* call with g->lk held; returns 1 if an item was popped, 0 if empty */
static int
outbuf_pop(char **chunkp)
{
	OutBuf *b;
	if(outbuf_head == nil) return 0;
	b = outbuf_head;
	outbuf_head = b->next;
	if(outbuf_head == nil) outbuf_tail = nil;
	*chunkp = b->chunk;
	free(b);
	return 1;
}

static void
outwatcher(void *v)
{
	char *chunk;
	Req  *r;

	USED(v);
	for(;;) {
		chunk = chanrecvp(g->outchan);
		qlock(&g->lk);
		r = g->pending_output;
		if(r != nil) {
			g->pending_output = nil;
			qunlock(&g->lk);
			r->ifcall.offset = 0;
			if(chunk == nil) {
				r->ofcall.count = 0;
				respond(r, nil);
			} else {
				readstr(r, chunk);
				respond(r, nil);
				free(chunk);
			}
		} else {
			/* no reader parked — buffer the chunk for fsread to pick up */
			outbuf_push(chunk);
			qunlock(&g->lk);
		}
	}
}

/*
 * eventwatcher — same pattern as outwatcher for /event records.
 */
typedef struct EvBuf EvBuf;
struct EvBuf {
	char  *rec;
	EvBuf *next;
};

static EvBuf *evbuf_head = nil;
static EvBuf *evbuf_tail = nil;

static void
evbuf_push(char *rec)
{
	EvBuf *b = mallocz(sizeof *b, 1);
	if(b == nil) { free(rec); return; }
	b->rec  = rec;
	b->next = nil;
	if(evbuf_tail != nil) evbuf_tail->next = b;
	else                  evbuf_head = b;
	evbuf_tail = b;
}

static int
evbuf_pop(char **recp)
{
	EvBuf *b;
	if(evbuf_head == nil) return 0;
	b = evbuf_head;
	evbuf_head = b->next;
	if(evbuf_head == nil) evbuf_tail = nil;
	*recp = b->rec;
	free(b);
	return 1;
}

static void
eventwatcher(void *v)
{
	char *rec;
	Req  *r;

	USED(v);
	for(;;) {
		rec = chanrecvp(g->eventchan);
		qlock(&g->lk);
		r = g->pending_event;
		if(r != nil) {
			g->pending_event = nil;
			qunlock(&g->lk);
			r->ifcall.offset = 0;
			if(rec == nil) {
				r->ofcall.count = 0;
				respond(r, nil);
			} else {
				readbuf(r, rec, strlen(rec));
				respond(r, nil);
				free(rec);
			}
		} else {
			evbuf_push(rec);
			qunlock(&g->lk);
		}
	}
}

/* ── Agent proc ────────────────────────────────────────────────────────── */

/*
 * agent_ontext — callback from agentrun(); sends text chunk to outchan.
 * The chunk is duplicated because agentrun's buffer is stack/heap local.
 */
static void
agent_ontext(const char *text, void *aux)
{
	USED(aux);
	chansendp(g->outchan, strdup((char*)text));
}

/*
 * agent_onevent — callback from agentrun(); sends RS record to eventchan.
 */
static void
agent_onevent(const char *rec, long len, void *aux)
{
	char *copy;
	USED(aux); USED(len);
	copy = strdup((char*)rec);
	chansendp(g->eventchan, copy);
}

/*
 * agentproc — main agent loop proc.
 *
 * Waits for AgentReq messages on reqchan.  For each PROMPT, runs
 * agentrun().  On completion, signals EOF on outchan and eventchan
 * (nil pointers) so parked readers get EOF for the turn.
 *
 * STEER: not yet wired (phase 14); accepted but ignored.
 */
static void
agentproc(void *v)
{
	AgentReq *req;
	AgentCfg  cfg;

	USED(v);

	for(;;) {
		req = chanrecvp(g->reqchan);
		if(req == nil)
			continue;

		if(req->type == AgentReqSteer) {
			/* phase 14: not yet implemented */
			free(req->text);
			free(req);
			continue;
		}

		/* set busy */
		qlock(&g->lk);
		g->busy = 1;
		g->errmsg[0] = '\0';
		qunlock(&g->lk);

		/* open session if not already open */
		qlock(&g->lk);
		if(g->sess_bio == nil) {
			AgentCfg tmpcfg;
			char    *sessdir;
			/* derive session dir from tokpath parent, not ~/lib/9ai */
			{
				char *tp = strdup(g->tokpath);
				char *slash = strrchr(tp, '/');
				if(slash != nil) *slash = '\0';
				sessdir = smprint("%s/sessions/", tp);
				free(tp);
			}
			memset(&tmpcfg, 0, sizeof tmpcfg);
			tmpcfg.model   = g->model;
			tmpcfg.sessdir = sessdir;
			tmpcfg.sess_bio = nil;
			if(agentsessopen(&tmpcfg) == 0) {
				memmove(g->uuid, tmpcfg.uuid, 37);
				g->sess_bio = tmpcfg.sess_bio;
			}
			free(sessdir);
		}
		qunlock(&g->lk);

		/* build AgentCfg pointing at global state */
		memset(&cfg, 0, sizeof cfg);
		qlock(&g->lk);
		cfg.model    = strdup(g->model);
		cfg.sockpath = g->sockpath ? strdup(g->sockpath) : nil;
		cfg.tokpath  = strdup(g->tokpath);
		cfg.system   = strdup((char*)defaultsystem);
		memmove(cfg.uuid, g->uuid, 37);
		cfg.sess_bio = g->sess_bio;
		cfg.ontext   = agent_ontext;
		cfg.onevent  = agent_onevent;
		cfg.aux      = nil;
		qunlock(&g->lk);

		int rc;
		qlock(&g->lk);
		int fmt = g->fmt;
		qunlock(&g->lk);

		if(fmt == Fmt_Ant)
			rc = agentrunant(req->text, g->antreq, &cfg);
		else
			rc = agentrun(req->text, g->oaireq, &cfg);

		/* sync uuid back (agentsessopen may have set it) */
		qlock(&g->lk);
		memmove(g->uuid, cfg.uuid, 37);
		if(rc < 0) {
			rerrstr(g->errmsg, sizeof g->errmsg);
		}
		g->busy = 0;
		g->toolbusy = 0;
		qunlock(&g->lk);

		free(cfg.model);
		free(cfg.sockpath);
		free(cfg.tokpath);
		free(cfg.system);
		/* do not free cfg.sess_bio — still owned by g */

		free(req->text);
		free(req);

		/* signal end-of-turn to watchers */
		chansendp(g->outchan,  nil);
		chansendp(g->eventchan, nil);
	}
}

/* ── devwatcher — relays devchan → parked /auth/device Req* ───────────── */

static void
devwatcher(void *v)
{
	char *payload;  /* "uri\ncode\n" or nil (flow done/error) */
	Req  *r;

	USED(v);
	for(;;) {
		payload = chanrecvp(g->devchan);
		qlock(&g->lk);
		r = g->devpending;
		if(r != nil) {
			g->devpending = nil;
			qunlock(&g->lk);
			r->ifcall.offset = 0;
			if(payload == nil) {
				/* flow done or error — EOF */
				r->ofcall.count = 0;
				respond(r, nil);
			} else {
				readstr(r, payload);
				respond(r, nil);
				free(payload);
			}
		} else {
			/* no reader parked: if payload non-nil, drop it (client
			 * will read authuri/authdev fields from /auth/status next open) */
			if(payload != nil)
				free(payload);
			qunlock(&g->lk);
		}
	}
}

/* ── authproc — OAuth device-code flow ────────────────────────────────── */

/*
 * authproc runs in its own proc.  It waits for a signal on loginreqchan,
 * then runs the OAuth device-code flow:
 *
 *   1. oauthdevicestart() → OAuthDeviceCode
 *   2. Update g->authstatus=1, g->authuri, g->authdev under lk
 *   3. Send "uri\ncode\n" on devchan (wakes any parked /auth/device reader)
 *   4. Poll oauthdevicepoll() with sleep(interval)
 *   5. On success: get session token, enable models, set authstatus=2
 *      Emit auth_ok event on eventchan.
 *   6. On error: set authstatus=0, autherr; emit auth_err event.
 *   7. Send nil on devchan (EOF for any still-parked /auth/device reader).
 *
 * A new loginreqchan signal while a flow is in progress is ignored
 * (the current flow continues).
 */
static void
authproc(void *v)
{
	USED(v);

	for(;;) {
		/* wait for a login request */
		chanrecvp(g->loginreqchan);

		/* check if already logged in */
		qlock(&g->lk);
		if(g->authstatus == 2) {
			qunlock(&g->lk);
			/* already logged in — nothing to do */
			continue;
		}
		if(g->authstatus == 1) {
			/* already a flow in progress — ignore */
			qunlock(&g->lk);
			continue;
		}
		g->authstatus  = 1;
		g->authuri[0]  = '\0';
		g->authdev[0]  = '\0';
		g->autherr[0]  = '\0';
		char sockpath[512], tokpath[512];
		snprint(sockpath, sizeof sockpath, "%s", g->sockpath ? g->sockpath : "");
		snprint(tokpath,  sizeof tokpath,  "%s", g->tokpath);
		qunlock(&g->lk);

		/* step 1: start device flow */
		OAuthDeviceCode *dc = oauthdevicestart(sockpath[0] ? sockpath : nil);
		if(dc == nil) {
			char errbuf[256];
			rerrstr(errbuf, sizeof errbuf);
			qlock(&g->lk);
			g->authstatus = 0;
			snprint(g->autherr, sizeof g->autherr, "%s", errbuf);
			qunlock(&g->lk);
			/* emit error event */
			{
				char *ev = smprint("auth_err\x1f%s\x1e", errbuf);
				chansendp(g->eventchan, ev);
			}
			/* EOF for any parked /auth/device reader */
			chansendp(g->devchan, nil);
			continue;
		}

		/* step 2+3: update state and wake /auth/device reader */
		qlock(&g->lk);
		snprint(g->authuri, sizeof g->authuri, "%s", dc->verification_uri);
		snprint(g->authdev, sizeof g->authdev, "%s", dc->user_code);
		qunlock(&g->lk);

		{
			char *payload = smprint("%s\n%s\n", dc->verification_uri, dc->user_code);
			chansendp(g->devchan, payload);
		}

		/* step 4: poll loop */
		int done = 0;
		int rc   = 0;
		while(!done) {
			sleep(dc->interval * 1000);
			rc = oauthdevicepoll(dc, sockpath[0] ? sockpath : nil, tokpath, &done);
		}
		oauthdevcodefree(dc);

		if(rc < 0) {
			/* error */
			char errbuf[256];
			rerrstr(errbuf, sizeof errbuf);
			qlock(&g->lk);
			g->authstatus = 0;
			snprint(g->autherr, sizeof g->autherr, "%s", errbuf);
			qunlock(&g->lk);
			{
				char *ev = smprint("auth_err\x1f%s\x1e", errbuf);
				chansendp(g->eventchan, ev);
			}
			chansendp(g->devchan, nil);
			continue;
		}

		/* step 5: success — get session token, enable models */
		{
			char *refresh;
			int   fd;
			char  buf[512];
			int   n;

			fd = open(tokpath, OREAD);
			if(fd >= 0) {
				n = read(fd, buf, sizeof buf - 1);
				close(fd);
				if(n > 0) {
					buf[n] = '\0';
					while(n > 0 && (buf[n-1]=='\n'||buf[n-1]=='\r'||buf[n-1]==' '))
						buf[--n] = '\0';
					refresh = buf;
					OAuthToken *tok = oauthsession(refresh, sockpath[0] ? sockpath : nil);
					if(tok != nil) {
						oauthenablemodels(tok->token, sockpath[0] ? sockpath : nil);
						oauthtokenfree(tok);
					}
				}
			}
		}

		qlock(&g->lk);
		g->authstatus = 2;
		g->authuri[0] = '\0';
		g->authdev[0] = '\0';
		qunlock(&g->lk);

		/* emit auth_ok event */
		{
			char *ev = smprint("auth_ok\x1e");
			chansendp(g->eventchan, ev);
		}
		/* EOF for any still-parked /auth/device reader */
		chansendp(g->devchan, nil);
	}
}

/* ── aiinit / aimain ───────────────────────────────────────────────────── */

AiState *
aiinit(char *model, char *sockpath, char *tokpath)
{
	AiState *ai;

	ai = mallocz(sizeof *ai, 1);
	if(ai == nil)
		sysfatal("mallocz AiState: %r");

	ai->model    = strdup(model);
	ai->sockpath = sockpath ? strdup(sockpath) : nil;
	ai->tokpath  = strdup(tokpath);
	ai->oaireq   = oaireqnew(model);
	ai->antreq   = antreqnew(model);
	ai->fmt      = Fmt_Oai;  /* default; switched when model changes */

	ai->reqchan      = chancreate(sizeof(void*), 8);  /* buffered: fsdestroyfid must not block srv */
	ai->outchan      = chancreate(sizeof(void*), 16);
	ai->eventchan    = chancreate(sizeof(void*), 16);
	ai->abortchan    = chancreate(sizeof(void*), 4);
	ai->loginreqchan = chancreate(sizeof(void*), 4);
	ai->devchan      = chancreate(sizeof(void*), 4);

	/* initialise auth status based on whether a token already exists */
	ai->authstatus = oauthtokenexists(tokpath) ? 2 : 0;

	return ai;
}

void
aimain(AiState *ai, char *srvname, char *mtpt)
{
	g = ai;

	/* start agent proc */
	proccreate(agentproc, nil, 524288);

	/* start channel watcher procs */
	proccreate(outwatcher,   nil, 16384);
	proccreate(eventwatcher, nil, 16384);
	proccreate(devwatcher,   nil, 16384);

	/* start auth proc */
	proccreate(authproc, nil, 65536);

	/* if no token exists, immediately kick off the login flow */
	if(ai->authstatus == 0)
		chansendp(ai->loginreqchan, (void*)(uintptr)1);

	/* post 9P service; proccreate's the srv loop and returns */
	fs.aux = ai;
	threadpostmountsrv(&fs, srvname, mtpt, MREPL);
}
