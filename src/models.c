/*
 * models.c — /models fetch and parser
 *
 * See models.h for the interface.
 *
 * Response shape (abbreviated):
 *
 *   {
 *     "data": [
 *       {
 *         "id": "claude-sonnet-4.5",
 *         "name": "Claude Sonnet 4.5",
 *         "vendor": "Anthropic",
 *         "model_picker_enabled": true,
 *         "supported_endpoints": ["/v1/messages", "/chat/completions"],
 *         "capabilities": {
 *           "limits": {
 *             "max_context_window_tokens": 200000,
 *             "max_output_tokens": 32000
 *           },
 *           "supports": {
 *             "tool_calls": true
 *           }
 *         }
 *       },
 *       ...
 *     ]
 *   }
 *
 * Traversal:
 *   root → "data" array
 *   each element → "id", "name", "vendor", "model_picker_enabled"
 *   each element → "supported_endpoints" array (scan for endpoint strings)
 *   each element → "capabilities" → "limits" → max_context_window_tokens,
 *                                               max_output_tokens
 *   each element → "capabilities" → "supports" → "tool_calls"
 *
 * The JSON body can be ~32KB with ~2500 jsmn tokens.  We allocate 6000
 * tokens to have headroom.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>

#include "http.h"
#include "json.h"
#include "models.h"

#define COPILOT_HOST "api.individual.githubcopilot.com"
#define MODELS_PATH  "/models"
#define MAXTOK       6000

/* ── Internal parse helpers ────────────────────────────────────────── */

/*
 * endpoint_fmt — walk a supported_endpoints array and return the
 * preferred wire format: Fmt_Ant if /v1/messages is present,
 * Fmt_Oai if /chat/completions is present, -1 if neither.
 *
 * ep_tok: index of the JSMN_ARRAY token in toks.
 */
static int
endpoint_fmt(const char *js, jsmntok_t *toks, int ntoks, int ep_tok)
{
	int i, elem;
	char buf[64];
	int has_ant = 0, has_oai = 0;

	if(ep_tok < 0 || toks[ep_tok].type != JSMN_ARRAY)
		return -1;

	elem = ep_tok + 1;
	for(i = 0; i < toks[ep_tok].size && elem < ntoks; i++) {
		jsonstr(js, &toks[elem], buf, sizeof buf);
		if(strcmp(buf, "/v1/messages") == 0)
			has_ant = 1;
		else if(strcmp(buf, "/chat/completions") == 0)
			has_oai = 1;
		elem = jsonnext(toks, ntoks, elem);
	}
	if(has_ant) return Fmt_Ant;
	if(has_oai) return Fmt_Oai;
	return -1;
}

/*
 * parse_model — fill in a Model from a single element of the "data" array.
 *
 * elem: index of the JSMN_OBJECT token for this model element.
 * Returns 0 on success, -1 if mandatory fields are missing or the model
 * should be skipped (model_picker_enabled=false, no usable endpoint).
 */
static int
parse_model(const char *js, jsmntok_t *toks, int ntoks, int elem, Model *m)
{
	int vi, caps, lim, sup;
	char buf[1024];

	/* id */
	vi = jsonget(js, toks, ntoks, elem, "id");
	if(vi < 0) return -1;
	jsonstr(js, &toks[vi], buf, sizeof buf);
	m->id = strdup(buf);

	/* name */
	vi = jsonget(js, toks, ntoks, elem, "name");
	if(vi >= 0) {
		jsonstr(js, &toks[vi], buf, sizeof buf);
		m->name = strdup(buf);
	} else {
		m->name = strdup(m->id);
	}

	/* vendor */
	vi = jsonget(js, toks, ntoks, elem, "vendor");
	if(vi >= 0) {
		jsonstr(js, &toks[vi], buf, sizeof buf);
		m->vendor = strdup(buf);
	} else {
		m->vendor = strdup("");
	}

	/* model_picker_enabled — primitive: true/false */
	vi = jsonget(js, toks, ntoks, elem, "model_picker_enabled");
	if(vi >= 0 && toks[vi].type == JSMN_PRIMITIVE) {
		if(js[toks[vi].start] != 't') {
			/* false — skip */
			free(m->id); free(m->name); free(m->vendor);
			return -1;
		}
	}

	/* supported_endpoints → wire format */
	vi = jsonget(js, toks, ntoks, elem, "supported_endpoints");
	m->fmt = endpoint_fmt(js, toks, ntoks, vi);
	if(m->fmt < 0) {
		/* no usable endpoint */
		free(m->id); free(m->name); free(m->vendor);
		return -1;
	}

	/* capabilities → limits → max_context_window_tokens, max_output_tokens */
	m->ctx_k    = 0;
	m->maxout_k = 0;
	caps = jsonget(js, toks, ntoks, elem, "capabilities");
	if(caps >= 0 && toks[caps].type == JSMN_OBJECT) {
		lim = jsonget(js, toks, ntoks, caps, "limits");
		if(lim >= 0 && toks[lim].type == JSMN_OBJECT) {
			vi = jsonget(js, toks, ntoks, lim, "max_context_window_tokens");
			if(vi >= 0)
				m->ctx_k = jsonint(js, &toks[vi]) / 1000;

			vi = jsonget(js, toks, ntoks, lim, "max_output_tokens");
			if(vi >= 0)
				m->maxout_k = jsonint(js, &toks[vi]) / 1000;
		}

		/* capabilities → supports → tool_calls */
		sup = jsonget(js, toks, ntoks, caps, "supports");
		if(sup >= 0 && toks[sup].type == JSMN_OBJECT) {
			vi = jsonget(js, toks, ntoks, sup, "tool_calls");
			if(vi >= 0 && toks[vi].type == JSMN_PRIMITIVE)
				m->tools = (js[toks[vi].start] == 't');
		}
	}

	return 0;
}

/*
 * parsemodels — parse a /models JSON body into a Model list.
 * Returns the head of a singly-linked list, or nil on error.
 */
static Model *
parsemodels(const char *js, jsmntok_t *toks, int ntoks)
{
	int data_tok, i, elem;
	Model *head, **tail, *m;

	data_tok = jsonget(js, toks, ntoks, 0, "data");
	if(data_tok < 0 || toks[data_tok].type != JSMN_ARRAY) {
		werrstr("models: no 'data' array in response");
		return nil;
	}

	head = nil;
	tail = &head;
	elem = data_tok + 1;

	for(i = 0; i < toks[data_tok].size && elem < ntoks; i++) {
		int next = jsonnext(toks, ntoks, elem);

		if(toks[elem].type == JSMN_OBJECT) {
			m = mallocz(sizeof *m, 1);
			if(m == nil)
				break;
			if(parse_model(js, toks, ntoks, elem, m) == 0) {
				*tail = m;
				tail  = &m->next;
			} else {
				free(m);
			}
		}

		elem = next;
	}

	return head;
}

/* ── Public API ─────────────────────────────────────────────────────── */

Model *
modelsfetch(char *session, char *sockpath)
{
	HTTPConn   *c;
	HTTPResp   *r;
	jsmntok_t  *toks;
	jsmn_parser p;
	Model      *list;
	char        auth[1200];
	HTTPHdr     hdrs[8];
	int         nhdrs, n;

	snprint(auth, sizeof auth, "Bearer %s", session);

	nhdrs = 0;
	hdrs[nhdrs].name  = "Authorization";         hdrs[nhdrs].value = auth;                        nhdrs++;
	hdrs[nhdrs].name  = "Content-Type";          hdrs[nhdrs].value = "application/json";          nhdrs++;
	hdrs[nhdrs].name  = "User-Agent";            hdrs[nhdrs].value = "GitHubCopilotChat/0.35.0";  nhdrs++;
	hdrs[nhdrs].name  = "Editor-Version";        hdrs[nhdrs].value = "vscode/1.107.0";            nhdrs++;
	hdrs[nhdrs].name  = "Editor-Plugin-Version"; hdrs[nhdrs].value = "copilot-chat/0.35.0";       nhdrs++;
	hdrs[nhdrs].name  = "Copilot-Integration-Id"; hdrs[nhdrs].value = "vscode-chat";              nhdrs++;

	c = httpdial(sockpath);
	if(c == nil)
		return nil;

	r = httpget(c, MODELS_PATH, COPILOT_HOST, hdrs, nhdrs);
	if(r == nil) {
		httpclose(c);
		return nil;
	}
	if(httpreadbody(r) < 0) {
		httprespfree(r);
		httpclose(c);
		return nil;
	}
	if(r->code != 200) {
		werrstr("models: HTTP %d: %.120s", r->code, r->body ? r->body : "");
		httprespfree(r);
		httpclose(c);
		return nil;
	}

	toks = mallocz(MAXTOK * sizeof *toks, 1);
	if(toks == nil) {
		httprespfree(r);
		httpclose(c);
		return nil;
	}

	jsmn_init(&p);
	n = jsmn_parse(&p, r->body, r->bodylen, toks, MAXTOK);
	if(n < 0) {
		werrstr("models: JSON parse error %d", n);
		free(toks);
		httprespfree(r);
		httpclose(c);
		return nil;
	}

	list = parsemodels(r->body, toks, n);

	free(toks);
	httprespfree(r);
	httpclose(c);
	return list;
}

void
modelsfmt(Model *list, Biobuf *b)
{
	Model *m;

	for(m = list; m != nil; m = m->next) {
		Bprint(b, "%s\t%s\t%ldk\t%ldk\t%s\t%s\n",
			m->id,
			m->fmt == Fmt_Ant ? "ant" : "oai",
			m->ctx_k,
			m->maxout_k,
			m->tools ? "y" : "n",
			m->name);
	}
}

void
modelsfree(Model *list)
{
	Model *m, *next;

	for(m = list; m != nil; m = next) {
		next = m->next;
		free(m->id);
		free(m->name);
		free(m->vendor);
		free(m);
	}
}
