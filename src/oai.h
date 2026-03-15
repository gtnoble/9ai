/*
 * oai.h — OpenAI Completions request builder and SSE delta parser for 9ai
 *
 * Implements the Copilot /chat/completions wire format (OAI Completions).
 * Used for free-tier models: gpt-4o, gpt-4.1, etc.
 *
 * ── Conversation history ───────────────────────────────────────────────
 *
 * Messages are stored as a linked list of OAIMsg structs.  Each message
 * has a role (user, assistant, tool) and a linked list of OAIBlock content
 * blocks.
 *
 * Block types:
 *   OAIText       — text content ("content" field in API messages)
 *   OAIToolCall   — tool call in an assistant message
 *   OAIToolResult — tool result in a tool-role message
 *
 * The exec tool is hardcoded as the single tool declaration.
 *
 * ── Request building ───────────────────────────────────────────────────
 *
 *   OAIReq *req = oaireqnew("gpt-4o");
 *   oaireqaddmsg(req, oaimsguser("hello world"));
 *   long bodylen;
 *   char *body = oaireqjson(req, &bodylen);
 *   // POST body to /chat/completions
 *   free(body);
 *   oaireqfree(req);
 *
 * ── SSE delta parsing ──────────────────────────────────────────────────
 *
 * The OAI delta parser sits on top of the SSE parser.  Each call to
 * oaidelta() returns one logical delta event.  The caller drives the
 * loop until OAI_DONE or OAI_EOF.
 *
 *   OAIDelta d;
 *   OAIParser p;
 *   oaiinit(&p, resp);
 *   int rc;
 *   char text[65536];
 *   text[0] = '\0';
 *   while((rc = oaidelta(&p, &d)) == OAI_OK) {
 *       switch(d.type) {
 *       case OAIDText:     strncat(text, d.text, sizeof text - 1); break;
 *       case OAIDTool:     // tool call started: d.tool_id, d.tool_name  break;
 *       case OAIDToolArg:  // partial JSON args: d.text                  break;
 *       case OAIDStop:     // finish_reason in d.stop_reason             break;
 *       }
 *   }
 *   // rc == OAI_DONE or OAI_EOF
 *
 * OAIDelta fields are valid only until the next call to oaidelta().
 * Copy any strings you need to retain.
 *
 * ── GitHub Copilot OAI compat notes ───────────────────────────────────
 *
 *   - "store" field: omit (supportsStore=false)
 *   - System role: "system" not "developer" (supportsDeveloperRole=false)
 *   - Max tokens field: "max_completion_tokens" not "max_tokens"
 *   - reasoning_effort: omit (supportsReasoningEffort=false)
 *   - X-Initiator: "user" if last message is user; "agent" otherwise
 *   - Assistant message content: plain string, never an array
 *     (Copilot rejects array content on assistant messages)
 *   - Tool arguments returned as: {"argv":["ls","-l"],"stdin":""}
 */

/* ── Message and block types ──────────────────────────────────────────── */

enum {
	OAIRoleUser      = 0,
	OAIRoleAssistant = 1,
	OAIRoleTool      = 2,
};

enum {
	OAIBlockText       = 0,  /* text content block */
	OAIBlockToolCall   = 1,  /* assistant tool call */
	OAIBlockToolResult = 2,  /* tool result (role=tool) */
};

typedef struct OAIBlock OAIBlock;
typedef struct OAIMsg   OAIMsg;
typedef struct OAIReq   OAIReq;

struct OAIBlock {
	int    type;       /* OAIBlock* constant */
	char  *text;       /* OAIBlockText: content; OAIBlockToolResult: output */
	char  *tool_id;    /* OAIBlockToolCall, OAIBlockToolResult */
	char  *tool_name;  /* OAIBlockToolCall: function name */
	char  *tool_args;  /* OAIBlockToolCall: JSON arguments string */
	int    is_error;   /* OAIBlockToolResult: 1 if exec failed */
	OAIBlock *next;
};

struct OAIMsg {
	int       role;     /* OAIRole* constant */
	OAIBlock *content;  /* linked list; nil for empty */
	OAIMsg   *next;
};

struct OAIReq {
	char   *model;    /* model id, strdup'd */
	OAIMsg *msgs;     /* linked list; append-only */
	OAIMsg *msgtail;
};

/* ── Request builders ─────────────────────────────────────────────────── */

OAIReq   *oaireqnew(char *model);
void      oaireqfree(OAIReq *req);

/* Append a pre-built message to the request. */
void      oaireqaddmsg(OAIReq *req, OAIMsg *msg);

/* Build message constructors. */
OAIMsg   *oaimsguser(char *text);            /* {"role":"user","content":"..."} */
OAIMsg   *oaimsgassistant(char *text);       /* text-only assistant response */
OAIMsg   *oaimsgtoolcall(char *text, char *tool_id, char *tool_name, char *tool_args);
OAIMsg   *oaimsgtoolresult(char *tool_id, char *output, int is_error);

/*
 * oaireqctxtokens — estimate context token count for the request.
 *
 * Walks every content block in every message and sums strlen(text)/4.
 * Returns a conservative (slight over-) estimate; no tokeniser needed.
 * Thread-safe as long as the caller holds any lock protecting req.
 */
long      oaireqctxtokens(OAIReq *req);

/*
 * oaireqtrim — remove the oldest nturns user+assistant turn pairs.
 *
 * A "turn" is one user message plus all immediately following
 * assistant/tool messages up to (but not including) the next user
 * message.  Returns the number of messages actually removed.
 * Safe to call with nturns == 0 (no-op) or nturns > actual turns
 * (removes all messages).
 */
int       oaireqtrim(OAIReq *req, int nturns);

/*
 * oaireqjson — serialise the full request to a malloc'd JSON string.
 *
 * Produces the request body for POST /chat/completions.  Includes:
 *   - "model", "stream": true, "max_completion_tokens": 16384
 *   - "system" message prepended if system_prompt != nil
 *   - all messages in req->msgs
 *   - exec tool declaration
 *   - no "store" field (Copilot compat)
 *
 * Returns nil on error.  Caller must free() the result.
 * Sets *lenp to the body length (not counting the NUL terminator).
 */
char     *oaireqjson(OAIReq *req, char *system_prompt, long *lenp);

/*
 * oaireqhdrs — fill hdrs[0..] with the Copilot headers needed for
 * /chat/completions.  Writes at most maxn entries.
 * Sets X-Initiator based on the last message role.
 * Returns the number of headers written.
 *
 * session: Copilot session token (tid=...) used as Bearer.
 */
int       oaireqhdrs(OAIReq *req, char *session, HTTPHdr *hdrs, int maxn);

/* ── SSE delta parser ─────────────────────────────────────────────────── */

enum {
	OAI_OK   = 0,   /* delta filled; call again            */
	OAI_DONE = 1,   /* finish_reason received; turn ended  */
	OAI_EOF  = 2,   /* SSE_EOF before finish_reason        */
};

enum {
	OAIDText     = 0,  /* content delta: d.text                  */
	OAIDTool     = 1,  /* new tool call: d.tool_id, d.tool_name  */
	OAIDToolArg  = 2,  /* tool argument chunk: d.text            */
	OAIDStop     = 3,  /* finish_reason: d.stop_reason           */
	OAIDThinking = 4,  /* reasoning delta: d.text                */
};

typedef struct OAIParser OAIParser;
typedef struct OAIDelta  OAIDelta;

struct OAIDelta {
	int   type;        /* OAID* constant */
	char *text;        /* OAIDText, OAIDToolArg, OAIDThinking: delta text */
	char *tool_id;     /* OAIDTool: new tool call id                      */
	char *tool_name;   /* OAIDTool: function name                         */
	char *stop_reason; /* OAIDStop: "stop" | "tool_calls"                 */
};

struct OAIParser {
	SSEParser sse;
	/* heap buffers; fields in OAIDelta point into these */
	char *textbuf;
	long  textbufsz;
	char  tool_idbuf[128];
	char  tool_namebuf[128];
	char  stop_reasonbuf[32];
};

void oaiinit(OAIParser *p, HTTPResp *resp);
void oaiterm(OAIParser *p);

/*
 * oaidelta — parse the next logical delta event from the SSE stream.
 *
 * Returns OAI_OK with *d filled, OAI_DONE at end of turn, OAI_EOF on error.
 * Skips empty/null content chunks automatically.
 * d is valid only until the next call.
 */
int  oaidelta(OAIParser *p, OAIDelta *d);
