/*
 * ant.h — Anthropic Messages request builder and SSE delta parser for 9ai
 *
 * Implements the Copilot /v1/messages wire format (Anthropic Messages).
 * Used for Claude models: claude-sonnet-4.5, claude-opus-4.5, etc.
 *
 * ── Conversation history ───────────────────────────────────────────────
 *
 * Messages are stored as a linked list of ANTMsg structs.  Each message
 * has a role (user, assistant) and a linked list of ANTBlock content
 * blocks.
 *
 * Block types:
 *   ANTText       — text content block
 *   ANTThinking   — thinking block (assistant only; not sent back to API)
 *   ANTToolUse    — tool use block (assistant only)
 *   ANTToolResult — tool result block (user message only)
 *
 * The exec tool is hardcoded as the single tool declaration.
 *
 * ── Request building ───────────────────────────────────────────────────
 *
 *   ANTReq *req = antreqnew("claude-sonnet-4.5");
 *   antreqaddmsg(req, antmsguser("hello world"));
 *   long bodylen;
 *   char *body = antreqjson(req, "system prompt", &bodylen);
 *   // POST body to /v1/messages
 *   free(body);
 *   antreqfree(req);
 *
 * ── SSE delta parsing ──────────────────────────────────────────────────
 *
 * The ANT delta parser sits on top of the SSE parser.  Each call to
 * antdelta() returns one logical delta event.  The caller drives the
 * loop until ANT_DONE or ANT_EOF.
 *
 *   ANTDelta d;
 *   ANTParser p;
 *   antinit(&p, resp);
 *   int rc;
 *   char text[65536];
 *   text[0] = '\0';
 *   while((rc = antdelta(&p, &d)) == ANT_OK) {
 *       switch(d.type) {
 *       case ANTDText:     strncat(text, d.text, sizeof text - 1); break;
 *       case ANTDThinking: // d.text is a thinking chunk            break;
 *       case ANTDTool:     // d.tool_id, d.tool_name                break;
 *       case ANTDToolArg:  // partial JSON input: d.text            break;
 *       case ANTDStop:     // stop_reason in d.stop_reason          break;
 *       }
 *   }
 *   // rc == ANT_DONE or ANT_EOF
 *
 * ANTDelta fields are valid only until the next call to antdelta().
 * Copy any strings you need to retain.
 *
 * ── GitHub Copilot ANT compat notes ───────────────────────────────────
 *
 *   - No anthropic-version or anthropic-beta headers (proxy adds them).
 *   - "system" is a top-level string field (not a messages[] entry).
 *   - Tool declaration uses "input_schema" not "parameters".
 *   - Max tokens field: "max_tokens" (not "max_completion_tokens").
 *   - X-Initiator: "user" if last message is user; "agent" otherwise.
 *   - Tool results go in a user message with role "user" and content
 *     array containing a tool_result block.
 *   - Assistant message with tool_use: content is an array of blocks,
 *     which may include text + tool_use in sequence.
 *   - Thinking blocks (type "thinking") are present in SSE but must NOT
 *     be included in the messages[] sent back to the API.
 *   - stop_reason values: "end_turn" (text done), "tool_use" (tool call).
 */

/* ── Message and block types ──────────────────────────────────────────── */

enum {
	ANTRoleUser      = 0,
	ANTRoleAssistant = 1,
};

enum {
	ANTBlockText       = 0,  /* text content block          */
	ANTBlockThinking   = 1,  /* thinking block (display only) */
	ANTBlockToolUse    = 2,  /* tool_use block (assistant)  */
	ANTBlockToolResult = 3,  /* tool_result block (user)    */
};

typedef struct ANTBlock ANTBlock;
typedef struct ANTMsg   ANTMsg;
typedef struct ANTReq   ANTReq;

struct ANTBlock {
	int    type;       /* ANTBlock* constant */
	char  *text;       /* ANTBlockText: content; ANTBlockThinking: thinking text;
	                      ANTBlockToolResult: output */
	char  *tool_id;    /* ANTBlockToolUse, ANTBlockToolResult */
	char  *tool_name;  /* ANTBlockToolUse: function name */
	char  *tool_input; /* ANTBlockToolUse: JSON input string */
	int    is_error;   /* ANTBlockToolResult: 1 if exec failed */
	ANTBlock *next;
};

struct ANTMsg {
	int       role;     /* ANTRole* constant */
	ANTBlock *content;  /* linked list; nil for empty */
	ANTMsg   *next;
};

struct ANTReq {
	char   *model;    /* model id, strdup'd */
	ANTMsg *msgs;     /* linked list; append-only */
	ANTMsg *msgtail;
};

/* ── Request builders ─────────────────────────────────────────────────── */

ANTReq   *antreqnew(char *model);
void      antreqfree(ANTReq *req);

/* Append a pre-built message to the request. */
void      antreqaddmsg(ANTReq *req, ANTMsg *msg);

/* Build message constructors. */
ANTMsg   *antmsguser(char *text);            /* user text message */
ANTMsg   *antmsgassistant(char *text);       /* text-only assistant response */
ANTMsg   *antmsgtooluse(char *text, char *tool_id, char *tool_name, char *tool_input);
ANTMsg   *antmsgtoolresult(char *tool_id, char *output, int is_error);

/*
 * antreqjson — serialise the full request to a malloc'd JSON string.
 *
 * Produces the request body for POST /v1/messages.  Includes:
 *   - "model", "stream": true, "max_tokens": 32000
 *   - "system": system_prompt (top-level string; omitted if nil/empty)
 *   - all messages in req->msgs
 *   - exec tool declaration (using "input_schema")
 *
 * Returns nil on error.  Caller must free() the result.
 * Sets *lenp to the body length (not counting the NUL terminator).
 */
char     *antreqjson(ANTReq *req, char *system_prompt, long *lenp);

/*
 * antreqhdrs — fill hdrs[0..] with the Copilot headers needed for
 * /v1/messages.  Writes at most maxn entries.
 * Sets X-Initiator based on the last message role.
 * Returns the number of headers written.
 *
 * session: Copilot session token (tid=...) used as Bearer.
 */
int       antreqhdrs(ANTReq *req, char *session, HTTPHdr *hdrs, int maxn);

/* ── SSE delta parser ─────────────────────────────────────────────────── */

enum {
	ANT_OK   = 0,   /* delta filled; call again            */
	ANT_DONE = 1,   /* message_stop/[DONE] received        */
	ANT_EOF  = 2,   /* SSE_EOF before message_stop         */
};

enum {
	ANTDText    = 0,  /* text_delta: d.text                     */
	ANTDThinking = 1, /* thinking_delta: d.text                 */
	ANTDTool    = 2,  /* new tool_use block: d.tool_id, d.tool_name */
	ANTDToolArg = 3,  /* input_json_delta: d.text               */
	ANTDStop    = 4,  /* stop_reason: d.stop_reason             */
};

typedef struct ANTParser ANTParser;
typedef struct ANTDelta  ANTDelta;

struct ANTDelta {
	int   type;        /* ANTD* constant */
	char *text;        /* ANTDText, ANTDThinking, ANTDToolArg: delta text */
	char *tool_id;     /* ANTDTool: new tool_use id          */
	char *tool_name;   /* ANTDTool: function name             */
	char *stop_reason; /* ANTDStop: "end_turn" | "tool_use"   */
};

struct ANTParser {
	SSEParser sse;
	/* internal buffers; fields in ANTDelta point into these */
	char textbuf[65536];
	char tool_idbuf[128];
	char tool_namebuf[128];
	char stop_reasonbuf[32];
	/* block type tracking between content_block_start and content_block_stop */
	int  block_type;   /* ANTBlockText, ANTBlockToolUse, or -1 */
};

void antinit(ANTParser *p, HTTPResp *resp);

/*
 * antdelta — parse the next logical delta event from the SSE stream.
 *
 * Returns ANT_OK with *d filled, ANT_DONE at end of turn, ANT_EOF on error.
 * Skips empty chunks and non-delta events automatically.
 * d is valid only until the next call.
 */
int  antdelta(ANTParser *p, ANTDelta *d);
