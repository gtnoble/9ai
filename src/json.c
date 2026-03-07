/*
 * json.c — JSON parse and emit helpers for 9ai
 *
 * See json.h for the full API and usage examples.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>

/*
 * Pull in jsmn implementation.  JSMN_HEADER is defined in json.h (which
 * the caller includes) to suppress the inline implementation there; we
 * undefine it here so this .c file gets the full implementation.
 */
#undef JSMN_HEADER
#include "vendor/jsmn/jsmn.h"

#include "json.h"

/*
 * jsoneq — compare token t in string js to C string s.
 */
int
jsoneq(const char *js, jsmntok_t *t, const char *s)
{
	int len;

	len = t->end - t->start;
	if(t->type != JSMN_STRING)
		return 0;
	if((int)strlen(s) != len)
		return 0;
	return memcmp(js + t->start, s, len) == 0;
}

/*
 * jsonstr — copy and unescape the string token t into buf[0..n).
 * Returns bytes written (excluding NUL), or -1 if buf too small.
 */
int
jsonstr(const char *js, jsmntok_t *t, char *buf, int n)
{
	const char *src, *end;
	char *dst, *dend;
	char c;
	int  u;

	if(t->type != JSMN_STRING)
		return -1;

	src  = js + t->start;
	end  = js + t->end;
	dst  = buf;
	dend = buf + n - 1;  /* leave room for NUL */

	while(src < end) {
		if(dst >= dend)
			return -1;
		c = *src++;
		if(c != '\\') {
			*dst++ = c;
			continue;
		}
		/* escape sequence */
		if(src >= end)
			break;
		c = *src++;
		switch(c) {
		case '"':  *dst++ = '"';  break;
		case '\\': *dst++ = '\\'; break;
		case '/':  *dst++ = '/';  break;
		case 'n':  *dst++ = '\n'; break;
		case 'r':  *dst++ = '\r'; break;
		case 't':  *dst++ = '\t'; break;
		case 'b':  *dst++ = '\b'; break;
		case 'f':  *dst++ = '\f'; break;
		case 'u':
			/* \uXXXX — BMP only, emit as UTF-8 */
			if(src + 4 > end)
				break;
			u = 0;
			{
				int i;
				for(i = 0; i < 4; i++) {
					char h = src[i];
					u <<= 4;
					if(h >= '0' && h <= '9')      u |= h - '0';
					else if(h >= 'a' && h <= 'f') u |= h - 'a' + 10;
					else if(h >= 'A' && h <= 'F') u |= h - 'A' + 10;
				}
			}
			src += 4;
			/* encode as UTF-8 */
			if(u < 0x80) {
				if(dst >= dend) return -1;
				*dst++ = u;
			} else if(u < 0x800) {
				if(dst + 1 >= dend) return -1;
				*dst++ = 0xC0 | (u >> 6);
				*dst++ = 0x80 | (u & 0x3F);
			} else {
				if(dst + 2 >= dend) return -1;
				*dst++ = 0xE0 | (u >> 12);
				*dst++ = 0x80 | ((u >> 6) & 0x3F);
				*dst++ = 0x80 | (u & 0x3F);
			}
			break;
		default:
			/* unknown escape: pass through literally */
			if(dst >= dend) return -1;
			*dst++ = c;
			break;
		}
	}
	*dst = '\0';
	return dst - buf;
}

/*
 * jsonint — parse primitive token t as a decimal integer.
 */
long
jsonint(const char *js, jsmntok_t *t)
{
	char buf[32];
	int  len;

	len = t->end - t->start;
	if(len <= 0 || len >= (int)sizeof buf)
		return 0;
	memcpy(buf, js + t->start, len);
	buf[len] = '\0';
	return atol(buf);
}

/*
 * jsonnext — skip the subtree at toks[tok], return next sibling index.
 *
 * For a scalar (string or primitive), that's tok+1.
 * For an object or array, we skip tok+1 through all descendants.
 * We count descendants by accumulating the size fields: each object or
 * array token's .size gives the number of direct children (for objects,
 * that's key+value pairs * 2; for arrays, it's element count).  We walk
 * until the pending count reaches zero.
 */
int
jsonnext(jsmntok_t *toks, int ntoks, int tok)
{
	int pending;

	if(tok >= ntoks)
		return ntoks;

	pending = 1;
	while(pending > 0 && tok < ntoks) {
		if(toks[tok].type == JSMN_OBJECT)
			pending += toks[tok].size * 2;
		else if(toks[tok].type == JSMN_ARRAY)
			pending += toks[tok].size;
		tok++;
		pending--;
	}
	return tok;
}

/*
 * jsonget — find key s in the object at toks[obj], return value index.
 *
 * Walks key-value pairs at the direct-child level of the object.
 * Returns -1 if the key is not found or obj is not an object.
 */
int
jsonget(const char *js, jsmntok_t *toks, int ntoks, int obj, const char *key)
{
	int i, vi, n;

	if(obj >= ntoks || toks[obj].type != JSMN_OBJECT)
		return -1;

	n  = toks[obj].size;  /* number of key-value pairs */
	i  = obj + 1;
	while(n-- > 0 && i < ntoks) {
		vi = i + 1;
		if(jsoneq(js, &toks[i], key))
			return vi;
		/* skip the value subtree to reach the next key */
		i = jsonnext(toks, ntoks, vi);
	}
	return -1;
}

/*
 * jsonemitstr — write s as a JSON string literal to b with escaping.
 */
void
jsonemitstr(Biobuf *b, const char *s)
{
	jsonemitstrn(b, s, strlen(s));
}

/*
 * jsonemitstrn — write s[0..n) as a JSON string literal to b.
 */
void
jsonemitstrn(Biobuf *b, const char *s, int n)
{
	int i;
	unsigned char c;

	Bputc(b, '"');
	for(i = 0; i < n; i++) {
		c = (unsigned char)s[i];
		switch(c) {
		case '"':  Bprint(b, "\\\""); break;
		case '\\': Bprint(b, "\\\\"); break;
		case '\n': Bprint(b, "\\n");  break;
		case '\r': Bprint(b, "\\r");  break;
		case '\t': Bprint(b, "\\t");  break;
		case '\b': Bprint(b, "\\b");  break;
		case '\f': Bprint(b, "\\f");  break;
		default:
			if(c < 0x20)
				Bprint(b, "\\u%04x", c);
			else
				Bputc(b, c);
			break;
		}
	}
	Bputc(b, '"');
}
