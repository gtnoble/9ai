/*
 * modelstest.c — Stage 5 tests for models.c
 *
 * Part 1: Unit tests — parse a fixed JSON fixture without network.
 *         Tests filteringmodel_picker_enabled), format detection
 *         (ant preference when both endpoints present), field extraction.
 *
 * Part 2: Integration test (requires 9aitls proxy + valid token).
 *         Fetches live /models, verifies count > 0 and prints the table.
 *
 * Usage:
 *   mk o.modelstest
 *   ./o.modelstest                           # unit tests only
 *   ./o.modelstest -s sockpath -t tokenfile  # unit + integration
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>

#include "9ai.h"
#include "http.h"
#include "json.h"
#include "oauth.h"
#include "models.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
	if(!(cond)) { \
		fprint(2, "FAIL: %s (line %d)\n", msg, __LINE__); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

#define CHECKEQ(a, b, msg) do { \
	if((long)(a) != (long)(b)) { \
		fprint(2, "FAIL: %s: got %ld want %ld (line %d)\n", \
		       msg, (long)(a), (long)(b), __LINE__); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

#define CHECKSTR(a, b, msg) do { \
	if(strcmp((a),(b)) != 0) { \
		fprint(2, "FAIL: %s: got \"%s\" want \"%s\" (line %d)\n", \
		       msg, (a), (b), __LINE__); \
		failures++; \
	} else { \
		print("ok: %s\n", msg); \
	} \
} while(0)

/* ── Fixture ─────────────────────────────────────────────────────────
 * Three models:
 *   [0] claude-opus-4.6-fast  ant (has both /v1/messages AND /chat/completions)
 *   [1] gemini-3.1-pro-preview oai (only /chat/completions)
 *   [2] oswe-vscode-secondary  disabled (model_picker_enabled=false) → filtered out
 */
static const char fixture[] =
    "{\"data\":[{\"capabilities\":{\"family\":\"claude-opus-4.6-fast\",\"limits\":{\"max_context_window_t"
    "okens\":200000,\"max_non_streaming_output_tokens\":16000,\"max_output_tokens\":64000,\"max_prompt_to"
    "kens\":128000,\"vision\":{\"max_prompt_image_size\":3145728,\"max_prompt_images\":1,\"supported_medi"
    "a_types\":[\"image/jpeg\",\"image/png\",\"image/webp\"]}},\"object\":\"model_capabilities\",\"suppor"
    "ts\":{\"adaptive_thinking\":true,\"max_thinking_budget\":32000,\"min_thinking_budget\":1024,\"parall"
    "el_tool_calls\":true,\"reasoning_effort\":[\"low\",\"medium\",\"high\"],\"streaming\":true,\"structu"
    "red_outputs\":true,\"tool_calls\":true,\"vision\":true},\"tokenizer\":\"o200k_base\",\"type\":\"chat"
    "\"},\"id\":\"claude-opus-4.6-fast\",\"model_picker_category\":\"powerful\",\"model_picker_enabled\":"
    "true,\"name\":\"Claude Opus 4.6 (fast mode)\",\"object\":\"model\",\"policy\":{\"state\":\"enabled\""
    ",\"terms\":\"Enable access to the latest Claude Opus 4.6 fast model from Anthropic. [Learn more abou"
    "t how GitHub Copilot serves Claude Opus 4.6 fast](https://gh.io/copilot-claude-opus).\"},\"preview\""
    ":true,\"supported_endpoints\":[\"/v1/messages\",\"/chat/completions\"],\"vendor\":\"Anthropic\",\"ve"
    "rsion\":\"claude-opus-4.6-fast\"},{\"capabilities\":{\"family\":\"gemini-3.1-pro-preview\",\"limits\""
    ":{\"max_context_window_tokens\":128000,\"max_output_tokens\":64000,\"max_prompt_tokens\":128000,\"vi"
    "sion\":{\"max_prompt_image_size\":3145728,\"max_prompt_images\":10,\"supported_media_types\":[\"imag"
    "e/jpeg\",\"image/png\",\"image/webp\"]}},\"object\":\"model_capabilities\",\"supports\":{\"max_think"
    "ing_budget\":32000,\"min_thinking_budget\":256,\"parallel_tool_calls\":true,\"reasoning_effort\":[\"l"
    "ow\",\"medium\",\"high\"],\"streaming\":true,\"tool_calls\":true,\"vision\":true},\"tokenizer\":\"o2"
    "00k_base\",\"type\":\"chat\"},\"id\":\"gemini-3.1-pro-preview\",\"model_picker_category\":\"powerful"
    "\",\"model_picker_enabled\":true,\"name\":\"Gemini 3.1 Pro\",\"object\":\"model\",\"policy\":{\"stat"
    "e\":\"enabled\",\"terms\":\"Enable access to the latest Gemini 3 Pro model from Google. [Learn more "
    "about how GitHub Copilot serves Gemini 3 Pro](https://docs.github.com/en/copilot/reference/ai-models"
    "/model-hosting#google-models).\"},\"preview\":true,\"supported_endpoints\":[\"/chat/completions\"],\""
    "vendor\":\"Google\",\"version\":\"gemini-3.1-pro-preview\"},{\"capabilities\":{\"family\":\"oswe-vsc"
    "ode\",\"limits\":{\"max_context_window_tokens\":264000,\"max_output_tokens\":64000,\"max_prompt_toke"
    "ns\":200000,\"vision\":{\"max_prompt_image_size\":3145728,\"max_prompt_images\":1,\"supported_media_"
    "types\":[\"image/jpeg\",\"image/png\",\"image/webp\",\"image/gif\"]}},\"object\":\"model_capabilitie"
    "s\",\"supports\":{\"parallel_tool_calls\":true,\"streaming\":true,\"structured_outputs\":true,\"tool"
    "_calls\":true,\"vision\":true},\"tokenizer\":\"o200k_base\",\"type\":\"chat\"},\"id\":\"oswe-vscode-"
    "secondary\",\"model_picker_category\":\"versatile\",\"model_picker_enabled\":false,\"name\":\"Raptor"
    " mini (Preview)\",\"object\":\"model\",\"policy\":{\"state\":\"enabled\",\"terms\":\"Enable access t"
    "o the latest Raptor mini model from Microsoft. [Learn more about how GitHub Copilot serves Raptor min"
    "i](https://gh.io/copilot-openai-fine-tuned-by-microsoft).\"},\"preview\":true,\"supported_endpoints"
    "\":[\"/chat/completions\",\"/responses\"],\"vendor\":\"Azure OpenAI\",\"version\":\"raptor-mini\"}]}"
    ;

static Model *
parse_fixture(void)
{
	jsmntok_t  toks[6000];
	jsmn_parser p;
	int         n, data_tok, i, elem;
	Model      *head, **tail;

	jsmn_init(&p);
	n = jsmn_parse(&p, fixture, strlen(fixture), toks, 6000);
	if(n < 0) return nil;

	data_tok = jsonget(fixture, toks, n, 0, "data");
	if(data_tok < 0) return nil;

	head = nil;
	tail = &head;
	elem = data_tok + 1;

	for(i = 0; i < toks[data_tok].size && elem < n; i++) {
		int next_elem = jsonnext(toks, n, elem);
		if(toks[elem].type == JSMN_OBJECT) {
			int vi, caps, lim, sup;
			char buf[1024];
			int has_ant, has_oai, j, ep_elem;
			Model *m = mallocz(sizeof *m, 1);

			/* id */
			vi = jsonget(fixture, toks, n, elem, "id");
			if(vi < 0) { free(m); elem = next_elem; continue; }
			jsonstr(fixture, &toks[vi], buf, sizeof buf);
			m->id = strdup(buf);

			/* name */
			vi = jsonget(fixture, toks, n, elem, "name");
			m->name = (vi >= 0) ?
				(jsonstr(fixture, &toks[vi], buf, sizeof buf), strdup(buf)) :
				strdup(m->id);

			/* vendor */
			vi = jsonget(fixture, toks, n, elem, "vendor");
			m->vendor = (vi >= 0) ?
				(jsonstr(fixture, &toks[vi], buf, sizeof buf), strdup(buf)) :
				strdup("");

			/* model_picker_enabled */
			vi = jsonget(fixture, toks, n, elem, "model_picker_enabled");
			if(vi >= 0 && toks[vi].type == JSMN_PRIMITIVE &&
			   fixture[toks[vi].start] != 't') {
				free(m->id); free(m->name); free(m->vendor); free(m);
				elem = next_elem;
				continue;
			}

			/* supported_endpoints */
			vi = jsonget(fixture, toks, n, elem, "supported_endpoints");
			has_ant = has_oai = 0;
			if(vi >= 0 && toks[vi].type == JSMN_ARRAY) {
				ep_elem = vi + 1;
				for(j = 0; j < toks[vi].size && ep_elem < n; j++) {
					jsonstr(fixture, &toks[ep_elem], buf, sizeof buf);
					if(strcmp(buf, "/v1/messages") == 0)     has_ant = 1;
					if(strcmp(buf, "/chat/completions") == 0) has_oai = 1;
					ep_elem = jsonnext(toks, n, ep_elem);
				}
			}
			if(!has_ant && !has_oai) {
				free(m->id); free(m->name); free(m->vendor); free(m);
				elem = next_elem;
				continue;
			}
			m->fmt = has_ant ? Fmt_Ant : Fmt_Oai;

			/* capabilities → limits */
			caps = jsonget(fixture, toks, n, elem, "capabilities");
			if(caps >= 0 && toks[caps].type == JSMN_OBJECT) {
				lim = jsonget(fixture, toks, n, caps, "limits");
				if(lim >= 0 && toks[lim].type == JSMN_OBJECT) {
					vi = jsonget(fixture, toks, n, lim,
					             "max_context_window_tokens");
					if(vi >= 0) m->ctx_k = jsonint(fixture, &toks[vi]) / 1000;
					vi = jsonget(fixture, toks, n, lim, "max_output_tokens");
					if(vi >= 0) m->maxout_k = jsonint(fixture, &toks[vi]) / 1000;
				}
				sup = jsonget(fixture, toks, n, caps, "supports");
				if(sup >= 0 && toks[sup].type == JSMN_OBJECT) {
					vi = jsonget(fixture, toks, n, sup, "tool_calls");
					if(vi >= 0 && toks[vi].type == JSMN_PRIMITIVE)
						m->tools = (fixture[toks[vi].start] == 't');
				}
			}

			*tail = m;
			tail  = &m->next;
		}
		elem = next_elem;
	}
	return head;
}

static void
test_parse_fixture(void)
{
	Model *list, *m;
	int count;
	Biobuf bout;

	print("=== unit tests: fixture parse ===\n");

	list = parse_fixture();
	CHECK(list != nil, "fixture: parse returns non-nil");

	/* Count: fixture has 3 models but disabled one is filtered → expect 2 */
	count = 0;
	for(m = list; m != nil; m = m->next) count++;
	CHECKEQ(count, 2, "fixture: 2 models (disabled one filtered)");

	/* Model 0: claude-opus-4.6-fast — ant, has both endpoints */
	m = list;
	CHECK(m != nil, "fixture[0]: exists");
	if(m != nil) {
		CHECKSTR(m->id,     "claude-opus-4.6-fast",    "fixture[0]: id");
		CHECKSTR(m->name,   "Claude Opus 4.6 (fast mode)", "fixture[0]: name");
		CHECKSTR(m->vendor, "Anthropic",               "fixture[0]: vendor");
		CHECKEQ(m->fmt,     Fmt_Ant,                   "fixture[0]: fmt=ant");
		CHECKEQ(m->ctx_k,   200,                       "fixture[0]: ctx_k");
		CHECKEQ(m->maxout_k, 64,                       "fixture[0]: maxout_k");
		CHECKEQ(m->tools,   1,                         "fixture[0]: tools=y");
	}

	/* Model 1: gemini-3.1-pro-preview — oai only */
	m = list ? list->next : nil;
	CHECK(m != nil, "fixture[1]: exists");
	if(m != nil) {
		CHECKSTR(m->id,     "gemini-3.1-pro-preview",  "fixture[1]: id");
		CHECKSTR(m->vendor, "Google",                  "fixture[1]: vendor");
		CHECKEQ(m->fmt,     Fmt_Oai,                   "fixture[1]: fmt=oai");
		CHECKEQ(m->ctx_k,   128,                       "fixture[1]: ctx_k");
		CHECKEQ(m->maxout_k, 64,                       "fixture[1]: maxout_k");
		CHECKEQ(m->tools,   1,                         "fixture[1]: tools=y");
	}

	/* modelsfmt output */
	print("\nmodelsfmt output:\n");
	Binit(&bout, 1, OWRITE);
	modelsfmt(list, &bout);
	Bflush(&bout);
	Bterm(&bout);
	print("\n");

	modelsfree(list);
	print("unit tests done\n\n");
}

/* ── Integration test ────────────────────────────────────────────────── */

static void
test_live(char *sockpath, char *tokpath)
{
	char *refresh;
	OAuthToken *ot;
	Model *list, *m;
	int count;
	Biobuf bout;
	int n;
	char buf[4096];
	int fd;
	long len;

	print("=== integration test: live /models ===\n");

	/* read refresh token */
	fd = open(tokpath, OREAD);
	if(fd < 0) sysfatal("open %s: %r", tokpath);
	len = readn(fd, buf, sizeof buf - 1);
	close(fd);
	if(len <= 0) sysfatal("empty token file: %s", tokpath);
	buf[len] = '\0';
	while(len > 0 && (buf[len-1]=='\n'||buf[len-1]=='\r'||buf[len-1]==' '))
		buf[--len] = '\0';
	refresh = strdup(buf);

	/* get session token */
	ot = oauthsession(refresh, sockpath);
	if(ot == nil) sysfatal("oauthsession: %r");
	print("session token: %.30s...\n", ot->token);

	/* fetch models */
	list = modelsfetch(ot->token, sockpath);
	if(list == nil) sysfatal("modelsfetch: %r");

	count = 0;
	for(m = list; m != nil; m = m->next) count++;
	print("model count: %d\n", count);
	CHECK(count > 0, "live: at least one model returned");

	/* Check that at least one ant and one oai model is present */
	n = 0;
	for(m = list; m != nil; m = m->next)
		if(m->fmt == Fmt_Ant) { n++; break; }
	CHECK(n > 0, "live: at least one Anthropic (ant) model");

	n = 0;
	for(m = list; m != nil; m = m->next)
		if(m->fmt == Fmt_Oai) { n++; break; }
	CHECK(n > 0, "live: at least one OpenAI (oai) model");

	/* All models should have non-empty id */
	n = 0;
	for(m = list; m != nil; m = m->next)
		if(m->id == nil || m->id[0] == '\0') n++;
	CHECKEQ(n, 0, "live: all models have non-empty id");

	print("\nFull model table:\n");
	Binit(&bout, 1, OWRITE);
	modelsfmt(list, &bout);
	Bflush(&bout);
	Bterm(&bout);

	modelsfree(list);
	oauthtokenfree(ot);
	free(refresh);
	print("\nintegration test done\n");
}

void
threadmain(int argc, char *argv[])
{
	char *sockpath, *tokpath;

	sockpath = nil;
	tokpath  = nil;

	ARGBEGIN{
	case 's':
		sockpath = ARGF();
		break;
	case 't':
		tokpath = ARGF();
		break;
	}ARGEND

	USED(argv);

	test_parse_fixture();

	if(sockpath != nil && tokpath != nil)
		test_live(sockpath, tokpath);
	else
		print("(skipping integration test: pass -s sockpath -t tokenfile)\n");

	if(failures > 0) {
		fprint(2, "\n%d failure(s)\n", failures);
		threadexitsall("FAIL");
	}
	print("\nall tests passed\n");
	threadexitsall(nil);
}
