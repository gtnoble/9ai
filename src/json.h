/*
 * json.h — JSON parse and emit helpers for 9ai
 *
 * Parse side: thin layer over jsmn.  jsmn tokenises a JSON string into
 * an array of jsmntok_t descriptors (type, start, end, size) without
 * copying or allocating.  All helpers here work against the original
 * string and the token array.
 *
 * Emit side: write JSON directly to a Biobuf using jsonemit_* helpers.
 * No intermediate representation; caller controls structure.
 *
 * ── Parsing ────────────────────────────────────────────────────────────
 *
 * Typical usage:
 *
 *   enum { MAXTOK = 512 };
 *   jsmn_parser p;
 *   jsmntok_t   toks[MAXTOK];
 *   int ntoks;
 *
 *   jsmn_init(&p);
 *   ntoks = jsmn_parse(&p, js, strlen(js), toks, MAXTOK);
 *   if(ntoks < 0) { ... error ... }
 *
 *   // find "id" in a top-level object (toks[0] must be JSMN_OBJECT)
 *   int vi = jsonget(js, toks, ntoks, 0, "id");
 *   if(vi >= 0) {
 *       char buf[256];
 *       jsonstr(js, &toks[vi], buf, sizeof buf);
 *   }
 *
 *   // walk "data" array
 *   int ai = jsonget(js, toks, ntoks, 0, "data");
 *   if(ai >= 0 && toks[ai].type == JSMN_ARRAY) {
 *       int elem = ai + 1;
 *       for(int i = 0; i < toks[ai].size; i++) {
 *           // process toks[elem] ...
 *           elem = jsonnext(toks, ntoks, elem);
 *       }
 *   }
 *
 * ── Emitting ───────────────────────────────────────────────────────────
 *
 * Typical usage:
 *
 *   Biobuf b;
 *   Binit(&b, fd, OWRITE);
 *   Bprint(&b, "{");
 *   Bprint(&b, "\"model\":");  jsonemitstr(&b, model);
 *   Bprint(&b, ",\"stream\":true");
 *   Bprint(&b, ",\"messages\":[");
 *   // ... emit messages ...
 *   Bprint(&b, "]}");
 *   Bflush(&b);
 *   Bterm(&b);
 */

#define JSMN_HEADER   /* use jsmn as header-only; implementation in json.c */
#include "vendor/jsmn/jsmn.h"

/*
 * ── Parse helpers ───────────────────────────────────────────────────────
 */

/*
 * jsoneq — return 1 if token t in string js equals the C string s.
 * Standard jsmn comparison idiom.
 */
int   jsoneq(const char *js, jsmntok_t *t, const char *s);

/*
 * jsonstr — copy and JSON-unescape the string token t from js into buf.
 * buf is nil-terminated.  Returns the number of bytes written (< n),
 * or -1 if buf is too small or t is not a string token.
 * Only handles \", \\, \/, \n, \r, \t, \b, \f and \uXXXX (BMP only).
 */
int   jsonstr(const char *js, jsmntok_t *t, char *buf, int n);

/*
 * jsonint — parse primitive token t as a decimal integer.
 * Returns the value, or 0 on error (use jsoneq to distinguish "0").
 */
long  jsonint(const char *js, jsmntok_t *t);

/*
 * jsonget — find key s in the object at token index obj in toks[0..ntoks).
 * Returns the token index of the value, or -1 if not found.
 * obj must be a JSMN_OBJECT token; only searches the direct children
 * (does not recurse into nested objects).
 */
int   jsonget(const char *js, jsmntok_t *toks, int ntoks, int obj, const char *key);

/*
 * jsonnext — return the token index of the next sibling after toks[tok].
 * Skips the entire subtree rooted at tok (object, array, or scalar).
 * Returns ntoks if tok is the last token.
 */
int   jsonnext(jsmntok_t *toks, int ntoks, int tok);

/*
 * ── Emit helpers ────────────────────────────────────────────────────────
 */

/*
 * jsonemitstr — write s as a JSON string literal to b, with proper escaping.
 * Writes the surrounding double quotes.
 */
void  jsonemitstr(Biobuf *b, const char *s);

/*
 * jsonemitstrn — like jsonemitstr but for a string of known length n
 * (may contain embedded NULs, though JSON strings normally don't).
 */
void  jsonemitstrn(Biobuf *b, const char *s, int n);
