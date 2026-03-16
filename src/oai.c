/*
 * oai.c — OpenAI Completions request builder and SSE delta parser
 *
 * See oai.h for the interface and usage.
 *
 * ── Request serialisation ─────────────────────────────────────────────
 *
 * We write JSON directly into a growing Biobuf backed by a plan9port
 * pipe.  When we are done, we read back the accumulated bytes into a
 * malloc'd buffer and return it.
 *
 * Serialisation rules for GitHub Copilot OAI compat:
 *   - No "store" field.
 *   - System prompt goes as {"role":"system","content":"..."} first.
 *   - Assistant message "content" is always a plain string (never an array).
 *   - Tool calls appear as the "tool_calls" array on the assistant message;
 *     the "content" field is "" (empty string) when there is no text.
 *   - Tool results have role "tool" with "tool_call_id" and "content".
 *   - Max tokens field: "max_completion_tokens".
 *   - X-Initiator: "user" if last message is user; "agent" otherwise.
 *
 * ── Delta parsing ─────────────────────────────────────────────────────
 *
 * One SSE data line = one OAI choices[] chunk.  We parse:
 *
 *   delta.content            → OAIDText
 *   delta.tool_calls[].id    → OAIDTool (on first chunk with non-empty id)
 *   delta.tool_calls[].function.arguments → OAIDToolArg
 *   finish_reason != nil/null → OAIDStop
 *
 * Empty or null content chunks are skipped (no delta event emitted).
 * We rely on the invariant that each data line has at most one choice
 * (choices[0]) and at most one tool_call in tool_calls[0].
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>

#include "http.h"
#include "json.h"
#include "sse.h"
#include "oai.h"

#define COPILOT_HOST "api.individual.githubcopilot.com"
#define OAI_PATH     "/chat/completions"
#define MAX_OUT_TOK  16384

/* ── Exec tool declaration ─────────────────────────────────────────────
 *
 * Hardcoded constant; identical across all OAI requests from 9ai.
 * The "parameters" key is used for OpenAI Completions format.
 */
static const char exec_tool_json[] =
    "{\"type\":\"function\",\"function\":{"
    "\"name\":\"exec\","
    "\"description\":\"Execute a program directly (no shell). Use standard "
    "Unix programs for all file operations: cat/head to read, "
    "grep/find to search, ed/patch/awk/sed to edit, cc/mk to compile.\","
    "\"parameters\":{"
    "\"type\":\"object\","
    "\"properties\":{"
    "\"argv\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
    "\"description\":\"Program and arguments, e.g. [\\\"grep\\\",\\\"-r\\\","
    "\\\"foo\\\",\\\"src\\\"]\"},"
    "\"stdin\":{\"type\":\"string\","
    "\"description\":\"Optional text to supply as stdin.\"},"
    "\"timeout\":{\"type\":\"integer\","
    "\"description\":\"Timeout in seconds before the process is killed. "
    "Defaults to 30. Increase for long-running builds or tests.\"}"
    "},"
    "\"required\":[\"argv\"]"
    "}}}";

/* ── Block and message constructors ────────────────────────────────────── */

static OAIBlock *
newblock(int type)
{
	OAIBlock *b = mallocz(sizeof *b, 1);
	if(b == nil)
		sysfatal("mallocz: %r");
	b->type = type;
	return b;
}

static OAIMsg *
newmsg(int role)
{
	OAIMsg *m = mallocz(sizeof *m, 1);
	if(m == nil)
		sysfatal("mallocz: %r");
	m->role = role;
	return m;
}

static void
appendblock(OAIMsg *m, OAIBlock *b)
{
	OAIBlock **pp = &m->content;
	while(*pp != nil)
		pp = &(*pp)->next;
	*pp = b;
}

OAIMsg *
oaimsguser(char *text)
{
	OAIMsg  *m = newmsg(OAIRoleUser);
	OAIBlock *b = newblock(OAIBlockText);
	b->text = strdup(text);
	appendblock(m, b);
	return m;
}

OAIMsg *
oaimsgassistant(char *text)
{
	OAIMsg  *m = newmsg(OAIRoleAssistant);
	OAIBlock *b = newblock(OAIBlockText);
	b->text = strdup(text != nil ? text : "");
	appendblock(m, b);
	return m;
}

OAIMsg *
oaimsgtoolcall(char *text, char *tool_id, char *tool_name, char *tool_args)
{
	OAIMsg  *m = newmsg(OAIRoleAssistant);
	OAIBlock *b;

	/* text block (may be empty) */
	b = newblock(OAIBlockText);
	b->text = strdup(text != nil ? text : "");
	appendblock(m, b);

	/* tool call block */
	b = newblock(OAIBlockToolCall);
	b->tool_id   = strdup(tool_id);
	b->tool_name = strdup(tool_name);
	b->tool_args = strdup(tool_args != nil ? tool_args : "{}");
	appendblock(m, b);
	return m;
}

OAIMsg *
oaimsgtoolresult(char *tool_id, char *output, int is_error)
{
	OAIMsg  *m = newmsg(OAIRoleTool);
	OAIBlock *b = newblock(OAIBlockToolResult);
	b->tool_id  = strdup(tool_id);
	b->text     = strdup(output != nil ? output : "");
	b->is_error = is_error;
	appendblock(m, b);
	return m;
}

/* ── OAIReq lifecycle ──────────────────────────────────────────────────── */

OAIReq *
oaireqnew(char *model)
{
	OAIReq *req = mallocz(sizeof *req, 1);
	if(req == nil)
		sysfatal("mallocz: %r");
	req->model = strdup(model);
	return req;
}

void
oaireqaddmsg(OAIReq *req, OAIMsg *msg)
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
oaireqctxtokens(OAIReq *req)
{
	OAIMsg   *m;
	OAIBlock *b;
	long      chars = 0;

	for(m = req->msgs; m != nil; m = m->next)
		for(b = m->content; b != nil; b = b->next) {
			if(b->text      != nil) chars += strlen(b->text);
			if(b->tool_args != nil) chars += strlen(b->tool_args);
		}
	return (chars + 3) / 4;  /* ceil(chars/4) */
}

static void
blockfree(OAIBlock *b)
{
	if(b == nil) return;
	free(b->text);
	free(b->tool_id);
	free(b->tool_name);
	free(b->tool_args);
	free(b);
}

static void
msgfree(OAIMsg *m)
{
	OAIBlock *b, *nb;
	if(m == nil) return;
	for(b = m->content; b != nil; b = nb) {
		nb = b->next;
		blockfree(b);
	}
	free(m);
}

void
oaireqfree(OAIReq *req)
{
	OAIMsg *m, *nm;
	if(req == nil) return;
	free(req->model);
	for(m = req->msgs; m != nil; m = nm) {
		nm = m->next;
		msgfree(m);
	}
	free(req);
}

int
oaireqtrim(OAIReq *req, int nturns)
{
	OAIMsg *m, *next;
	int     turns, removed;

	if(nturns <= 0 || req->msgs == nil)
		return 0;

	/*
	 * Walk forward, counting user messages as turn boundaries.
	 * We stop when we've seen nturns user messages and the next
	 * message is also a user message (i.e. we're at the start of
	 * an un-trimmed turn) — or we run out of messages.
	 *
	 * The cut point is the first message of the first turn to keep.
	 */
	turns = 0;
	m = req->msgs;
	while(m != nil) {
		if(m->role == OAIRoleUser) {
			if(turns == nturns)
				break;   /* m is the first message to keep */
			turns++;
		}
		m = m->next;
	}
	/* m == nil means we consumed everything (trim all) */

	/* free everything from req->msgs up to (but not including) m */
	removed = 0;
	next = req->msgs;
	while(next != m) {
		OAIMsg *cur = next;
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
 * emitmsg — write one message to b in OAI wire format.
 *
 * user:      {"role":"user","content":<string>}
 * assistant: {"role":"assistant","content":<string>,"tool_calls":[...]}
 *   content is the concatenation of all OAIBlockText blocks.
 *   GitHub Copilot requires a plain string, not an array.
 * tool:      {"role":"tool","tool_call_id":<id>,"content":<string>}
 */
static void
emitmsg(Biobuf *b, OAIMsg *m)
{
	OAIBlock *blk;

	switch(m->role) {
	case OAIRoleUser:
		Bprint(b, "{\"role\":\"user\",\"content\":");
		/* collect all text blocks (should be exactly one) */
		for(blk = m->content; blk != nil; blk = blk->next)
			if(blk->type == OAIBlockText)
				jsonemitstr(b, blk->text);
		Bprint(b, "}");
		break;

	case OAIRoleAssistant: {
		int has_toolcalls = 0;
		/* check for tool call blocks */
		for(blk = m->content; blk != nil; blk = blk->next)
			if(blk->type == OAIBlockToolCall) { has_toolcalls = 1; break; }

		Bprint(b, "{\"role\":\"assistant\",\"content\":");

		/* content: concatenate all text blocks into one string.
		 * GitHub Copilot requires a plain string, not an array.
		 *
		 * Use a growable heap buffer instead of a pipe to avoid the
		 * deadlock that occurred when assistant text exceeded the pipe
		 * buffer capacity (~73 KB: 8 KB Biobuf + 65536 B pipe), and the
		 * silent 65535-byte truncation on the read side. */
		{
			char *buf = nil;
			char *tmp;
			long  cap = 0, len = 0, dlen, newcap;

			for(blk = m->content; blk != nil; blk = blk->next) {
				if(blk->type != OAIBlockText || blk->text == nil)
					continue;
				dlen = strlen(blk->text);
				if(len + dlen + 1 > cap) {
					newcap = cap ? cap * 2 : 4096;
					while(newcap < len + dlen + 1)
						newcap *= 2;
					tmp = realloc(buf, newcap);
					if(tmp == nil) { free(buf); buf = nil; break; }
					buf = tmp;
					cap = newcap;
				}
				memmove(buf + len, blk->text, dlen);
				len += dlen;
			}
			if(buf != nil) {
				buf[len] = '\0';
				jsonemitstr(b, buf);
				free(buf);
			} else {
				Bprint(b, "\"\"");
			}
		}

		/* tool_calls array */
		if(has_toolcalls) {
			int first = 1;
			Bprint(b, ",\"tool_calls\":[");
			for(blk = m->content; blk != nil; blk = blk->next) {
				if(blk->type != OAIBlockToolCall) continue;
				if(!first) Bprint(b, ",");
				first = 0;
				Bprint(b, "{\"id\":");
				jsonemitstr(b, blk->tool_id);
				Bprint(b, ",\"type\":\"function\",\"function\":{\"name\":");
				jsonemitstr(b, blk->tool_name);
				Bprint(b, ",\"arguments\":");
				jsonemitstr(b, blk->tool_args != nil ? blk->tool_args : "{}");
				Bprint(b, "}}");
			}
			Bprint(b, "]");
		}

		Bprint(b, "}");
		break;
	}

	case OAIRoleTool:
		/* one block per tool result message */
		for(blk = m->content; blk != nil; blk = blk->next) {
			if(blk->type != OAIBlockToolResult) continue;
			Bprint(b, "{\"role\":\"tool\",\"tool_call_id\":");
			jsonemitstr(b, blk->tool_id);
			Bprint(b, ",\"content\":");
			jsonemitstr(b, blk->text);
			Bprint(b, "}");
			/* only one result per message; break after first */
			break;
		}
		break;
	}
}

char *
oaireqjson(OAIReq *req, char *system_prompt, long *lenp)
{
	int pfd[2];
	Biobuf *b;
	OAIMsg *m;
	char *buf;
	long cap, len, n;

	enum { READCHUNK = 8192 };

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
	Bprint(b, ",\"max_completion_tokens\":%d", MAX_OUT_TOK);
	Bprint(b, ",\"messages\":[");

	/* system prompt first */
	int first = 1;
	if(system_prompt != nil && system_prompt[0] != '\0') {
		Bprint(b, "{\"role\":\"system\",\"content\":");
		jsonemitstr(b, system_prompt);
		Bprint(b, "}");
		first = 0;
	}

	for(m = req->msgs; m != nil; m = m->next) {
		if(!first) Bprint(b, ",");
		first = 0;
		emitmsg(b, m);
	}

	Bprint(b, "]");
	Bprint(b, ",\"tools\":[%s]", exec_tool_json);
	Bprint(b, "}");

	Bflush(b);
	Bterm(b);
	free(b);
	close(pfd[1]);

	/* read back into malloc'd buffer */
	cap = READCHUNK;
	len = 0;
	buf = malloc(cap + 1);
	if(buf == nil) {
		close(pfd[0]);
		return nil;
	}
	for(;;) {
		if(len + READCHUNK > cap) {
			cap *= 2;
			buf = realloc(buf, cap + 1);
			if(buf == nil) {
				close(pfd[0]);
				return nil;
			}
		}
		n = read(pfd[0], buf + len, READCHUNK);
		if(n <= 0) break;
		len += n;
	}
	close(pfd[0]);
	buf[len] = '\0';
	if(lenp != nil)
		*lenp = len;
	return buf;
}

/*
 * oaireqhdrs — fill Copilot request headers.
 * X-Initiator is "user" if the last message is a user message, "agent" otherwise.
 */
int
oaireqhdrs(OAIReq *req, char *session, HTTPHdr *hdrs, int maxn)
{
	const char *initiator;
	int n = 0;

	/* Determine X-Initiator from last message role */
	initiator = "user";
	if(req->msgtail != nil && req->msgtail->role != OAIRoleUser)
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
 * growbuf — ensure *buf is at least need bytes, doubling from SSE_INITBUFSZ.
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
jsonstr_grow(OAIParser *p, const char *js, jsmntok_t *t)
{
	long need;
	int n;

	if(t->type != JSMN_STRING)
		return -1;
	need = (t->end - t->start) + 1;  /* upper bound: raw >= decoded */
	if(growbuf(&p->textbuf, &p->textbufsz, need) < 0)
		return -1;
	n = jsonstr(js, t, p->textbuf, (int)p->textbufsz);
	return n;
}

void
oaiinit(OAIParser *p, HTTPResp *resp)
{
	memset(p, 0, sizeof *p);
	sseinit(&p->sse, resp);
}

void
oaiterm(OAIParser *p)
{
	sseterm(&p->sse);
	free(p->textbuf);
	p->textbuf   = nil;
	p->textbufsz = 0;
}

/*
 * oaidelta — parse the next logical delta from the SSE stream.
 *
 * OAI chunk structure (the "choices[0].delta" sub-object):
 *   delta.content             — text chunk (string or null)
 *   delta.reasoning_content   — reasoning/thinking chunk (llama.cpp)
 *   delta.reasoning           — reasoning/thinking chunk (DeepSeek, generic)
 *   delta.reasoning_text      — reasoning/thinking chunk (some others)
 *   delta.tool_calls[0].id   — tool call id (first chunk only, then "")
 *   delta.tool_calls[0].function.name      — function name (first chunk only)
 *   delta.tool_calls[0].function.arguments — partial JSON args
 *
 * finish_reason field appears on the final chunk for a choice:
 *   "stop"       — text turn complete
 *   "tool_calls" — tool call complete
 *
 * We issue one OAIDelta per event:
 *   - OAIDThinking for non-empty reasoning_content / reasoning / reasoning_text
 *   - OAIDText for non-empty content
 *   - OAIDTool when a tool call starts (id non-empty)
 *   - OAIDToolArg for each non-empty arguments chunk
 *   - OAIDStop for finish_reason
 *   - Empty/null fields are skipped (we loop internally)
 *
 * Reasoning fields are checked before content so that chunks containing both
 * (unlikely but possible) emit thinking first.  We take only the first
 * non-empty reasoning field to avoid duplicate emission on providers that
 * return multiple synonymous fields (e.g. chutes.ai).
 */
int
oaidelta(OAIParser *p, OAIDelta *d)
{
	SSEEvent ev;
	int rc, ret;
	enum { MAXTOK = 256 };
	jsmn_parser jp;
	jsmntok_t *toks;
	int ntoks;
	int choices_i, choice_i, delta_i, fr_i;
	int tc_i, tc0_i, fn_i;
	char buf[128];

	toks = malloc(MAXTOK * sizeof(jsmntok_t));
	if(toks == nil)
		return OAI_EOF;

	ret = -1;
	while(ret < 0) {
		rc = ssestep(&p->sse, &ev);
		if(rc == SSE_DONE) { ret = OAI_DONE; break; }
		if(rc == SSE_EOF)  { ret = OAI_EOF;  break; }

		/* parse the JSON chunk */
		jsmn_init(&jp);
		ntoks = jsmn_parse(&jp, ev.data, strlen(ev.data), toks, MAXTOK);
		if(ntoks < 1)
			continue;

		/* root is object; find "choices" array */
		choices_i = jsonget(ev.data, toks, ntoks, 0, "choices");
		if(choices_i < 0 || toks[choices_i].type != JSMN_ARRAY)
			continue;
		if(toks[choices_i].size == 0)
			continue;

		/* choices[0] */
		choice_i = choices_i + 1;

		/* check finish_reason first */
		fr_i = jsonget(ev.data, toks, ntoks, choice_i, "finish_reason");
		if(fr_i >= 0 && toks[fr_i].type != JSMN_UNDEFINED) {
			/* finish_reason may be null or a string */
			if(toks[fr_i].type == JSMN_STRING) {
				jsonstr(ev.data, &toks[fr_i], p->stop_reasonbuf,
				        sizeof p->stop_reasonbuf);
				if(p->stop_reasonbuf[0] != '\0') {
					d->type        = OAIDStop;
					d->text        = nil;
					d->tool_id     = nil;
					d->tool_name   = nil;
					d->stop_reason = p->stop_reasonbuf;
					ret = OAI_OK;
					continue;
				}
			}
		}

		/* find "delta" object inside choices[0] */
		delta_i = jsonget(ev.data, toks, ntoks, choice_i, "delta");
		if(delta_i < 0 || toks[delta_i].type != JSMN_OBJECT)
			continue;

		/* check for "content" field */
		{
			int content_i = jsonget(ev.data, toks, ntoks, delta_i, "content");
			if(content_i >= 0 && toks[content_i].type == JSMN_STRING) {
				int n = jsonstr_grow(p, ev.data, &toks[content_i]);
				if(n > 0) {
					d->type      = OAIDText;
					d->text      = p->textbuf;
					d->tool_id   = nil;
					d->tool_name = nil;
					d->stop_reason = nil;
					ret = OAI_OK;
					continue;
				}
			}
		}

		/*
		 * Check for reasoning/thinking fields.
		 * Different providers use different field names; probe in priority
		 * order and take the first non-empty one:
		 *   "reasoning_content" — llama.cpp
		 *   "reasoning"         — DeepSeek, generic OAI-compat
		 *   "reasoning_text"    — some others
		 * We emit OAIDThinking; the caller (agentrun) saves it to the session
		 * and event stream but does NOT feed it back to the API.
		 */
		{
			static const char *rfields[] = {
				"reasoning_content", "reasoning", "reasoning_text", nil
			};
			int ri;
			for(ri = 0; rfields[ri] != nil; ri++) {
				int r_i = jsonget(ev.data, toks, ntoks, delta_i, rfields[ri]);
				if(r_i >= 0 && toks[r_i].type == JSMN_STRING) {
					int n = jsonstr_grow(p, ev.data, &toks[r_i]);
					if(n > 0) {
						d->type        = OAIDThinking;
						d->text        = p->textbuf;
						d->tool_id     = nil;
						d->tool_name   = nil;
						d->stop_reason = nil;
						ret = OAI_OK;
						break;
					}
				}
			}
			if(ret == OAI_OK)
				continue;
		}

		tc_i = jsonget(ev.data, toks, ntoks, delta_i, "tool_calls");
		if(tc_i < 0 || toks[tc_i].type != JSMN_ARRAY || toks[tc_i].size == 0)
			continue;

		tc0_i = tc_i + 1;  /* tool_calls[0] object */

		/* id: non-empty on first chunk for a given tool call */
		{
			int id_i = jsonget(ev.data, toks, ntoks, tc0_i, "id");
			if(id_i >= 0 && toks[id_i].type == JSMN_STRING) {
				int n = jsonstr(ev.data, &toks[id_i],
				                p->tool_idbuf, sizeof p->tool_idbuf);
				if(n > 0) {
					/* new tool call starting */
					p->tool_namebuf[0] = '\0';
					fn_i = jsonget(ev.data, toks, ntoks, tc0_i, "function");
					if(fn_i >= 0 && toks[fn_i].type == JSMN_OBJECT) {
						int name_i = jsonget(ev.data, toks, ntoks, fn_i, "name");
						if(name_i >= 0 && toks[name_i].type == JSMN_STRING)
							jsonstr(ev.data, &toks[name_i],
							        p->tool_namebuf, sizeof p->tool_namebuf);
					}
					d->type      = OAIDTool;
					d->text      = nil;
					d->tool_id   = p->tool_idbuf;
					d->tool_name = p->tool_namebuf;
					d->stop_reason = nil;
					ret = OAI_OK;
					continue;
				}
			}
		}

		/* arguments chunk */
		fn_i = jsonget(ev.data, toks, ntoks, tc0_i, "function");
		if(fn_i >= 0 && toks[fn_i].type == JSMN_OBJECT) {
			int args_i = jsonget(ev.data, toks, ntoks, fn_i, "arguments");
			if(args_i >= 0 && toks[args_i].type == JSMN_STRING) {
				int n = jsonstr_grow(p, ev.data, &toks[args_i]);
				if(n > 0) {
					d->type      = OAIDToolArg;
					d->text      = p->textbuf;
					d->tool_id   = nil;
					d->tool_name = nil;
					d->stop_reason = nil;
					ret = OAI_OK;
					continue;
				}
			}
		}

		/* nothing actionable in this chunk; loop for next SSE event */
		USED(buf);
	}
	free(toks);
	return ret;
}
