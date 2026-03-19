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
 *   ├── context          ro    0444
 *   └── session/         dir   0555
 *       ├── id           ro    0444
 *       ├── new          wo    0200
 *       ├── load         wo    0200
 *       └── save         wo    0200
 *
 * ── Concurrency ───────────────────────────────────────────────────────
 *
 * Two dedicated procs beyond the lib9p srv loop:
 *   - agentproc: runs the AI agent (HTTP, tool loop, session I/O).
 *     Must be a separate OS proc with its own note handler for abort.
 *   - authproc:  OAuth device-code flow; sleeps between polls.
 *
 * reqchan (agentproc only): main → agent, carries AgentReq* prompts.
 *
 * Blocking reads (/output, /event, /auth/device, /models) use
 * srvrelease/srvacquire so the srv loop stays live while this proc
 * blocks.  agentproc and authproc wake blocked reads directly via
 * Rendez, writing data into staging fields under g->lk.
 *
 * ── Usage ─────────────────────────────────────────────────────────────
 *
 *   AiState *ai = aiinit(model, tokpath, mtpt);
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
	char *tokpath;    /* GitHub refresh token path */
	char *mtpt;       /* mount point of the 9ai FS, or nil */

	/* agent running state */
	int   busy;       /* 1 while agent turn is in progress */
	int   toolbusy;   /* 1 while exec child is running */
	char  errmsg[256];/* last error, or "" */

	/* session */
	char  uuid[37];   /* current session UUID, or "" */
	Biobuf *sess_bio; /* open session file, or nil */

	/* agentproc → srv: prompt request channel */
	Channel *reqchan;    /* AgentReq* */

	/* agentproc OS pid — written once at agentproc startup; read by fswrite */
	int agentprocpid;

	/* QLock for all state fields below; also used as the lock for all Rendez */
	QLock lk;

	/*
	 * /output — streaming agent text.
	 *
	 * agentproc writes outchunk (malloc'd string, or nil for EOF) under lk,
	 * then rwakeup(&outrend).  fsread blocks in rsleep(&outrend) until
	 * outready is set, then takes outchunk.
	 *
	 * outeof is set when the turn ends; cleared at the start of each turn.
	 * outbusy is set while a read is blocking; prevents concurrent reads.
	 */
	Rendez  outrend;
	char   *outchunk;  /* staged chunk, or nil = EOF sentinel */
	int     outready;  /* 1 when outchunk is valid and waiting */
	int     outeof;    /* 1 after turn-end nil has been consumed */
	int     outbusy;   /* 1 while an fsread is sleeping on outrend */

	/*
	 * /event — agent event records.
	 *
	 * Same pattern as /output.
	 */
	Rendez  eventrend;
	char   *evchunk;
	int     evready;
	int     eveof;
	int     evbusy;

	/* conversation history */
	OAIReq *oaireq;   /* OpenAI Completions history (Fmt_Oai models) */
	ANTReq *antreq;   /* Anthropic Messages history (Fmt_Ant models) */
	int     fmt;      /* Fmt_Oai or Fmt_Ant — derived from model on each switch */
	long    ctx_k;    /* context window in thousands of tokens (0 = unknown) */

	/*
	 * Auth subtree state.
	 *
	 * authstatus: 0 = logged_out, 1 = pending (flow in progress), 2 = logged_in
	 *
	 * loginreqchan: main → authproc: int (1 = start login).
	 *
	 * /auth/device: fsread blocks on devrend until authproc writes devdata
	 * (malloc'd "uri\ncode\n", or nil for EOF) under lk and rwakeup's.
	 */
	int   authstatus;    /* 0=logged_out 1=pending 2=logged_in */
	char  authdev[128];  /* user code, e.g. "ABCD-1234" */
	char  authuri[256];  /* verification URI */
	char  autherr[256];  /* last auth error, or "" */

	Channel *loginreqchan;  /* int (1 = start login) */

	Rendez  devrend;
	char   *devdata;   /* staged "uri\ncode\n" or nil (EOF), set by authproc */
	int     devready;  /* 1 when devdata is valid and waiting */
	int     devbusy;   /* 1 while an fsread is sleeping on devrend */
};

/*
 * aiinit — allocate and initialise AiState with default values.
 *
 * model   — initial model id (e.g. "gpt-4o"); strdup'd
 * tokpath — path to GitHub refresh token file; strdup'd
 * mtpt    — mount point of the 9ai FS (used to unmount from exec children
 *            when -U is passed); nil means no unmount
 *
 * Returns a heap-allocated AiState.  Does not start any procs.
 */
AiState *aiinit(char *model, char *tokpath, char *mtpt);

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
