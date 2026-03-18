/*
 * agent.h — agent loop, conversation history, and session I/O for 9ai
 *
 * Implements:
 *   - Conversation history (Msg/Block linked list)
 *   - emit_event() / write_session() — RS/US record format output
 *   - agentrun() — full OAI tool loop: prompt → stream → exec → continue
 *   - Session file creation and append
 *   - UUID generation
 *
 * ── Record format ────────────────────────────────────────────────────
 *
 * Every record is:  field₀ FS field₁ … FS fieldₙ RS
 *
 *   FS = 0x1F  (INFORMATION SEPARATOR ONE — field separator)
 *   RS = 0x1E  (INFORMATION SEPARATOR TWO — record separator)
 *
 * Fields use ESC-escaping for special bytes:
 *   ESC ESC → literal 0x1B
 *   ESC FS  → literal 0x1F within a field value
 *   ESC RS  → literal 0x1E within a field value
 * A bare FS always means field boundary; a bare RS always means record end.
 * This allows empty fields (two consecutive bare FS bytes) and zero-length
 * records (bare RS with no preceding content).
 *
 * ── Conversation history ─────────────────────────────────────────────
 *
 * Stored as a linked list of Msg structs, each containing a linked list
 * of Block content blocks.  The OAI serialiser in oai.c consumes this
 * directly via the OAIReq/OAIMsg/OAIBlock types.
 *
 * ── Agent output ─────────────────────────────────────────────────────
 *
 * agentrun() delivers output through two callbacks:
 *
 *   ontext(text, aux)   — called for each streaming text delta
 *   onevent(rec, len, aux) — called for each complete RS-terminated record
 *
 * Passing nil for a callback silences that output path.  In the test
 * binary these write to stdout/a log buffer; in the 9P server they
 * forward to /output and /event channels.
 *
 * ── Session file ─────────────────────────────────────────────────────
 *
 * agentrun() appends records to the session Biobuf if sess_bio != nil.
 * The session header (session FS uuid FS model FS timestamp RS) is
 * written by agentsessopen() when the file is first created.
 *
 * ── Wire format selection ─────────────────────────────────────────────
 *
 * agentrun()    — OpenAI Completions wire format (gpt-4o, gpt-4.1 …)
 * agentrunant() — Anthropic Messages wire format (claude-* models)
 *
 * The caller selects the correct function based on the model's fmt field
 * (Fmt_Oai / Fmt_Ant from models.h).  agentrunant() takes an ANTReq
 * instead of an OAIReq for conversation history.
 */

/* ── Record separator constants ─────────────────────────────────────── */
/*
 * RS, FS, ESC, AIRS, AIFS, AESC are defined in record.h, which is
 * included by agent.c and any file that needs the record codec.
 */

/* ── Agent configuration ─────────────────────────────────────────────── */

typedef struct AgentCfg AgentCfg;

struct AgentCfg {
	char *model;       /* model id, e.g. "gpt-4o" */
	char *tokpath;     /* path to GitHub refresh token file */
	char *system;      /* system prompt (may be nil) */
	char *sessdir;     /* session directory override (nil → ~/lib/9ai/sessions/) */

	/*
	 * exec_maxout — output cap for tool execution, in bytes.
	 * The tail of output is kept on overflow (ring buffer).
	 * 0 → use default (512 KB).  Set from the model's ctx_k so
	 * larger context windows allow larger tool outputs.
	 */
	long exec_maxout;

	/*
	 * exec_unmount_mtpt — if non-nil, unmount this path from the exec
	 * child's namespace before running the tool.  Use this to prevent
	 * the LLM-driven exec tool from accessing 9ai's own file system
	 * (e.g. writing to /message to inject prompts or reading /ctl).
	 * nil → no unmount (default; exec child inherits the full namespace).
	 */
	char *exec_unmount_mtpt;

	/* output callbacks — both may be nil */
	void (*ontext)(const char *text, void *aux);
	void (*onevent)(const char *rec, long len, void *aux);
	void *aux;

	/* session I/O — may be nil to skip session recording */
	Biobuf *sess_bio;
	char    uuid[37];  /* current session UUID (set by agentsessopen) */
};

/* ── Session management ──────────────────────────────────────────────── */

/*
 * isctxoverflow — return 1 if an HTTP error body indicates the request
 * was rejected because the context window was exceeded.
 * Matches GitHub Copilot OAI/ANT, generic OpenAI, and Anthropic formats.
 */
int isctxoverflow(const char *body);

/*
 * genuuid — generate a random UUID v4 string into buf (must be ≥37 bytes).
 */
void genuuid(char *buf);

/*
 * agentsessopen — open (create) a new session file.
 *
 * Creates ~/lib/9ai/sessions/ if needed.  Generates a new UUID, opens
 * the file, writes the session header record, and populates cfg->uuid
 * and cfg->sess_bio.
 *
 * Returns 0 on success, -1 on error (sets errstr).
 * Caller must eventually call agentsessclose().
 */
int agentsessopen(AgentCfg *cfg);

/*
 * agentsessload — load an existing session file and reconstruct history.
 *
 * path:    path to the session file (e.g. ~/.cache/9ai/sessions/<uuid>)
 * req:     an OAIReq to populate; caller should pass a freshly-allocated
 *          oaireqnew() — existing messages are NOT cleared here, so pass
 *          a fresh req or handle clearing before calling.
 * cfg:     uuid is populated from the session header; sess_bio is opened
 *          for append so subsequent turns write to the same file.
 *
 * Replay logic:
 *   session  → sets cfg->uuid and (optionally) cfg->model
 *   prompt   → oaireqaddmsg(req, oaimsguser(text))
 *   text     → concatenated across records between turn_start/turn_end
 *              → oaireqaddmsg(req, oaimsgassistant(full_text))
 *   tool_start → accumulate name, id, argv fields into JSON
 *   tool_end   → oaireqaddmsg(req, oaimsgtoolcall(...))
 *              + oaireqaddmsg(req, oaimsgtoolresult(...))
 *   model    → updates cfg->model (caller should sync to AiState)
 *   steer    → ignored (not part of API history)
 *   thinking → ignored (not sent to API)
 *
 * On success: cfg->uuid is set, cfg->sess_bio is open for append,
 * req contains the full history.  Returns 0.
 * On error: returns -1 and sets errstr.
 */
int agentsessload(char *path, OAIReq *req, AgentCfg *cfg);

/*
 * agentsessclose — flush and close the session Biobuf.
 */
void agentsessclose(AgentCfg *cfg);

/* ── Record I/O ──────────────────────────────────────────────────────── */

/*
 * emitevent — format a record and deliver it to cfg->onevent.
 *
 * Fields are supplied as a nil-terminated vararg list of char *.
 * Each field is ESC-encoded by fmtrecfields(): 0x1B, 0x1E, and 0x1F
 * within a field value are preceded by an ESC (0x1B) byte.
 *
 * Example:
 *   emitevent(cfg, "text", delta_text, nil);
 *   emitevent(cfg, "tool_start", "exec", tool_id, argv0, nil);
 */
void emitevent(AgentCfg *cfg, ...);

/*
 * writesession — format a record and append it to cfg->sess_bio.
 * Same vararg convention as emitevent().
 */
void writesession(AgentCfg *cfg, ...);

/*
 * emitandsave — call both emitevent() and writesession() with the same args.
 * Convenience for agent events that go to both destinations.
 */
void emitandsave(AgentCfg *cfg, ...);

/* ── Agent loop ──────────────────────────────────────────────────────── */

/*
 * agentrun — run one full agent turn starting from prompt.
 *
 * prompt:  the user message text
 * req:     an OAIReq containing the existing conversation history;
 *          agentrun() appends the user message and assistant response(s)
 *          to req in place.  Pass an empty OAIReq for the first turn.
 * cfg:     agent configuration, callbacks, session Biobuf
 *
 * The loop:
 *   1. Append user message to req; emit "prompt" session record.
 *   2. Obtain/refresh session token.
 *   3. POST to /chat/completions; stream OAIDelta events.
 *   4. Accumulate text/tool deltas; call cfg->ontext for text chunks.
 *   5. On OAIDStop "stop": emit turn_end; flush session; return 0.
 *   6. On OAIDStop "tool_calls":
 *      a. emit tool_start event/session record
 *      b. call execrun() with accumulated tool args JSON
 *      c. emit tool_end event/session record
 *      d. append tool result to req
 *      e. loop back to step 3 (new POST with tool result in history)
 *
 * Returns 0 on success (turn completed with stop or tool_calls→result→stop).
 * Returns -1 on error (network, parse, exec, etc.) — sets errstr.
 *
 * The session token is obtained fresh before the first POST of each
 * agentrun() call and refreshed if the token is within 5 minutes of
 * expiry between tool loops.
 */
int agentrun(char *prompt, OAIReq *req, AgentCfg *cfg);

/*
 * agentrunant — run one full agent turn using the Anthropic Messages wire format.
 *
 * prompt:  the user message text
 * req:     an ANTReq containing the existing conversation history;
 *          agentrunant() appends the user message and assistant response(s)
 *          to req in place.  Pass an empty ANTReq for the first turn.
 * cfg:     agent configuration, callbacks, session Biobuf
 *
 * Differences from agentrun():
 *   - POSTs to /v1/messages using antreqjson() / antreqhdrs().
 *   - Streams ANTDelta events via antdelta().
 *   - ANTDThinking deltas → emitandsave("thinking", chunk) only;
 *     NOT delivered to cfg->ontext and NOT included in API history.
 *   - stop_reason "end_turn" (text done) vs "tool_use" (tool call).
 *   - Tool result appended as antmsgtoolresult() (user message with
 *     content array containing a tool_result block).
 *
 * Returns 0 on success, -1 on error.
 */
int agentrunant(char *prompt, ANTReq *req, AgentCfg *cfg);
