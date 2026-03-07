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

#include "9ai.h"
#include "http.h"
#include "json.h"
#include "oauth.h"
#include "models.h"
#include "sse.h"
#include "oai.h"
#include "exec.h"
#include "agent.h"
#include "fs.h"

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
	{nil, 0, 0},
};

static FileEntry sessiontab[] = {
	{"id",   Qsession_id,   0444},
	{"new",  Qsession_new,  0200},
	{"load", Qsession_load, 0200},
	{"save", Qsession_save, 0200},
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
		char *tmp = p9realloc(w->buf, newcap);
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
	p9free(w->buf);
	p9free(w);
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
	d->name  = p9strdup(fe->name);
	d->uid   = p9strdup("9ai");
	d->gid   = p9strdup("9ai");
	d->muid  = p9strdup("9ai");
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
	   path == Qsession_new || path == Qsession_load || path == Qsession_save) {
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
		if(p9pipe(pfd) < 0) {
			modelsfree(mlist);
			respond(r, "pipe: %r");
			return;
		}
		Binit(&bio, pfd[1], OWRITE);
		modelsfmt(mlist, &bio);
		modelsfree(mlist);
		Bflush(&bio);
		Bterm(&bio);
		p9close(pfd[1]);

		{
			char *mbuf;
			long  mcap = 4096, mlen = 0;
			int   nr;

			mbuf = p9malloc(mcap);
			if(mbuf == nil) {
				p9close(pfd[0]);
				respond(r, "malloc");
				return;
			}
			while((nr = read(pfd[0], mbuf + mlen, mcap - mlen - 1)) > 0) {
				mlen += nr;
				if(mlen + 1 >= mcap) {
					mcap *= 2;
					char *tmp = p9realloc(mbuf, mcap);
					if(tmp == nil) {
						p9free(mbuf);
						p9close(pfd[0]);
						respond(r, "realloc");
						return;
					}
					mbuf = tmp;
				}
			}
			p9close(pfd[0]);
			mbuf[mlen] = '\0';
			readbuf(r, mbuf, mlen);
			p9free(mbuf);
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
				p9free(chunk);
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
				p9free(rec);
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
		p9free(g->model);
		g->model = p9strdup(newmodel);
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
			g->uuid[0] = '\0';
			if(g->sess_bio != nil) {
				int fd2 = g->sess_bio->fid;
				Bflush(g->sess_bio);
				Bterm(g->sess_bio);
				close(fd2);
				p9free(g->sess_bio);
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
			p9free(g->model);
			g->model = p9strdup(nm);
			/* update OAIReq model too */
			if(g->oaireq != nil) {
				p9free(g->oaireq->model);
				g->oaireq->model = p9strdup(nm);
			}
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
				req->text = p9strdup(w->buf);
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
				p9free(g->sess_bio);
				g->sess_bio = nil;
			}
			g->uuid[0] = '\0';
			if(agentsessopen(&tmpcfg) == 0) {
				memmove(g->uuid, tmpcfg.uuid, 37);
				g->sess_bio = tmpcfg.sess_bio;
				/* emit session_new event */
				char evbuf[64];
				snprint(evbuf, sizeof evbuf, "session_new\x1f%s\x1e", g->uuid);
				char *evcopy = p9strdup(evbuf);
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
						p9free(g->sess_bio);
						g->sess_bio = nil;
					}
					g->uuid[0] = '\0';

					/* build a fresh OAIReq to fill */
					OAIReq *newreq = oaireqnew(g->model);

					/* AgentCfg for the load call */
					AgentCfg tmpcfg;
					memset(&tmpcfg, 0, sizeof tmpcfg);
					tmpcfg.model   = p9strdup(g->model);
					tmpcfg.sessdir = nil;
					tmpcfg.sess_bio = nil;

					qunlock(&g->lk);

					if(agentsessload(sesspath, newreq, &tmpcfg) == 0) {
						qlock(&g->lk);
						/* replace model if session had one */
						if(tmpcfg.model != nil && tmpcfg.model[0] != '\0') {
							p9free(g->model);
							g->model = p9strdup(tmpcfg.model);
							if(g->oaireq != nil) {
								p9free(g->oaireq->model);
								g->oaireq->model = p9strdup(g->model);
							}
						}
						/* replace history */
						oaireqfree(g->oaireq);
						newreq->model = p9strdup(g->model);
						g->oaireq = newreq;
						memmove(g->uuid, tmpcfg.uuid, 37);
						g->sess_bio = tmpcfg.sess_bio;
						/* emit session_new event with loaded uuid */
						char evbuf2[64];
						snprint(evbuf2, sizeof evbuf2, "session_new\x1f%s\x1e", g->uuid);
						char *evcopy2 = p9strdup(evbuf2);
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
					p9free(tmpcfg.model);
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
				char *tp = p9strdup(g->tokpath);
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
					p9free(srcpath);
				}
				p9free(tp);
			}
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
		d.name  = p9strdup("/");
		d.uid   = p9strdup("9ai");
		d.gid   = p9strdup("9ai");
		d.muid  = p9strdup("9ai");
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
	if(b == nil) { p9free(chunk); return; }
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
	p9free(b);
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
				p9free(chunk);
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
	if(b == nil) { p9free(rec); return; }
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
	p9free(b);
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
				p9free(rec);
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
	chansendp(g->outchan, p9strdup((char*)text));
}

/*
 * agent_onevent — callback from agentrun(); sends RS record to eventchan.
 */
static void
agent_onevent(const char *rec, long len, void *aux)
{
	char *copy;
	USED(aux); USED(len);
	copy = p9strdup((char*)rec);
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
			p9free(req->text);
			p9free(req);
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
				char *tp = p9strdup(g->tokpath);
				char *slash = strrchr(tp, '/');
				if(slash != nil) *slash = '\0';
				sessdir = smprint("%s/sessions/", tp);
				p9free(tp);
			}
			memset(&tmpcfg, 0, sizeof tmpcfg);
			tmpcfg.model   = g->model;
			tmpcfg.sessdir = sessdir;
			tmpcfg.sess_bio = nil;
			if(agentsessopen(&tmpcfg) == 0) {
				memmove(g->uuid, tmpcfg.uuid, 37);
				g->sess_bio = tmpcfg.sess_bio;
			}
			p9free(sessdir);
		}
		qunlock(&g->lk);

		/* build AgentCfg pointing at global state */
		memset(&cfg, 0, sizeof cfg);
		qlock(&g->lk);
		cfg.model    = p9strdup(g->model);
		cfg.sockpath = p9strdup(g->sockpath);
		cfg.tokpath  = p9strdup(g->tokpath);
		cfg.system   = p9strdup("You are a helpful coding assistant. "
		                        "Use the exec tool for file operations and compilation.");
		memmove(cfg.uuid, g->uuid, 37);
		cfg.sess_bio = g->sess_bio;
		cfg.ontext   = agent_ontext;
		cfg.onevent  = agent_onevent;
		cfg.aux      = nil;
		qunlock(&g->lk);

		int rc = agentrun(req->text, g->oaireq, &cfg);

		/* send [done] sentinel before EOF on /output */
		chansendp(g->outchan, p9strdup("[done]\n"));

		/* sync uuid back (agentsessopen may have set it) */
		qlock(&g->lk);
		memmove(g->uuid, cfg.uuid, 37);
		if(rc < 0) {
			rerrstr(g->errmsg, sizeof g->errmsg);
		}
		g->busy = 0;
		g->toolbusy = 0;
		qunlock(&g->lk);

		p9free(cfg.model);
		p9free(cfg.sockpath);
		p9free(cfg.tokpath);
		p9free(cfg.system);
		/* do not free cfg.sess_bio — still owned by g */

		p9free(req->text);
		p9free(req);

		/* signal end-of-turn to watchers */
		chansendp(g->outchan,  nil);
		chansendp(g->eventchan, nil);
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

	ai->model    = p9strdup(model);
	ai->sockpath = p9strdup(sockpath);
	ai->tokpath  = p9strdup(tokpath);
	ai->oaireq   = oaireqnew(model);

	ai->reqchan   = chancreate(sizeof(void*), 8);  /* buffered: fsdestroyfid must not block srv */
	ai->outchan   = chancreate(sizeof(void*), 16);
	ai->eventchan = chancreate(sizeof(void*), 16);
	ai->abortchan = chancreate(sizeof(void*), 4);

	return ai;
}

void
aimain(AiState *ai, char *srvname, char *mtpt)
{
	g = ai;

	/* start agent proc */
	proccreate(agentproc, nil, 65536);

	/* start channel watcher procs */
	proccreate(outwatcher,   nil, 16384);
	proccreate(eventwatcher, nil, 16384);

	/* post 9P service; proccreate's the srv loop and returns */
	fs.aux = ai;
	threadpostmountsrv(&fs, srvname, mtpt, MREPL);
}
