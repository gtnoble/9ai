/*
 * ant.c — Anthropic Messages request builder and SSE delta parser
 *
 * See ant.h for the interface and usage.
 *
 * ── Request serialisation ─────────────────────────────────────────────
 *
 * We write JSON directly into a growing Biobuf backed by a plan9port
 * pipe.  When we are done, we read back the accumulated bytes into a
 * malloc'd buffer and return it.
 *
 * Serialisation rules for GitHub Copilot ANT compat:
 *   - "system" is a top-level string (not in messages[]).
 *   - Tool declaration uses "input_schema" (not "parameters").
 *   - Max tokens: "max_tokens" (not "max_completion_tokens").
 *   - No anthropic-version or anthropic-beta headers.
 *   - Assistant messages can carry mixed content arrays:
 *       text blocks + tool_use blocks in sequence.
 *   - Thinking blocks (ANTBlockThinking) are NOT serialised into the
 *     request — they are display-only.
 *   - Tool results are a user message with a content array containing
 *     a single tool_result block.
 *   - X-Initiator: "user" if last message is user; "agent" otherwise.
 *
 * ── Delta parsing ─────────────────────────────────────────────────────
 *
 * ANT SSE uses event:/data: pairs.  The state machine tracks the current
 * content block type across events:
 *
 *   content_block_start  → set block_type from content_block.type:
 *                           "text"     → ANTBlockText
 *                           "thinking" → ANTBlockThinking
 *                           "tool_use" → ANTBlockToolUse
 *                           emit ANTDTool for tool_use (id + name)
 *   content_block_delta  → dispatch on delta.type:
 *                           "text_delta"       → ANTDText
 *                           "thinking_delta"   → ANTDThinking
 *                           "input_json_delta" → ANTDToolArg
 *   content_block_stop   → reset block_type
 *   message_delta        → extract stop_reason → ANTDStop
 *   message_stop / DONE  → ANT_DONE
 *
 * Empty delta text ("") is skipped; no event emitted.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>

#include "http.h"
#include "json.h"
#include "sse.h"
#include "ant.h"

#define ANT_PATH      "/v1/messages"
#define MAX_OUT_TOK   32000

/* ── Exec tool declaration ─────────────────────────────────────────────
 *
 * Uses "input_schema" (Anthropic Messages format).
 */
static const char exec_tool_json[] =
    "{\"name\":\"exec\","
    "\"description\":\"Execute a program directly (no shell). Use standard "
    "Unix programs for all file operations: cat/head to read, "
    "grep/find to search, ed/patch/awk/sed to edit, cc/mk to compile.\","
    "\"input_schema\":{"
    "\"type\":\"object\","
    "\"properties\":{"
    "\"argv\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
    "\"description\":\"Program and arguments, e.g. [\\\"grep\\\",\\\"-r\\\","
    "\\\"foo\\\",\\\"src\\\"]\"},"
    "\"stdin\":{\"type\":\"string\","
    "\"description\":\"Optional text to supply as stdin.\"}"
    "},"
    "\"required\":[\"argv\"]"
    "}}";

/* ── Block and message constructors ────────────────────────────────────── */

static ANTBlock *
newblock(int type)
{
	ANTBlock *b = mallocz(sizeof *b, 1);
	if(b == nil)
		sysfatal("mallocz: %r");
	b->type = type;
	return b;
}

static ANTMsg *
newmsg(int role)
{
	ANTMsg *m = mallocz(sizeof *m, 1);
	if(m == nil)
		sysfatal("mallocz: %r");
	m->role = role;
	return m;
}

static void
appendblock(ANTMsg *m, ANTBlock *b)
{
	ANTBlock **pp = &m->content;
	while(*pp != nil)
		pp = &(*pp)->next;
	*pp = b;
}

ANTMsg *
antmsguser(char *text)
{
	ANTMsg   *m = newmsg(ANTRoleUser);
	ANTBlock *b = newblock(ANTBlockText);
	b->text = strdup(text);
	appendblock(m, b);
	return m;
}

ANTMsg *
antmsgassistant(char *text)
{
	ANTMsg   *m = newmsg(ANTRoleAssistant);
	ANTBlock *b = newblock(ANTBlockText);
	b->text = strdup(text != nil ? text : "");
	appendblock(m, b);
	return m;
}

ANTMsg *
antmsgtooluse(char *text, char *tool_id, char *tool_name, char *tool_input)
{
	ANTMsg   *m = newmsg(ANTRoleAssistant);
	ANTBlock *b;

	/*
	 * Only include the text block if the text is non-empty.
	 * The Copilot/Claude proxy rejects empty text content blocks
	 * ("text content blocks must be non-empty").
	 */
	if(text != nil && text[0] != '\0') {
		b = newblock(ANTBlockText);
		b->text = strdup(text);
		appendblock(m, b);
	}

	/* tool_use block */
	b = newblock(ANTBlockToolUse);
	b->tool_id    = strdup(tool_id);
	b->tool_name  = strdup(tool_name);
	b->tool_input = strdup(tool_input != nil ? tool_input : "{}");
	appendblock(m, b);
	return m;
}

ANTMsg *
antmsgtoolresult(char *tool_id, char *output, int is_error)
{
	ANTMsg   *m = newmsg(ANTRoleUser);
	ANTBlock *b = newblock(ANTBlockToolResult);
	b->tool_id  = strdup(tool_id);
	b->text     = strdup(output != nil ? output : "");
	b->is_error = is_error;
	appendblock(m, b);
	return m;
}

/* ── ANTReq lifecycle ──────────────────────────────────────────────────── */

ANTReq *
antreqnew(char *model)
{
	ANTReq *req = mallocz(sizeof *req, 1);
	if(req == nil)
		sysfatal("mallocz: %r");
	req->model = strdup(model);
	return req;
}

void
antreqaddmsg(ANTReq *req, ANTMsg *msg)
{
	if(req->msgtail == nil) {
		req->msgs    = msg;
		req->msgtail = msg;
	} else {
		req->msgtail->next = msg;
		req->msgtail       = msg;
	}
}

long
antreqctxtokens(ANTReq *req)
{
	ANTMsg   *m;
	ANTBlock *b;
	long      chars = 0;

	for(m = req->msgs; m != nil; m = m->next)
		for(b = m->content; b != nil; b = b->next) {
			if(b->text       != nil) chars += strlen(b->text);
			if(b->tool_input != nil) chars += strlen(b->tool_input);
		}
	return (chars + 3) / 4;  /* ceil(chars/4) */
}

static void
blockfree(ANTBlock *b)
{
	if(b == nil) return;
	free(b->text);
	free(b->tool_id);
	free(b->tool_name);
	free(b->tool_input);
	free(b);
}

static void
msgfree(ANTMsg *m)
{
	ANTBlock *b, *nb;
	if(m == nil) return;
	for(b = m->content; b != nil; b = nb) {
		nb = b->next;
		blockfree(b);
	}
	free(m);
}

void
antreqfree(ANTReq *req)
{
	ANTMsg *m, *nm;
	if(req == nil) return;
	free(req->model);
	for(m = req->msgs; m != nil; m = nm) {
		nm = m->next;
		msgfree(m);
	}
	free(req);
}

int
antreqtrim(ANTReq *req, int nturns)
{
	ANTMsg   *m, *next;
	ANTBlock *b;
	int       turns, removed, is_toolresult;

	if(nturns <= 0 || req->msgs == nil)
		return 0;

	/*
	 * Walk forward counting turn-starting user messages.
	 *
	 * In the Anthropic wire format, tool results are sent as a user
	 * message containing only ANTBlockToolResult blocks.  These must
	 * NOT be counted as turn boundaries — they are the tail of the
	 * preceding assistant/tool turn, not the start of a new one.
	 *
	 * We count a user message as a turn start only if it contains
	 * at least one non-ANTBlockToolResult block (i.e. it has real
	 * user text).
	 */
	turns = 0;
	m = req->msgs;
	while(m != nil) {
		if(m->role == ANTRoleUser) {
			/* check whether this is a pure tool-result message */
			is_toolresult = 1;
			for(b = m->content; b != nil; b = b->next)
				if(b->type != ANTBlockToolResult) {
					is_toolresult = 0;
					break;
				}
			if(!is_toolresult) {
				if(turns == nturns)
					break;   /* m is the first message to keep */
				turns++;
			}
		}
		m = m->next;
	}

	removed = 0;
	next = req->msgs;
	while(next != m) {
		ANTMsg *cur = next;
		next = cur->next;
		msgfree(cur);
		removed++;
	}
	req->msgs = m;
	if(m == nil)
		req->msgtail = nil;
	return removed;
}

/* ── JSON serialisation ────────────────────────────────────────────────── */

/*
 * readpipe — drain a pipe fd into a malloc'd NUL-terminated buffer.
 * Returns nil on error.  Sets *lenp to bytes read.
 */
static char *
readpipe(int fd, long *lenp)
{
	enum { READCHUNK = 8192 };
	char *buf;
	long  cap, len, n;

	cap = READCHUNK;
	len = 0;
	buf = malloc(cap + 1);
	if(buf == nil)
		return nil;
	for(;;) {
		if(len + READCHUNK > cap) {
			cap *= 2;
			buf = realloc(buf, cap + 1);
			if(buf == nil)
				return nil;
		}
		n = read(fd, buf + len, READCHUNK);
		if(n <= 0) break;
		len += n;
	}
	buf[len] = '\0';
	if(lenp != nil)
		*lenp = len;
	return buf;
}

/*
 * emitmsg — write one ANTMsg to b in Anthropic Messages wire format.
 *
 * user (text only):
 *   {"role":"user","content":<string>}
 *
 * user (tool_result):
 *   {"role":"user","content":[{
 *     "type":"tool_result",
 *     "tool_use_id":<id>,
 *     "content":<string>,
 *     "is_error":<bool>
 *   }]}
 *
 * assistant (text only):
 *   {"role":"assistant","content":[{"type":"text","text":<string>}]}
 *
 * assistant (text + tool_use):
 *   {"role":"assistant","content":[
 *     {"type":"text","text":<string>},
 *     {"type":"tool_use","id":<id>,"name":<name>,"input":<json_obj>}
 *   ]}
 *
 * ANTBlockThinking blocks are skipped (not sent to API).
 */
static void
emitmsg(Biobuf *b, ANTMsg *m)
{
	ANTBlock *blk;
	int       first;

	switch(m->role) {
	case ANTRoleUser: {
		/*
		 * Determine if this is a pure tool_result message or a plain text
		 * message.  A pure tool_result user message uses an array content;
		 * a plain text user message uses a string content.
		 */
		int has_toolresult = 0;
		for(blk = m->content; blk != nil; blk = blk->next)
			if(blk->type == ANTBlockToolResult) { has_toolresult = 1; break; }

		Bprint(b, "{\"role\":\"user\",\"content\":");
		if(has_toolresult) {
			Bprint(b, "[");
			first = 1;
			for(blk = m->content; blk != nil; blk = blk->next) {
				if(blk->type != ANTBlockToolResult) continue;
				if(!first) Bprint(b, ",");
				first = 0;
				Bprint(b, "{\"type\":\"tool_result\",\"tool_use_id\":");
				jsonemitstr(b, blk->tool_id);
				Bprint(b, ",\"content\":");
				jsonemitstr(b, blk->text);
				Bprint(b, ",\"is_error\":%s}", blk->is_error ? "true" : "false");
			}
			Bprint(b, "]");
		} else {
			/* plain text: concatenate all text blocks into a heap buffer */
			long tlen = 0;
			char *tbuf, *p;
			for(blk = m->content; blk != nil; blk = blk->next)
				if(blk->type == ANTBlockText && blk->text != nil)
					tlen += strlen(blk->text);
			tbuf = malloc(tlen + 1);
			if(tbuf == nil) sysfatal("malloc: %r");
			p = tbuf;
			for(blk = m->content; blk != nil; blk = blk->next)
				if(blk->type == ANTBlockText && blk->text != nil) {
					long n = strlen(blk->text);
					memmove(p, blk->text, n);
					p += n;
				}
			*p = '\0';
			jsonemitstr(b, tbuf);
			free(tbuf);
		}
		Bprint(b, "}");
		break;
	}

	case ANTRoleAssistant: {
		/*
		 * Always emit content as an array for assistant messages.
		 * Skip ANTBlockThinking — not sent to API.
		 */
		Bprint(b, "{\"role\":\"assistant\",\"content\":[");
		first = 1;
		for(blk = m->content; blk != nil; blk = blk->next) {
			switch(blk->type) {
			case ANTBlockText:
				/* skip empty text blocks — API rejects them */
				if(blk->text == nil || blk->text[0] == '\0') break;
				if(!first) Bprint(b, ",");
				first = 0;
				Bprint(b, "{\"type\":\"text\",\"text\":");
				jsonemitstr(b, blk->text);
				Bprint(b, "}");
				break;
			case ANTBlockToolUse:
				if(!first) Bprint(b, ",");
				first = 0;
				Bprint(b, "{\"type\":\"tool_use\",\"id\":");
				jsonemitstr(b, blk->tool_id);
				Bprint(b, ",\"name\":");
				jsonemitstr(b, blk->tool_name);
				/*
				 * "input" must be a JSON object, not a string.
				 * blk->tool_input is already a JSON object string (e.g. {"argv":[...]}).
				 * Emit it verbatim.
				 */
				Bprint(b, ",\"input\":%s}",
				       blk->tool_input != nil && blk->tool_input[0] != '\0'
				           ? blk->tool_input : "{}");
				break;
			case ANTBlockThinking:
				/* skip — not sent to API */
				break;
			}
		}
		Bprint(b, "]}");
		break;
	}
	}
}

char *
antreqjson(ANTReq *req, char *system_prompt, long *lenp)
{
	int pfd[2];
	Biobuf *b;
	ANTMsg *m;
	char *buf;
	long len;

	if(pipe(pfd) < 0)
		return nil;

	b = mallocz(sizeof(Biobuf), 1);
	if(b == nil) {
		close(pfd[0]);
		close(pfd[1]);
		return nil;
	}
	Binit(b, pfd[1], OWRITE);

	Bprint(b, "{\"model\":");
	jsonemitstr(b, req->model);
	Bprint(b, ",\"stream\":true");
	Bprint(b, ",\"max_tokens\":%d", MAX_OUT_TOK);

	if(system_prompt != nil && system_prompt[0] != '\0') {
		Bprint(b, ",\"system\":");
		jsonemitstr(b, system_prompt);
	}

	Bprint(b, ",\"messages\":[");
	{
		int first = 1;
		for(m = req->msgs; m != nil; m = m->next) {
			if(!first) Bprint(b, ",");
			first = 0;
			emitmsg(b, m);
		}
	}
	Bprint(b, "]");

	Bprint(b, ",\"tools\":[%s]", exec_tool_json);
	Bprint(b, "}");

	Bflush(b);
	Bterm(b);
	free(b);
	close(pfd[1]);

	buf = readpipe(pfd[0], &len);
	close(pfd[0]);
	if(buf == nil)
		len = 0;
	if(lenp != nil)
		*lenp = len;
	return buf;
}

/*
 * antreqhdrs — fill Copilot request headers for /v1/messages.
 * X-Initiator is "user" if the last message is a user message, "agent" otherwise.
 */
int
antreqhdrs(ANTReq *req, char *session, HTTPHdr *hdrs, int maxn)
{
	const char *initiator;
	int n = 0;

	initiator = "user";
	if(req->msgtail != nil && req->msgtail->role != ANTRoleUser)
		initiator = "agent";

	if(n < maxn) { hdrs[n].name = "Authorization";
	               hdrs[n].value = smprint("Bearer %s", session); n++; }
	if(n < maxn) { hdrs[n].name = "Content-Type";
	               hdrs[n].value = "application/json"; n++; }
	if(n < maxn) { hdrs[n].name = "User-Agent";
	               hdrs[n].value = "GitHubCopilotChat/0.35.0"; n++; }
	if(n < maxn) { hdrs[n].name = "Editor-Version";
	               hdrs[n].value = "vscode/1.107.0"; n++; }
	if(n < maxn) { hdrs[n].name = "Editor-Plugin-Version";
	               hdrs[n].value = "copilot-chat/0.35.0"; n++; }
	if(n < maxn) { hdrs[n].name = "Copilot-Integration-Id";
	               hdrs[n].value = "vscode-chat"; n++; }
	if(n < maxn) { hdrs[n].name = "X-Initiator";
	               hdrs[n].value = (char *)initiator; n++; }
	if(n < maxn) { hdrs[n].name = "Openai-Intent";
	               hdrs[n].value = "conversation-edits"; n++; }

	return n;
}

/* ── SSE delta parser ─────────────────────────────────────────────────── */

/*
 * growbuf — ensure *buf is at least need bytes, doubling from 256.
 * Returns 0 on success, -1 on failure.
 */
static int
growbuf(char **buf, long *sz, long need)
{
	long newsz;
	char *p;

	if(need <= *sz)
		return 0;
	newsz = *sz ? *sz : 256;
	while(newsz < need)
		newsz *= 2;
	p = realloc(*buf, newsz);
	if(p == nil)
		return -1;
	*buf = p;
	*sz  = newsz;
	return 0;
}

/*
 * jsonstr_grow — decode JSON string token into p->textbuf, growing as needed.
 * Returns number of bytes written (>= 0), or -1 on error.
 */
static int
jsonstr_grow(ANTParser *p, const char *js, jsmntok_t *t)
{
	long need;
	int n;

	if(t->type != JSMN_STRING)
		return -1;
	need = (t->end - t->start) + 1;
	if(growbuf(&p->textbuf, &p->textbufsz, need) < 0)
		return -1;
	n = jsonstr(js, t, p->textbuf, (int)p->textbufsz);
	return n;
}

void
antinit(ANTParser *p, HTTPResp *resp)
{
	memset(p, 0, sizeof *p);
	sseinit(&p->sse, resp);
	p->block_type = -1;
}

void
antterm(ANTParser *p)
{
	sseterm(&p->sse);
	free(p->textbuf);
	p->textbuf   = nil;
	p->textbufsz = 0;
}

/*
 * antdelta — parse the next logical delta from the ANT SSE stream.
 *
 * ANT SSE event types and their handling:
 *
 *   message_start        — ignored (metadata)
 *   content_block_start  — set block_type; emit ANTDTool for tool_use
 *   content_block_delta  — emit ANTDText / ANTDThinking / ANTDToolArg
 *   content_block_stop   — reset block_type to -1
 *   message_delta        — extract stop_reason → emit ANTDStop
 *   message_stop         — next DONE will terminate; we skip this event
 *   [DONE]               — SSE_DONE → return ANT_DONE
 *
 * The caller must copy d->text/d->tool_id/d->tool_name if needed after
 * the call — internal buffers are reused.
 */
int
antdelta(ANTParser *p, ANTDelta *d)
{
	SSEEvent ev;
	int rc, ret;
	enum { MAXTOK = 256 };
	jsmn_parser jp;
	jsmntok_t *toks;
	int ntoks;

	toks = malloc(MAXTOK * sizeof(jsmntok_t));
	if(toks == nil)
		return ANT_EOF;

	ret = -1;
	while(ret < 0) {
		rc = ssestep(&p->sse, &ev);
		if(rc == SSE_DONE) { ret = ANT_DONE; break; }
		if(rc == SSE_EOF)  { ret = ANT_EOF;  break; }

		/* ev.event may be nil for lines without an "event:" prefix */
		if(ev.event == nil || ev.data == nil)
			continue;

		/* parse the JSON data payload */
		jsmn_init(&jp);
		ntoks = jsmn_parse(&jp, ev.data, strlen(ev.data), toks, MAXTOK);
		if(ntoks < 1)
			continue;

		/* ── content_block_start ────────────────────────────────── */
		if(strcmp(ev.event, "content_block_start") == 0) {
			int cb_i = jsonget(ev.data, toks, ntoks, 0, "content_block");
			if(cb_i < 0 || toks[cb_i].type != JSMN_OBJECT) continue;

			/* read type field */
			char typebuf[32];
			int type_i = jsonget(ev.data, toks, ntoks, cb_i, "type");
			if(type_i < 0 || toks[type_i].type != JSMN_STRING) continue;
			jsonstr(ev.data, &toks[type_i], typebuf, sizeof typebuf);

			if(strcmp(typebuf, "text") == 0) {
				p->block_type = ANTBlockText;

			} else if(strcmp(typebuf, "thinking") == 0) {
				p->block_type = ANTBlockThinking;

			} else if(strcmp(typebuf, "tool_use") == 0) {
				p->block_type = ANTBlockToolUse;

				/* emit ANTDTool with id and name */
				p->tool_idbuf[0]   = '\0';
				p->tool_namebuf[0] = '\0';

				int id_i   = jsonget(ev.data, toks, ntoks, cb_i, "id");
				int name_i = jsonget(ev.data, toks, ntoks, cb_i, "name");

				if(id_i >= 0 && toks[id_i].type == JSMN_STRING)
					jsonstr(ev.data, &toks[id_i], p->tool_idbuf, sizeof p->tool_idbuf);
				if(name_i >= 0 && toks[name_i].type == JSMN_STRING)
					jsonstr(ev.data, &toks[name_i], p->tool_namebuf, sizeof p->tool_namebuf);

				d->type      = ANTDTool;
				d->text      = nil;
				d->tool_id   = p->tool_idbuf;
				d->tool_name = p->tool_namebuf;
				d->stop_reason = nil;
				ret = ANT_OK;
			}
			continue;
		}

		/* ── content_block_delta ────────────────────────────────── */
		if(strcmp(ev.event, "content_block_delta") == 0) {
			int delta_i = jsonget(ev.data, toks, ntoks, 0, "delta");
			if(delta_i < 0 || toks[delta_i].type != JSMN_OBJECT) continue;

			char dtypebuf[32];
			int dtype_i = jsonget(ev.data, toks, ntoks, delta_i, "type");
			if(dtype_i < 0 || toks[dtype_i].type != JSMN_STRING) continue;
			jsonstr(ev.data, &toks[dtype_i], dtypebuf, sizeof dtypebuf);

			if(strcmp(dtypebuf, "text_delta") == 0) {
				int text_i = jsonget(ev.data, toks, ntoks, delta_i, "text");
				if(text_i < 0 || toks[text_i].type != JSMN_STRING) continue;
				int n = jsonstr_grow(p, ev.data, &toks[text_i]);
				if(n <= 0) continue;
				d->type      = ANTDText;
				d->text      = p->textbuf;
				d->tool_id   = nil;
				d->tool_name = nil;
				d->stop_reason = nil;
				ret = ANT_OK;

			} else if(strcmp(dtypebuf, "thinking_delta") == 0) {
				int think_i = jsonget(ev.data, toks, ntoks, delta_i, "thinking");
				if(think_i < 0 || toks[think_i].type != JSMN_STRING) continue;
				int n = jsonstr_grow(p, ev.data, &toks[think_i]);
				if(n <= 0) continue;
				d->type      = ANTDThinking;
				d->text      = p->textbuf;
				d->tool_id   = nil;
				d->tool_name = nil;
				d->stop_reason = nil;
				ret = ANT_OK;

			} else if(strcmp(dtypebuf, "input_json_delta") == 0) {
				int pj_i = jsonget(ev.data, toks, ntoks, delta_i, "partial_json");
				if(pj_i < 0 || toks[pj_i].type != JSMN_STRING) continue;
				int n = jsonstr_grow(p, ev.data, &toks[pj_i]);
				if(n <= 0) continue;
				d->type      = ANTDToolArg;
				d->text      = p->textbuf;
				d->tool_id   = nil;
				d->tool_name = nil;
				d->stop_reason = nil;
				ret = ANT_OK;
			}
			continue;
		}

		/* ── content_block_stop ─────────────────────────────────── */
		if(strcmp(ev.event, "content_block_stop") == 0) {
			p->block_type = -1;
			continue;
		}

		/* ── message_delta ──────────────────────────────────────── */
		if(strcmp(ev.event, "message_delta") == 0) {
			int delta_i = jsonget(ev.data, toks, ntoks, 0, "delta");
			if(delta_i < 0 || toks[delta_i].type != JSMN_OBJECT) continue;

			int sr_i = jsonget(ev.data, toks, ntoks, delta_i, "stop_reason");
			if(sr_i < 0 || toks[sr_i].type != JSMN_STRING) continue;
			int n = jsonstr(ev.data, &toks[sr_i], p->stop_reasonbuf, sizeof p->stop_reasonbuf);
			if(n <= 0) continue;

			d->type        = ANTDStop;
			d->text        = nil;
			d->tool_id     = nil;
			d->tool_name   = nil;
			d->stop_reason = p->stop_reasonbuf;
			ret = ANT_OK;
		}

		/* message_start, message_stop, ping: ignore */
	}
	free(toks);
	return ret;
}
