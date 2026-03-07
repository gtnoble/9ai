/*
 * models.h — /models fetch and parse for 9ai
 *
 * Fetches GET /models from the Copilot API and parses the response into
 * an array of Model structs representing the models available to the
 * authenticated user.
 *
 * Filtering:
 *   - model_picker_enabled must be true (or filterall=1 to skip filter)
 *   - supported_endpoints must include /chat/completions or /v1/messages
 *
 * Wire format:
 *   Fmt_Ant ("ant") — model supports /v1/messages (Anthropic Messages)
 *   Fmt_Oai ("oai") — model supports /chat/completions only (OpenAI Completions)
 *   When both are present, Fmt_Ant is preferred.
 *
 * /models file output (one line per model, tab-separated):
 *   <id>\t<fmt>\t<ctx_k>\t<maxout_k>\t<tools>\t<name>
 *
 * Example:
 *   gpt-4o	oai	128	4	y	GPT-4o
 *   claude-sonnet-4.5	ant	200	32	y	Claude Sonnet 4.5
 */

enum {
	Fmt_Oai = 0,  /* OpenAI Completions (/chat/completions) */
	Fmt_Ant = 1,  /* Anthropic Messages (/v1/messages)      */
};

typedef struct Model Model;
struct Model {
	char *id;
	char *name;
	char *vendor;
	int   fmt;         /* Fmt_Oai or Fmt_Ant */
	long  ctx_k;       /* context window, thousands of tokens */
	long  maxout_k;    /* max output tokens, thousands        */
	int   tools;       /* supports tool calls                 */
	Model *next;
};

/*
 * modelsfetch — GET /models from the Copilot API.
 *
 * session  — Copilot session token (tid=...) used as Bearer
 * sockpath — path to 9aitls Unix socket
 *
 * Returns a linked list of Model structs (heap-allocated).
 * Only models with model_picker_enabled=true are included.
 * Returns nil on error (sets errstr).
 */
Model *modelsfetch(char *session, char *sockpath);

/*
 * modelsfmt — format the model list as a /models file body.
 *
 * Writes one tab-separated line per model into b:
 *   <id>\t<fmt>\t<ctx_k>k\t<maxout_k>k\t<tools>\t<name>\n
 *
 * b must be open for writing.
 */
void modelsfmt(Model *list, Biobuf *b);

/*
 * modelsfree — free a Model list returned by modelsfetch.
 */
void modelsfree(Model *list);
