/*
 * fs.h — 9P file server interface for 9ai
 *
 * Implements the full 9P file tree described in the design doc:
 *
 *   /                    dir   0555
 *   ├── ctl              rw    0600
 *   ├── message          wo    0200
 *   ├── steer            wo    0200
 *   ├── output           ro    0400
 *   ├── event            ro    0400
 *   ├── status           ro    0444
 *   ├── model            rw    0600
 *   ├── models           ro    0444
 *   └── session/         dir   0555
 *       ├── id           ro    0444
 *       ├── new          wo    0200
 *       ├── load         wo    0200
 *       └── save         wo    0200
 *
 * ── Concurrency ───────────────────────────────────────────────────────
 *
 * Three procs:
 *   - 9P srv loop (lib9p dispatch, main proc)
 *   - Agent proc  (HTTP, tool loop, session I/O)
 *   - Auth proc   (token refresh — deferred to v2; phase 11 reads token inline)
 *
 * Channels:
 *   reqchan   — main → agent: AgentReq (prompt or steer)
 *   outchan   — agent → main: text chunk strings (nil = turn done)
 *   eventchan — agent → main: RS-terminated event records
 *   abortchan — main → agent: abort signal (int)
 *
 * /output and /event reads park a Req* (pending_output, pending_event).
 * When the agent sends on outchan/eventchan, the srv loop responds to
 * the parked Req*.
 *
 * ── Usage ─────────────────────────────────────────────────────────────
 *
 *   AiState *ai = aiinit(cfg);   // allocate and configure global state
 *   aimain(ai, srvname, mtpt);   // post 9P service; never returns
 */

typedef struct AiState  AiState;
typedef struct AgentReq AgentReq;
typedef struct Req      Req;  /* forward decl — defined in <9p.h> */

enum {
	AgentReqPrompt = 0,
	AgentReqSteer  = 1,
};

struct AgentReq {
	int   type;   /* AgentReqPrompt or AgentReqSteer */
	char *text;   /* malloc'd; agent proc must free */
};

/*
 * AiState — global server state.
 * Allocated and initialised by aiinit(); passed to aimain().
 */
struct AiState {
	/* configuration */
	char *model;      /* current model id (heap, always non-nil) */
	char *sockpath;   /* 9aitls proxy socket path */
	char *tokpath;    /* GitHub refresh token path */

	/* agent running state */
	int   busy;       /* 1 while agent turn is in progress */
	int   toolbusy;   /* 1 while exec child is running */
	char  errmsg[256];/* last error, or "" */

	/* session */
	char  uuid[37];   /* current session UUID, or "" */
	Biobuf *sess_bio; /* open session file, or nil */

	/* channels */
	Channel *reqchan;    /* AgentReq* */
	Channel *outchan;    /* char* (text chunks; nil = turn done) */
	Channel *eventchan;  /* char* (RS records; nil = turn done) */
	Channel *abortchan;  /* int */

	/* pending 9P reads */
	Req *pending_output;  /* parked /output read, or nil */
	Req *pending_event;   /* parked /event read, or nil */

	/* QLock for state fields accessed from both srv and agent procs */
	QLock lk;

	/* conversation history */
	OAIReq *oaireq;
};

/*
 * aiinit — allocate and initialise AiState with default values.
 *
 * model    — initial model id (e.g. "gpt-4o"); strdup'd
 * sockpath — path to 9aitls Unix socket; strdup'd
 * tokpath  — path to GitHub refresh token file; strdup'd
 *
 * Returns a heap-allocated AiState.  Does not start any procs.
 */
AiState *aiinit(char *model, char *sockpath, char *tokpath);

/*
 * aimain — post the 9P service and run the srv loop.
 *
 * srvname — 9P service name (e.g. "9ai"); posted to /srv/9ai or $NAMESPACE/9ai
 * mtpt    — optional mount point; nil to skip mount
 *
 * Spawns the agent proc and starts the lib9p dispatch loop.
 * Does not return.
 */
void aimain(AiState *ai, char *srvname, char *mtpt);
