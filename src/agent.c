/*
 * agent.c — agent loop, conversation history, and session I/O
 *
 * See agent.h for the full interface.
 *
 * ── Token lifecycle ───────────────────────────────────────────────────
 *
 * agentrun() loads the GitHub refresh token from cfg->tokpath, then
 * calls oauthsession() to obtain a live Copilot session token.  The
 * same session token is reused across tool-loop iterations within one
 * call to agentrun(); it is refreshed if expires_at is within 300s.
 *
 * ── Tool loop ─────────────────────────────────────────────────────────
 *
 * One call to agentrun() may make multiple HTTP requests:
 *
 *   POST /chat/completions   (with user message)
 *     → stream deltas until OAIDStop
 *     if stop_reason == "tool_calls":
 *       exec the tool
 *       POST /chat/completions again (with tool result appended)
 *       → stream deltas until OAIDStop
 *     repeat until stop_reason == "stop" or error
 *
 * Orphaned tool call invariant: if the last assistant message has a
 * tool call with no following result (aborted turn), we insert a
 * synthetic error result before the next user message.  Not needed in
 * phase 9 (no abort), but the OAI serialiser already handles this via
 * the message structure.
 *
 * ── Session file ──────────────────────────────────────────────────────
 *
 * Created by agentsessopen(): ~/lib/9ai/sessions/<uuid>
 * Records are appended in chronological order.
 * Flushed at turn_end (and after each tool_end for safety).
 *
 * ── emit / writesession varargs ───────────────────────────────────────
 *
 * emitevent(), writesession(), emitandsave() all take a nil-terminated
 * list of char* fields after cfg.  They format:
 *   field₀ FS field₁ … FS fieldₙ RS
 * into a stack buffer, then deliver it.
 *
 * We use a fixed 64KB buffer which is sufficient for any tool output
 * in a single record (the exec output in tool_end can be large, but
 * the session file gets the full output and the record format can hold it).
 * If the buffer overflows we silently truncate — the alternative would
 * be to heap-allocate, which complicates the code substantially.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>
#include <stdarg.h>

#include "9ai.h"
#include "http.h"
#include "json.h"
#include "oauth.h"
#include "sse.h"
#include "oai.h"
#include "ant.h"
#include "exec.h"
#include "agent.h"

/* ── genuuid ──────────────────────────────────────────────────────────── */

void
genuuid(char *buf)
{
	ulong r[4];
	uchar b[16];
	int i;

	/* 16 random bytes from 4 × truerand() */
	for(i = 0; i < 4; i++)
		r[i] = truerand();
	for(i = 0; i < 16; i++)
		b[i] = (r[i/4] >> (8 * (i % 4))) & 0xff;

	/* set version 4 and variant bits */
	b[6] = (b[6] & 0x0f) | 0x40;   /* version 4 */
	b[8] = (b[8] & 0x3f) | 0x80;   /* variant 10xx */

	snprint(buf, 37,
	        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
	        "%02x%02x%02x%02x%02x%02x",
	        b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7],
	        b[8],b[9], b[10],b[11],b[12],b[13],b[14],b[15]);
}

/* ── Session management ───────────────────────────────────────────────── */

/*
 * mkdirpath — ensure all parent directories of path exist.
 * Creates each missing component with mode 0700.
 * Stops at the last '/' — does not create the leaf itself.
 */
static int
mkdirpath(char *path)
{
	char *p, *q, buf[512];
	int n;

	if(strlen(path) >= sizeof buf) {
		werrstr("mkdirpath: path too long");
		return -1;
	}
	n = snprint(buf, sizeof buf, "%s", path);

	for(p = buf + 1; *p != '\0'; p++) {
		if(*p != '/')
			continue;
		*p = '\0';
		q = buf;
		/* try to create; ignore EEXIST */
		if(access(q, AEXIST) < 0) {
			int fd = create(q, OREAD, DMDIR | 0700);
			if(fd < 0) {
				/* may already exist (race) — re-check */
				if(access(q, AEXIST) < 0) {
					werrstr("mkdirpath: create %s: %r", q);
					return -1;
				}
			} else {
				close(fd);
			}
		}
		*p = '/';
	}
	return 0;
}

int
agentsessopen(AgentCfg *cfg)
{
	char *sessdir, *path;
	long  now;
	int   fd;

	genuuid(cfg->uuid);

	if(cfg->sessdir != nil) {
		sessdir = strdup(cfg->sessdir);
	} else {
		sessdir = configpath("sessions/");
	}
	if(sessdir == nil) {
		werrstr("agentsessopen: configpath: %r");
		return -1;
	}

	/* ensure ~/lib/9ai/sessions/ exists */
	{
		char tmp[512];
		snprint(tmp, sizeof tmp, "%ssessions_ensure", sessdir);
		/* mkdirpath needs a file path; add a dummy leaf */
		snprint(tmp, sizeof tmp, "%sdummy", sessdir);
		if(mkdirpath(tmp) < 0) {
			free(sessdir);
			return -1;
		}
		/* also try to create sessdir itself */
		if(access(sessdir, AEXIST) < 0) {
			fd = create(sessdir, OREAD, DMDIR | 0700);
			if(fd < 0 && access(sessdir, AEXIST) < 0) {
				werrstr("agentsessopen: mkdir %s: %r", sessdir);
				free(sessdir);
				return -1;
			}
			if(fd >= 0) close(fd);
		}
	}

	path = smprint("%s%s", sessdir, cfg->uuid);
	free(sessdir);
	if(path == nil) {
		werrstr("agentsessopen: smprint: %r");
		return -1;
	}

	fd = create(path, OWRITE, 0600);
	if(fd < 0) {
		werrstr("agentsessopen: create %s: %r", path);
		free(path);
		return -1;
	}
	free(path);

	cfg->sess_bio = mallocz(sizeof *cfg->sess_bio, 1);
	if(cfg->sess_bio == nil) {
		close(fd);
		werrstr("agentsessopen: mallocz: %r");
		return -1;
	}
	Binit(cfg->sess_bio, fd, OWRITE);

	/* write session header record */
	now = time(0);
	Bprint(cfg->sess_bio, "session" AIFS "%s" AIFS "%s" AIFS "%ld" AIRS,
	       cfg->uuid, cfg->model, now);

	return 0;
}

void
agentsessclose(AgentCfg *cfg)
{
	int fd;

	if(cfg->sess_bio == nil)
		return;
	Bflush(cfg->sess_bio);
	fd = cfg->sess_bio->fid;
	Bterm(cfg->sess_bio);
	close(fd);
	free(cfg->sess_bio);
	cfg->sess_bio = nil;
}

/* ── agentsessload — session file replay ─────────────────────────────── */

/*
 * splitrec — split a RS-terminated record into fields separated by FS.
 *
 * rec:    the record bytes (may contain embedded NULs in theory, but
 *         our fields are text so we treat them as C strings)
 * reclen: length in bytes including the trailing RS (0x1E)
 * fields: output array of malloc'd strings; caller must free each
 * maxfields: capacity of fields[]
 *
 * Returns the number of fields extracted (>= 1 if record is non-empty).
 * The trailing RS is not included in any field.
 *
 * Handles doubled FS (0x1F 0x1F → literal 0x1F) and RS (0x1E 0x1E →
 * literal 0x1E) per the design doc, though these are vanishingly rare.
 */
static int
splitrec(char *rec, long reclen, char **fields, int maxfields)
{
	char *p, *end, *fbuf;
	int   nf;
	long  flen;

	/* strip trailing RS if present */
	end = rec + reclen;
	if(end > rec && (uchar)*(end-1) == 0x1e)
		end--;

	nf  = 0;
	p   = rec;

	while(p < end && nf < maxfields) {
		/* find next FS or end-of-record */
		char *fs = p;
		long  cap = 256;
		long  wlen = 0;

		fbuf = malloc(cap);
		if(fbuf == nil)
			break;

		while(fs < end) {
			uchar c = (uchar)*fs;
			if(c == 0x1f) {
				/* doubled FS → literal 0x1F; single FS → field separator */
				if(fs+1 < end && (uchar)*(fs+1) == 0x1f) {
					/* literal FS */
					if(wlen + 1 >= cap) {
						cap *= 2;
						char *tmp = realloc(fbuf, cap);
						if(tmp == nil) { free(fbuf); fbuf = nil; break; }
						fbuf = tmp;
					}
					fbuf[wlen++] = 0x1f;
					fs += 2;
				} else {
					/* field separator */
					fs++;
					break;
				}
			} else if(c == 0x1e) {
				/* doubled RS → literal 0x1E */
				if(fs+1 < end && (uchar)*(fs+1) == 0x1e) {
					if(wlen + 1 >= cap) {
						cap *= 2;
						char *tmp = realloc(fbuf, cap);
						if(tmp == nil) { free(fbuf); fbuf = nil; break; }
						fbuf = tmp;
					}
					fbuf[wlen++] = 0x1e;
					fs += 2;
				} else {
					/* should not happen after stripping trailing RS */
					fs++;
					break;
				}
			} else {
				if(wlen + 1 >= cap) {
					cap *= 2;
					char *tmp = realloc(fbuf, cap);
					if(tmp == nil) { free(fbuf); fbuf = nil; break; }
					fbuf = tmp;
				}
				fbuf[wlen++] = c;
				fs++;
			}
		}
		if(fbuf == nil)
			break;
		fbuf[wlen] = '\0';
		fields[nf++] = fbuf;
		p = fs;
		USED(flen);
	}
	return nf;
}

/*
 * freesplit — free an array of fields returned by splitrec.
 */
static void
freesplit(char **fields, int nf)
{
	int i;
	for(i = 0; i < nf; i++)
		free(fields[i]);
}

/*
 * argv2json — convert an argv array into a JSON object string:
 *   {"argv":["arg0","arg1",...],"stdin":""}
 *
 * Returns a malloc'd string.  Returns nil on error.
 */
static char *
argv2json(char **argv, int argc)
{
	char *buf;
	long  cap = 1024, len = 0;
	int   i;

	buf = malloc(cap);
	if(buf == nil)
		return nil;

	len += snprint(buf + len, cap - len, "{\"argv\":[");
	for(i = 0; i < argc; i++) {
		if(i > 0) {
			if(len + 2 < cap) { buf[len++] = ','; buf[len] = '\0'; }
		}
		/* JSON-encode argv[i]: escape backslash and double-quote */
		char *s = argv[i];
		if(len + 2 < cap) { buf[len++] = '"'; buf[len] = '\0'; }
		while(*s) {
			if(len + 4 >= cap) {
				cap *= 2;
				char *tmp = realloc(buf, cap);
				if(tmp == nil) { free(buf); return nil; }
				buf = tmp;
			}
			if(*s == '"' || *s == '\\') buf[len++] = '\\';
			buf[len++] = *s++;
		}
		if(len + 2 < cap) { buf[len++] = '"'; buf[len] = '\0'; }
	}
	if(len + 32 < cap || (cap = len + 32, buf = realloc(buf, cap), buf != nil))
		len += snprint(buf + len, cap - len, "],\"stdin\":\"\"}");
	buf[len] = '\0';
	return buf;
}

int
agentsessload(char *path, OAIReq *req, AgentCfg *cfg)
{
	int     fd;
	Biobuf  bio;
	char   *line;
	long    linelen;

	/* State machine for reconstructing turns */
	enum { ST_IDLE, ST_TURN, ST_TOOL };
	int   state = ST_IDLE;

	/* text accumulator for assistant response */
	char *textbuf = nil;
	long  textcap = 0, textlen = 0;

	/* tool call accumulator */
	char *tc_name = nil;
	char *tc_id   = nil;
	char **tc_argv = nil;
	int   tc_argc  = 0;
	int   tc_argv_cap = 0;

	/* tool result accumulator */
	char *tr_output  = nil;
	int   tr_is_error = 0;

	/* pending tool call (stored until we see tool_end) */
	/* We need: assistant message with text+toolcall, then tool result */

	fd = open(path, ORDWR);  /* ORDWR so we can later append */
	if(fd < 0) {
		werrstr("agentsessload: open %s: %r", path);
		return -1;
	}
	Binit(&bio, fd, OREAD);

	/*
	 * Read records one at a time.  Records are terminated by RS (0x1E).
	 * Brdline reads up to and including the delimiter.
	 */
	while((line = Brdline(&bio, 0x1e)) != nil) {
		char  *fields[64];
		int    nf;

		linelen = Blinelen(&bio);
		nf = splitrec(line, linelen, fields, 64);
		if(nf == 0)
			goto nextrecord;

		/* dispatch on record type (field 0) */
		if(strcmp(fields[0], "session") == 0) {
			/* session FS uuid FS model FS timestamp */
			if(nf >= 2)
				memmove(cfg->uuid, fields[1],
				        strlen(fields[1]) < 36 ? strlen(fields[1])+1 : 37);
			/* optionally update model */
			if(nf >= 3 && cfg->model != nil) {
				free(cfg->model);
				cfg->model = strdup(fields[2]);
			}

		} else if(strcmp(fields[0], "prompt") == 0) {
			/* prompt FS text → user message */
			if(nf >= 2)
				oaireqaddmsg(req, oaimsguser(fields[1]));

		} else if(strcmp(fields[0], "turn_start") == 0) {
			/* begin accumulating assistant text */
			state = ST_TURN;
			free(textbuf);
			textbuf = nil; textcap = 0; textlen = 0;

		} else if(strcmp(fields[0], "text") == 0) {
			/* text FS chunk — accumulate */
			if(nf >= 2 && state == ST_TURN) {
				long dlen = strlen(fields[1]);
				if(textbuf == nil || textlen + dlen >= textcap) {
					long newcap = (textcap == 0 ? 4096 : textcap * 2) + dlen + 1;
					char *tmp = realloc(textbuf, newcap);
					if(tmp != nil) {
						textbuf = tmp;
						textcap = newcap;
					}
				}
				if(textbuf != nil && textlen + dlen < textcap) {
					memmove(textbuf + textlen, fields[1], dlen);
					textlen += dlen;
					textbuf[textlen] = '\0';
				}
			}

		} else if(strcmp(fields[0], "tool_start") == 0) {
			/*
			 * tool_start FS name FS id FS argv[0] FS argv[1] ...
			 *
			 * Before recording the tool call we emit the assistant text
			 * message accumulated so far (it precedes the tool call in the
			 * same assistant turn).
			 */
			int i;

			/* free any previous tool accumulator */
			free(tc_name); tc_name = nil;
			free(tc_id);   tc_id   = nil;
			if(tc_argv != nil) {
				for(i = 0; i < tc_argc; i++) free(tc_argv[i]);
				free(tc_argv);
				tc_argv = nil;
			}
			tc_argc = 0; tc_argv_cap = 0;
			free(tr_output); tr_output = nil;
			tr_is_error = 0;

			if(nf >= 2) tc_name = strdup(fields[1]);
			if(nf >= 3) tc_id   = strdup(fields[2]);

			/* collect argv fields (indices 3..nf-1) */
			tc_argc = nf - 3;
			if(tc_argc < 0) tc_argc = 0;
			if(tc_argc > 0) {
				tc_argv = malloc(tc_argc * sizeof(char*));
				if(tc_argv != nil) {
					for(i = 0; i < tc_argc; i++)
						tc_argv[i] = strdup(fields[3 + i]);
					tc_argv_cap = tc_argc;
				} else {
					tc_argc = 0;
				}
			}
			state = ST_TOOL;

		} else if(strcmp(fields[0], "tool_end") == 0) {
			/*
			 * tool_end FS ok|err FS output
			 *
			 * Now we have everything: emit the assistant message with
			 * text (if any) + tool call, then the tool result.
			 */
			char *tool_args_json;
			int   i;

			if(nf >= 2) tr_is_error = (strcmp(fields[1], "err") == 0);
			if(nf >= 3) { free(tr_output); tr_output = strdup(fields[2]); }

			/* build JSON args from argv */
			tool_args_json = argv2json(
			    tc_argv != nil ? tc_argv : (char*[]){NULL, NULL},
			    tc_argc);

			oaireqaddmsg(req, oaimsgtoolcall(
			    textbuf != nil && textlen > 0 ? textbuf : nil,
			    tc_id   != nil ? tc_id   : "call_unknown",
			    tc_name != nil ? tc_name : "exec",
			    tool_args_json != nil ? tool_args_json : "{}"));
			free(tool_args_json);

			oaireqaddmsg(req, oaimsgtoolresult(
			    tc_id     != nil ? tc_id     : "call_unknown",
			    tr_output != nil ? tr_output : "",
			    tr_is_error));

			/* reset text accumulator for next tool or turn end */
			free(textbuf); textbuf = nil; textcap = 0; textlen = 0;

			free(tc_name); tc_name = nil;
			free(tc_id);   tc_id   = nil;
			free(tr_output); tr_output = nil;
			if(tc_argv != nil) {
				for(i = 0; i < tc_argc; i++) free(tc_argv[i]);
				free(tc_argv); tc_argv = nil;
			}
			tc_argc = 0;
			state = ST_TURN;  /* more text or turn_end may follow */

		} else if(strcmp(fields[0], "turn_end") == 0) {
			/*
			 * turn_end — finalize the assistant response.
			 * If we have accumulated text and no pending tool call,
			 * add an assistant message.
			 */
			if(state == ST_TURN && textbuf != nil && textlen > 0)
				oaireqaddmsg(req, oaimsgassistant(textbuf));

			free(textbuf); textbuf = nil; textcap = 0; textlen = 0;
			state = ST_IDLE;

		} else if(strcmp(fields[0], "model") == 0) {
			if(nf >= 2 && cfg->model != nil) {
				free(cfg->model);
				cfg->model = strdup(fields[1]);
			}
		}
		/* steer, thinking: ignored for API history reconstruction */

nextrecord:
		freesplit(fields, nf);
	}

	Bterm(&bio);

	/* clean up any partial state */
	{
		int i;
		free(textbuf);
		free(tc_name);
		free(tc_id);
		free(tr_output);
		if(tc_argv != nil) {
			for(i = 0; i < tc_argc; i++) free(tc_argv[i]);
			free(tc_argv);
		}
	}

	/*
	 * Re-open for append so subsequent agentrun() calls write new records
	 * to the same session file.
	 */
	fd = open(path, OWRITE);
	if(fd < 0) {
		werrstr("agentsessload: reopen for append %s: %r", path);
		return -1;
	}
	/* seek to end */
	seek(fd, 0, 2);

	cfg->sess_bio = mallocz(sizeof *cfg->sess_bio, 1);
	if(cfg->sess_bio == nil) {
		close(fd);
		werrstr("agentsessload: mallocz: %r");
		return -1;
	}
	Binit(cfg->sess_bio, fd, OWRITE);

	return 0;
}

/* ── Record formatting ────────────────────────────────────────────────── */

/*
 * fmtrec — format a nil-terminated vararg field list into buf.
 *
 * Returns the number of bytes written (not counting NUL).
 * Truncates silently if buf is too small (n bytes available).
 *
 * Format: field₀ FS field₁ FS … FS fieldₙ RS
 */
static long
fmtrec(char *buf, int n, va_list ap)
{
	char  *field;
	int    first = 1;
	long   total = 0;
	long   flen, rem;

	while((field = va_arg(ap, char *)) != nil) {
		if(!first) {
			if(total < n - 1) buf[total] = '\x1f';
			total++;
		}
		first = 0;
		flen = strlen(field);
		rem  = n - 1 - total;
		if(rem < 0) rem = 0;
		if(flen > rem) flen = rem;
		if(flen > 0) memmove(buf + total, field, flen);
		total += strlen(field);  /* track true total even if truncated */
	}
	/* RS terminator */
	if(total < n - 1) buf[total < 0 ? 0 : total] = '\x1e';
	total++;

	/* NUL-terminate within buf */
	{
		long cap = total < n ? total : n - 1;
		if(cap >= 0) buf[cap] = '\0';
	}
	return total;
}

void
emitevent(AgentCfg *cfg, ...)
{
	char   buf[65536];
	long   len;
	va_list ap;

	if(cfg->onevent == nil)
		return;

	va_start(ap, cfg);
	len = fmtrec(buf, sizeof buf, ap);
	va_end(ap);

	cfg->onevent(buf, len < (long)sizeof buf ? len : (long)sizeof buf - 1, cfg->aux);
}

void
writesession(AgentCfg *cfg, ...)
{
	char   buf[65536];
	long   len;
	va_list ap;

	if(cfg->sess_bio == nil)
		return;

	va_start(ap, cfg);
	len = fmtrec(buf, sizeof buf, ap);
	va_end(ap);

	/* write the capped buf; true len may exceed buf if truncated */
	{
		long wlen = len < (long)sizeof buf ? len : (long)sizeof buf - 1;
		Bwrite(cfg->sess_bio, buf, wlen);
	}
}

/*
 * emitandsave — emit event AND write session with the same field list.
 *
 * We have to format the record twice because we can't reuse a va_list
 * after va_end.  The fields are passed as a nil-terminated char* array
 * to avoid the double-format cost on the hot path — but since this is
 * called at most a few times per second, simplicity wins.
 */
void
emitandsave(AgentCfg *cfg, ...)
{
	char   buf[65536];
	long   len;
	va_list ap;

	/* format once into buf */
	va_start(ap, cfg);
	len = fmtrec(buf, sizeof buf, ap);
	va_end(ap);

	{
		long wlen = len < (long)sizeof buf ? len : (long)sizeof buf - 1;

		if(cfg->onevent != nil)
			cfg->onevent(buf, wlen, cfg->aux);

		if(cfg->sess_bio != nil)
			Bwrite(cfg->sess_bio, buf, wlen);
	}
}

/* ── Token loading ────────────────────────────────────────────────────── */

/*
 * loadrefresh — read the GitHub access token from cfg->tokpath.
 * Returns a malloc'd nil-terminated string, or nil on error.
 */
static char *
loadrefresh(AgentCfg *cfg)
{
	int  fd;
	char buf[512];
	int  n;

	fd = open(cfg->tokpath, OREAD);
	if(fd < 0) {
		werrstr("loadrefresh: open %s: %r", cfg->tokpath);
		return nil;
	}
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0) {
		werrstr("loadrefresh: empty token file %s", cfg->tokpath);
		return nil;
	}
	buf[n] = '\0';
	/* strip trailing whitespace */
	while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
		buf[--n] = '\0';
	return strdup(buf);
}

/* ── agentrun ─────────────────────────────────────────────────────────── */

int
agentrun(char *prompt, OAIReq *req, AgentCfg *cfg)
{
	char        *refresh;
	OAuthToken  *tok;
	HTTPConn    *c;
	HTTPResp    *r;
	OAIParser    parser;
	OAIDelta     d;
	int          rc;
	HTTPHdr      hdrs[16];
	int          nhdrs;
	char        *body;
	long         bodylen;
	int          iteration;   /* tool loop iteration count */

	/* text accumulation for assistant message */
	char  *textbuf;
	long   textcap, textlen;

	/* tool call accumulation */
	char  tool_id[128];
	char  tool_name[128];
	char  argsbuf[65536];
	long  argslen;

	enum { TEXTCAP_INIT = 8192, MAX_ITERATIONS = 32 };

	/* ── load refresh token and get session token ── */
	refresh = loadrefresh(cfg);
	if(refresh == nil)
		return -1;

	tok = oauthsession(refresh, cfg->sockpath);
	free(refresh);
	if(tok == nil) {
		werrstr("agentrun: oauthsession: %r");
		return -1;
	}

	/* ── emit turn_start ── */
	emitandsave(cfg,
	    "turn_start", cfg->uuid[0] ? cfg->uuid : "no-session", cfg->model,
	    nil);

	/* ── append user message to req; write prompt session record ── */
	oaireqaddmsg(req, oaimsguser(prompt));
	writesession(cfg, "prompt", prompt, nil);

	/* allocate text accumulation buffer */
	textcap = TEXTCAP_INIT;
	textbuf = malloc(textcap + 1);
	if(textbuf == nil) {
		oauthtokenfree(tok);
		werrstr("agentrun: malloc textbuf: %r");
		return -1;
	}
	textbuf[0] = '\0';
	textlen    = 0;

	/* ── tool loop ── */
	for(iteration = 0; iteration < MAX_ITERATIONS; iteration++) {

		/* refresh token if near expiry */
		if(iteration > 0 && time(0) > tok->expires_at - 300) {
			char *ref2 = loadrefresh(cfg);
			if(ref2 != nil) {
				OAuthToken *newtok = oauthsession(ref2, cfg->sockpath);
				free(ref2);
				if(newtok != nil) {
					oauthtokenfree(tok);
					tok = newtok;
				}
			}
		}

		/* build and POST request */
		nhdrs = oaireqhdrs(req, tok->token, hdrs, 16);
		body  = oaireqjson(req, cfg->system, &bodylen);
		if(body == nil) {
			oauthtokenfree(tok);
			free(textbuf);
			werrstr("agentrun: oaireqjson: %r");
			return -1;
		}

		c = portdial("api.individual.githubcopilot.com", "443", cfg->sockpath);
		if(c == nil) {
			free(body);
			oauthtokenfree(tok);
			free(textbuf);
			werrstr("agentrun: portdial: %r");
			return -1;
		}

		r = httppost(c, "/chat/completions",
		             "api.individual.githubcopilot.com",
		             hdrs, nhdrs, body, bodylen);
		free(body);
		/* free smprint'd Authorization header value */
		{
			int i;
			for(i = 0; i < nhdrs; i++)
				if(strcmp(hdrs[i].name, "Authorization") == 0)
					free(hdrs[i].value);
		}

		if(r == nil || r->code != 200) {
			if(r != nil) {
				httpreadbody(r);
				werrstr("agentrun: HTTP %d: %s",
				        r->code, r->body ? r->body : "(no body)");
				httprespfree(r);
			} else {
				werrstr("agentrun: httppost: %r");
			}
			httpclose(c);
			oauthtokenfree(tok);
			free(textbuf);
			return -1;
		}

		/* ── stream deltas ── */
		oaiinit(&parser, r);

		textlen        = 0;
		textbuf[0]     = '\0';
		tool_id[0]     = '\0';
		tool_name[0]   = '\0';
		argslen        = 0;
		argsbuf[0]     = '\0';

		int stop_reason_is_tool = 0;
		char stop_reason[32];
		stop_reason[0] = '\0';

		while((rc = oaidelta(&parser, &d)) == OAI_OK) {
			switch(d.type) {

			case OAIDText:
				/* deliver text chunk to caller */
				if(cfg->ontext != nil)
					cfg->ontext(d.text, cfg->aux);
				/* emit text event record */
				emitandsave(cfg, "text", d.text, nil);
				/* accumulate for assistant message */
				{
					long dlen = strlen(d.text);
					if(textlen + dlen >= textcap) {
						long newcap = textcap * 2 + dlen + 1;
						char *tmp = realloc(textbuf, newcap + 1);
						if(tmp == nil) {
							/* out of memory — truncate */
							break;
						}
						textbuf = tmp;
						textcap = newcap;
					}
					memmove(textbuf + textlen, d.text, dlen);
					textlen += dlen;
					textbuf[textlen] = '\0';
				}
				break;

			case OAIDTool:
				/* new tool call starting */
				snprint(tool_id,   sizeof tool_id,   "%s",
				        d.tool_id   ? d.tool_id   : "");
				snprint(tool_name, sizeof tool_name, "%s",
				        d.tool_name ? d.tool_name : "exec");
				argslen    = 0;
				argsbuf[0] = '\0';
				break;

			case OAIDToolArg:
				/* accumulate JSON args */
				{
					long dlen = strlen(d.text);
					if(argslen + dlen < (long)sizeof argsbuf - 1) {
						memmove(argsbuf + argslen, d.text, dlen);
						argslen += dlen;
						argsbuf[argslen] = '\0';
					}
					/* if overflow: silently truncate — exec will fail gracefully */
				}
				break;

			case OAIDStop:
				snprint(stop_reason, sizeof stop_reason, "%s",
				        d.stop_reason ? d.stop_reason : "stop");
				stop_reason_is_tool = (strcmp(stop_reason, "tool_calls") == 0);
				break;
			}
		}

		/* done streaming this iteration */
		httprespfree(r);
		httpclose(c);

		if(rc == OAI_EOF && stop_reason[0] == '\0') {
			/* SSE ended without a finish_reason — treat as error */
			oauthtokenfree(tok);
			free(textbuf);
			werrstr("agentrun: SSE stream ended without finish_reason");
			return -1;
		}

		if(!stop_reason_is_tool) {
			/*
			 * Normal text turn complete.
			 * Append assistant text message to history.
			 */
			oaireqaddmsg(req, oaimsgassistant(textbuf));

			/* emit turn_end */
			emitandsave(cfg, "turn_end", "end_turn", nil);
			if(cfg->sess_bio != nil)
				Bflush(cfg->sess_bio);

			oauthtokenfree(tok);
			free(textbuf);
			return 0;
		}

		/*
		 * Tool call turn.
		 *
		 * 1. Append assistant message with text + tool call to history.
		 * 2. Emit tool_start event.
		 * 3. Execute the tool.
		 * 4. Emit tool_end event.
		 * 5. Append tool result to history.
		 * 6. Loop back to POST again.
		 */

		/* append assistant message: text + tool call */
		oaireqaddmsg(req, oaimsgtoolcall(
		    textbuf[0] ? textbuf : nil,
		    tool_id[0] ? tool_id : "call_unknown",
		    tool_name[0] ? tool_name : "exec",
		    argsbuf));

		/* emit tool_start */
		{
			/*
			 * Parse argv from argsbuf to get individual fields.
			 * For the event record, each argv element is its own field:
			 *   tool_start FS name FS id FS argv[0] FS argv[1] ...
			 *
			 * We parse here just enough to get argv elements as strings.
			 * If parsing fails, emit with just name+id (no argv fields).
			 */
			char *tsargv[EXEC_MAXARGV + 1];
			char *tsstdin = nil;
			int   tsargc;

			tsargc = execparse(argsbuf, argslen, tsargv, EXEC_MAXARGV, &tsstdin);
			free(tsstdin);

			if(tsargc > 0) {
				/* build nil-terminated field list on the stack */
				char *fields[EXEC_MAXARGV + 8];
				int   fi = 0, i;
				fields[fi++] = "tool_start";
				fields[fi++] = tool_name;
				fields[fi++] = tool_id;
				for(i = 0; i < tsargc; i++)
					fields[fi++] = tsargv[i];
				fields[fi] = nil;

				/* call emitevent and writesession using the fields array */
				{
					char  buf[65536];
					long  len, wlen;
					int   first = 1;
					long  total = 0;
					int   j;

					for(j = 0; j < fi; j++) {
						long flen;
						if(!first) {
							if(total < (long)sizeof buf - 1)
								buf[total] = '\x1f';
							total++;
						}
						first = 0;
						flen = strlen(fields[j]);
						{
							long rem = (long)sizeof buf - 1 - total;
							if(rem < 0) rem = 0;
							long copy = flen < rem ? flen : rem;
							if(copy > 0) memmove(buf + total, fields[j], copy);
						}
						total += flen;
					}
					if(total < (long)sizeof buf - 1)
						buf[total] = '\x1e';
					total++;
					wlen = total < (long)sizeof buf ? total : (long)sizeof buf - 1;
					buf[wlen] = '\0';
					len = wlen;

					if(cfg->onevent != nil)
						cfg->onevent(buf, len, cfg->aux);
					if(cfg->sess_bio != nil)
						Bwrite(cfg->sess_bio, buf, len);
				}

				for(i = 0; i < tsargc; i++)
					free(tsargv[i]);
			} else {
				/* fallback: no argv fields */
				emitandsave(cfg, "tool_start", tool_name, tool_id, nil);
			}
		}

		/* execute the tool */
		{
			ExecResult *er;
			char        result[EXEC_MAXOUT + 64];
			int         is_error;

			er = execrun(argsbuf, argslen);
			if(er == nil) {
				/* exec itself failed — send error result */
				char errbuf[256];
				rerrstr(errbuf, sizeof errbuf);
				emitandsave(cfg, "tool_end", "err", errbuf, nil);
				oaireqaddmsg(req, oaimsgtoolresult(
				    tool_id[0] ? tool_id : "call_unknown",
				    errbuf, 1));
				if(cfg->sess_bio != nil) Bflush(cfg->sess_bio);
			} else {
				is_error = (er->exitcode != 0);
				execresultstr(er, result, sizeof result);

				emitandsave(cfg, "tool_end",
				    is_error ? "err" : "ok",
				    result, nil);
				if(cfg->sess_bio != nil) Bflush(cfg->sess_bio);

				oaireqaddmsg(req, oaimsgtoolresult(
				    tool_id[0] ? tool_id : "call_unknown",
				    result, is_error));
				execresultfree(er);
			}
		}

		/* loop: POST again with tool result in history */
	}

	/* hit MAX_ITERATIONS */
	oauthtokenfree(tok);
	free(textbuf);
	werrstr("agentrun: exceeded %d tool iterations", MAX_ITERATIONS);
	return -1;
}

/* ── agentrunant ──────────────────────────────────────────────────────────
 *
 * Full agent turn using the Anthropic Messages wire format (Claude models).
 *
 * Structure mirrors agentrun() but:
 *   - Uses ANTReq / antdelta() / antreqjson() / antreqhdrs()
 *   - POSTs to /v1/messages
 *   - ANTDThinking → emitandsave("thinking", chunk); NOT sent to ontext
 *   - stop_reason "end_turn" → normal completion
 *   - stop_reason "tool_use" → tool call; loop
 *   - History: antmsgtooluse() + antmsgtoolresult()
 */
int
agentrunant(char *prompt, ANTReq *req, AgentCfg *cfg)
{
	char        *refresh;
	OAuthToken  *tok;
	HTTPConn    *c;
	HTTPResp    *r;
	ANTParser    parser;
	ANTDelta     d;
	int          rc;
	HTTPHdr      hdrs[16];
	int          nhdrs;
	char        *body;
	long         bodylen;
	int          iteration;

	/* text accumulation for assistant message */
	char  *textbuf;
	long   textcap, textlen;

	/* thinking accumulation (for session only; not sent to API) */
	/* We do not reconstruct thinking — just emit chunk by chunk */

	/* tool call accumulation */
	char  tool_id[128];
	char  tool_name[128];
	char  argsbuf[65536];
	long  argslen;

	enum { TEXTCAP_INIT = 8192, MAX_ITERATIONS = 32 };

	/* ── load refresh token and get session token ── */
	refresh = loadrefresh(cfg);
	if(refresh == nil)
		return -1;

	tok = oauthsession(refresh, cfg->sockpath);
	free(refresh);
	if(tok == nil) {
		werrstr("agentrunant: oauthsession: %r");
		return -1;
	}

	/* ── emit turn_start ── */
	emitandsave(cfg,
	    "turn_start", cfg->uuid[0] ? cfg->uuid : "no-session", cfg->model,
	    nil);

	/* ── append user message to req; write prompt session record ── */
	antreqaddmsg(req, antmsguser(prompt));
	writesession(cfg, "prompt", prompt, nil);

	/* allocate text accumulation buffer */
	textcap = TEXTCAP_INIT;
	textbuf = malloc(textcap + 1);
	if(textbuf == nil) {
		oauthtokenfree(tok);
		werrstr("agentrunant: malloc textbuf: %r");
		return -1;
	}
	textbuf[0] = '\0';
	textlen    = 0;

	/* ── tool loop ── */
	for(iteration = 0; iteration < MAX_ITERATIONS; iteration++) {

		/* refresh token if near expiry */
		if(iteration > 0 && time(0) > tok->expires_at - 300) {
			char *ref2 = loadrefresh(cfg);
			if(ref2 != nil) {
				OAuthToken *newtok = oauthsession(ref2, cfg->sockpath);
				free(ref2);
				if(newtok != nil) {
					oauthtokenfree(tok);
					tok = newtok;
				}
			}
		}

		/* build and POST request to /v1/messages */
		nhdrs = antreqhdrs(req, tok->token, hdrs, 16);
		body  = antreqjson(req, cfg->system, &bodylen);
		if(body == nil) {
			oauthtokenfree(tok);
			free(textbuf);
			werrstr("agentrunant: antreqjson: %r");
			return -1;
		}

		c = portdial("api.individual.githubcopilot.com", "443", cfg->sockpath);
		if(c == nil) {
			free(body);
			oauthtokenfree(tok);
			free(textbuf);
			werrstr("agentrunant: portdial: %r");
			return -1;
		}

		r = httppost(c, "/v1/messages",
		             "api.individual.githubcopilot.com",
		             hdrs, nhdrs, body, bodylen);
		free(body);
		/* free smprint'd Authorization header value */
		{
			int i;
			for(i = 0; i < nhdrs; i++)
				if(strcmp(hdrs[i].name, "Authorization") == 0)
					free(hdrs[i].value);
		}

		if(r == nil || r->code != 200) {
			if(r != nil) {
				httpreadbody(r);
				werrstr("agentrunant: HTTP %d: %s",
				        r->code, r->body ? r->body : "(no body)");
				httprespfree(r);
			} else {
				werrstr("agentrunant: httppost: %r");
			}
			httpclose(c);
			oauthtokenfree(tok);
			free(textbuf);
			return -1;
		}

		/* ── stream deltas ── */
		antinit(&parser, r);

		textlen        = 0;
		textbuf[0]     = '\0';
		tool_id[0]     = '\0';
		tool_name[0]   = '\0';
		argslen        = 0;
		argsbuf[0]     = '\0';

		int stop_reason_is_tool = 0;
		char stop_reason[32];
		stop_reason[0] = '\0';

		while((rc = antdelta(&parser, &d)) == ANT_OK) {
			switch(d.type) {

			case ANTDText:
				/* deliver text chunk to caller */
				if(cfg->ontext != nil)
					cfg->ontext(d.text, cfg->aux);
				/* emit text event record */
				emitandsave(cfg, "text", d.text, nil);
				/* accumulate for assistant message */
				{
					long dlen = strlen(d.text);
					if(textlen + dlen >= textcap) {
						long newcap = textcap * 2 + dlen + 1;
						char *tmp = realloc(textbuf, newcap + 1);
						if(tmp != nil) {
							textbuf = tmp;
							textcap = newcap;
						}
					}
					if(textlen + dlen < textcap) {
						memmove(textbuf + textlen, d.text, dlen);
						textlen += dlen;
						textbuf[textlen] = '\0';
					}
				}
				break;

			case ANTDThinking:
				/*
				 * Thinking blocks: emit to event stream and session file,
				 * but do NOT deliver to cfg->ontext and do NOT add to
				 * API history (Anthropic forbids sending thinking blocks back).
				 */
				emitandsave(cfg, "thinking", d.text, nil);
				break;

			case ANTDTool:
				/* new tool_use block starting */
				snprint(tool_id,   sizeof tool_id,   "%s",
				        d.tool_id   ? d.tool_id   : "");
				snprint(tool_name, sizeof tool_name, "%s",
				        d.tool_name ? d.tool_name : "exec");
				argslen    = 0;
				argsbuf[0] = '\0';
				break;

			case ANTDToolArg:
				/* accumulate JSON input */
				{
					long dlen = strlen(d.text);
					if(argslen + dlen < (long)sizeof argsbuf - 1) {
						memmove(argsbuf + argslen, d.text, dlen);
						argslen += dlen;
						argsbuf[argslen] = '\0';
					}
				}
				break;

			case ANTDStop:
				snprint(stop_reason, sizeof stop_reason, "%s",
				        d.stop_reason ? d.stop_reason : "end_turn");
				stop_reason_is_tool = (strcmp(stop_reason, "tool_use") == 0);
				break;
			}
		}

		/* done streaming this iteration */
		httprespfree(r);
		httpclose(c);

		if(rc == ANT_EOF && stop_reason[0] == '\0') {
			oauthtokenfree(tok);
			free(textbuf);
			werrstr("agentrunant: SSE stream ended without stop_reason");
			return -1;
		}

		if(!stop_reason_is_tool) {
			/*
			 * Normal text turn complete (stop_reason "end_turn").
			 * Append assistant text message to history.
			 */
			antreqaddmsg(req, antmsgassistant(textbuf));

			/* emit turn_end */
			emitandsave(cfg, "turn_end", "end_turn", nil);
			if(cfg->sess_bio != nil)
				Bflush(cfg->sess_bio);

			oauthtokenfree(tok);
			free(textbuf);
			return 0;
		}

		/*
		 * Tool call turn (stop_reason "tool_use").
		 *
		 * 1. Append assistant message (text + tool_use) to history.
		 * 2. Emit tool_start event.
		 * 3. Execute the tool.
		 * 4. Emit tool_end event.
		 * 5. Append tool result (user message) to history.
		 * 6. Loop back to POST again.
		 */

		/* append assistant message: text + tool_use block */
		antreqaddmsg(req, antmsgtooluse(
		    textbuf[0] ? textbuf : nil,
		    tool_id[0]   ? tool_id   : "toolu_unknown",
		    tool_name[0] ? tool_name : "exec",
		    argsbuf[0]   ? argsbuf   : "{}"));

		/* emit tool_start */
		{
			char *tsargv[EXEC_MAXARGV + 1];
			char *tsstdin = nil;
			int   tsargc;

			tsargc = execparse(argsbuf, argslen, tsargv, EXEC_MAXARGV, &tsstdin);
			free(tsstdin);

			if(tsargc > 0) {
				char  buf[65536];
				long  total = 0;
				int   first = 1;
				int   j;

				for(j = 0; j < tsargc + 3; j++) {
					char *fld;
					long  flen;
					if(j == 0)      fld = "tool_start";
					else if(j == 1) fld = tool_name;
					else if(j == 2) fld = tool_id;
					else            fld = tsargv[j - 3];

					if(!first) {
						if(total < (long)sizeof buf - 1) buf[total] = '\x1f';
						total++;
					}
					first = 0;
					flen = strlen(fld);
					{
						long rem = (long)sizeof buf - 1 - total;
						if(rem < 0) rem = 0;
						long copy = flen < rem ? flen : rem;
						if(copy > 0) memmove(buf + total, fld, copy);
					}
					total += flen;
				}
				if(total < (long)sizeof buf - 1) buf[total] = '\x1e';
				total++;
				{
					long wlen = total < (long)sizeof buf ? total : (long)sizeof buf - 1;
					buf[wlen] = '\0';
					if(cfg->onevent != nil) cfg->onevent(buf, wlen, cfg->aux);
					if(cfg->sess_bio != nil) Bwrite(cfg->sess_bio, buf, wlen);
				}

				for(j = 0; j < tsargc; j++) free(tsargv[j]);
			} else {
				emitandsave(cfg, "tool_start", tool_name, tool_id, nil);
			}
		}

		/* execute the tool */
		{
			ExecResult *er;
			char        result[EXEC_MAXOUT + 64];
			int         is_error;

			er = execrun(argsbuf, argslen);
			if(er == nil) {
				char errbuf[256];
				rerrstr(errbuf, sizeof errbuf);
				emitandsave(cfg, "tool_end", "err", errbuf, nil);
				antreqaddmsg(req, antmsgtoolresult(
				    tool_id[0] ? tool_id : "toolu_unknown",
				    errbuf, 1));
				if(cfg->sess_bio != nil) Bflush(cfg->sess_bio);
			} else {
				is_error = (er->exitcode != 0);
				execresultstr(er, result, sizeof result);

				emitandsave(cfg, "tool_end",
				    is_error ? "err" : "ok",
				    result, nil);
				if(cfg->sess_bio != nil) Bflush(cfg->sess_bio);

				antreqaddmsg(req, antmsgtoolresult(
				    tool_id[0] ? tool_id : "toolu_unknown",
				    result, is_error));
				execresultfree(er);
			}
		}

		/* loop: POST again with tool result in history */
	}

	/* hit MAX_ITERATIONS */
	oauthtokenfree(tok);
	free(textbuf);
	werrstr("agentrunant: exceeded %d tool iterations", MAX_ITERATIONS);
	return -1;
}
