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
 * emit / writesession varargs format records using fmtrecfields(), which
 * measures the required size and heap-allocates exactly the right buffer,
 * so records of any length are handled correctly.
 *
 * argsbuf for tool call JSON arguments grows dynamically via realloc(),
 * matching the same pattern used for textbuf.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>
#ifndef PLAN9
#include <stdarg.h>
#endif

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
			char *empty_argv[2];
			int   i;

			empty_argv[0] = nil;
			empty_argv[1] = nil;
			if(nf >= 2) tr_is_error = (strcmp(fields[1], "err") == 0);
			if(nf >= 3) { free(tr_output); tr_output = strdup(fields[2]); }

			/* build JSON args from argv */
			tool_args_json = argv2json(
			    tc_argv != nil ? tc_argv : empty_argv,
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
		} else if(strcmp(fields[0], "trim") == 0) {
			/*
			 * trim FS n — replay an in-session trim by discarding the
			 * oldest n turn pairs from the reconstructed history.
			 * Any partial state (accumulated text, tool call) is
			 * also discarded since it would have preceded the trim.
			 */
			if(nf >= 2) {
				int n = atoi(fields[1]);
				if(n > 0) {
					/* discard any partial turn being accumulated */
					free(textbuf); textbuf = nil; textcap = 0; textlen = 0;
					free(tc_name); tc_name = nil;
					free(tc_id);   tc_id   = nil;
					free(tr_output); tr_output = nil;
					if(tc_argv != nil) {
						int i;
						for(i = 0; i < tc_argc; i++) free(tc_argv[i]);
						free(tc_argv); tc_argv = nil;
					}
					tc_argc = 0;
					state = ST_IDLE;

					oaireqtrim(req, n);
				}
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
 * fmtrecfields — format a char*[] field list into a heap-allocated buffer.
 *
 * Format: field₀ FS field₁ FS … FS fieldₙ RS NUL
 *
 * Returns a malloc'd buffer sized exactly for the record (including the
 * trailing RS and NUL), with *lenp set to the number of bytes before the
 * NUL.  Returns nil on allocation failure.
 */
static char *
fmtrecfields(char **fields, int nfields, long *lenp)
{
	long  total = 0;
	long  wpos  = 0;
	int   i;
	char *buf;

	/* measure */
	for(i = 0; i < nfields; i++) {
		if(i > 0) total++;          /* FS separator */
		total += strlen(fields[i]);
	}
	total++;  /* RS terminator */

	buf = malloc(total + 1);
	if(buf == nil)
		return nil;

	/* write */
	for(i = 0; i < nfields; i++) {
		long flen = strlen(fields[i]);
		if(i > 0) buf[wpos++] = '\x1f';
		memmove(buf + wpos, fields[i], flen);
		wpos += flen;
	}
	buf[wpos++] = '\x1e';
	buf[wpos]   = '\0';

	*lenp = total;
	return buf;
}

/*
 * fmtrecva — collect varargs into a fields array, then call fmtrecfields.
 *
 * Takes a nil-terminated va_list of char *.
 * Returns a malloc'd record buffer, or nil on failure.
 */
static char *
fmtrecva(va_list ap, long *lenp)
{
	char  *tmp[64];
	int    n = 0;
	char  *f;

	while((f = va_arg(ap, char *)) != nil && n < 64)
		tmp[n++] = f;
	return fmtrecfields(tmp, n, lenp);
}

void
emitevent(AgentCfg *cfg, ...)
{
	char   *buf;
	long    len;
	va_list ap;

	if(cfg->onevent == nil)
		return;

	va_start(ap, cfg);
	buf = fmtrecva(ap, &len);
	va_end(ap);

	if(buf == nil)
		return;
	cfg->onevent(buf, len, cfg->aux);
	free(buf);
}

void
writesession(AgentCfg *cfg, ...)
{
	char   *buf;
	long    len;
	va_list ap;

	if(cfg->sess_bio == nil)
		return;

	va_start(ap, cfg);
	buf = fmtrecva(ap, &len);
	va_end(ap);

	if(buf == nil)
		return;
	Bwrite(cfg->sess_bio, buf, len);
	free(buf);
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
	char   *buf;
	long    len;
	va_list ap;

	va_start(ap, cfg);
	buf = fmtrecva(ap, &len);
	va_end(ap);

	if(buf == nil)
		return;
	if(cfg->onevent != nil)
		cfg->onevent(buf, len, cfg->aux);
	if(cfg->sess_bio != nil)
		Bwrite(cfg->sess_bio, buf, len);
	free(buf);
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

/* ── Context management helpers ──────────────────────────────────────────
 *
 * EXEC_MAXOUT_DEFAULT — fallback output cap for tool execution when
 * cfg->exec_maxout is 0 (model context window unknown).
 *
 * HIST_MAXOUT — cap applied to tool output when storing it in the history
 * linked list.  The model saw the full exec_maxout bytes on the turn the
 * tool ran; future turns need only enough to know what happened.  Keeping
 * this small prevents tool outputs from dominating context growth across
 * long sessions.
 *
 * isctxoverflow — return 1 if an HTTP error body looks like the model
 * rejected the request because the context is too long.  Matches the
 * GitHub Copilot error format as well as generic OpenAI / Anthropic forms.
 */

enum { EXEC_MAXOUT_DEFAULT = 512 * 1024 };  /* 512 KB fallback output cap  */
enum { HIST_MAXOUT = 16 * 1024 };           /* 16 KB per tool result in history */

int
isctxoverflow(const char *body)
{
	if(body == nil)
		return 0;
	/* GitHub Copilot OAI: "prompt token count of X exceeds the limit of Y" */
	if(strstr(body, "exceeds the limit") != nil) return 1;
	/* OpenAI generic */
	if(strstr(body, "exceeds the context window") != nil) return 1;
	/* Anthropic: "prompt is too long" */
	if(strstr(body, "prompt is too long") != nil) return 1;
	/* generic fallbacks */
	if(strstr(body, "context_length_exceeded") != nil) return 1;
	if(strstr(body, "context window") != nil) return 1;
	return 0;
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
	char  *argsbuf;
	long   argscap, argslen;

	enum { TEXTCAP_INIT = 8192, ARGSCAP_INIT = 8192 };

	argscap = ARGSCAP_INIT;
	argsbuf = mallocz(argscap + 1, 1);
	if(argsbuf == nil)
		return -1;

	/* ── load refresh token and get session token ── */
	refresh = loadrefresh(cfg);
	if(refresh == nil) {
		free(argsbuf);
		return -1;
	}

	tok = oauthsession(refresh, cfg->sockpath);
	free(refresh);
	if(tok == nil) {
		werrstr("agentrun: oauthsession: %r");
		free(argsbuf);
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
		free(argsbuf);
		werrstr("agentrun: malloc textbuf: %r");
		return -1;
	}
	textbuf[0] = '\0';
	textlen    = 0;

	/* ── tool loop ── */
	for(iteration = 0; ; iteration++) {

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
			free(textbuf); free(argsbuf);
			werrstr("agentrun: oaireqjson: %r");
			return -1;
		}

		c = portdial("api.individual.githubcopilot.com", "443", cfg->sockpath);
		if(c == nil) {
			free(body);
			oauthtokenfree(tok);
			free(textbuf); free(argsbuf);
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
				if(isctxoverflow(r->body))
					werrstr("agentrun: context too large — write 'clear' to /ctl to start a new session");
				else
					werrstr("agentrun: HTTP %d: %s",
					        r->code, r->body ? r->body : "(no body)");
				httprespfree(r);
			} else {
				werrstr("agentrun: httppost: %r");
			}
			httpclose(c);
			oauthtokenfree(tok);
			free(textbuf); free(argsbuf);
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
		int aborted = 0;

		while((rc = oaidelta(&parser, &d)) == OAI_OK) {
			/* check for abort signal between deltas */
			if(cfg->abortchan != nil && nbrecvp(cfg->abortchan) != nil) {
				aborted = 1;
				break;
			}
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
					if(argslen + dlen >= argscap) {
						long newcap = argscap * 2 + dlen + 1;
						char *tmp = realloc(argsbuf, newcap + 1);
						if(tmp == nil)
							break;
						argsbuf = tmp;
						argscap = newcap;
					}
					memmove(argsbuf + argslen, d.text, dlen);
					argslen += dlen;
					argsbuf[argslen] = '\0';
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
		oaiterm(&parser);
		httprespfree(r);
		httpclose(c);

		if(aborted) {
			emitandsave(cfg, "turn_end", "aborted", nil);
			if(cfg->sess_bio != nil)
				Bflush(cfg->sess_bio);
			oauthtokenfree(tok);
			free(textbuf); free(argsbuf);
			werrstr("agentrun: aborted");
			return -1;
		}

		if(rc == OAI_EOF && stop_reason[0] == '\0') {
			/* SSE ended without a finish_reason — treat as error */
			oauthtokenfree(tok);
			free(textbuf); free(argsbuf);
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
			free(textbuf); free(argsbuf);
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

				{
					char *buf;
					long  len;

					buf = fmtrecfields(fields, fi, &len);
					if(buf != nil) {
						if(cfg->onevent != nil)
							cfg->onevent(buf, len, cfg->aux);
						if(cfg->sess_bio != nil)
							Bwrite(cfg->sess_bio, buf, len);
						free(buf);
					}
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
			char       *result;
			int         is_error;
			int         tool_aborted = 0;
			long        maxout;

			maxout = cfg->exec_maxout > 0 ? cfg->exec_maxout : EXEC_MAXOUT_DEFAULT;

			result = mallocz(maxout + 64, 1);
			if(result == nil) {
				oauthtokenfree(tok);
				free(textbuf); free(argsbuf);
				werrstr("agentrun: malloc result: %r");
				return -1;
			}

			/*
			 * Spawn a watcher proc that blocks on abortchan and
			 * kills the child if an abort arrives while execrun()
			 * is blocking in collectoutput().  We pass the child
			 * pid via a shared int written before the watcher is
			 * spawned; the watcher polls until pid != 0.
			 *
			 * The watcher is RFNOWAIT so we never need to collect
			 * its exit status.
			 */
			er = execrun(argsbuf, argslen, maxout);

			/* check for abort that arrived during exec */
			if(cfg->abortchan != nil && nbrecvp(cfg->abortchan) != nil) {
				if(er != nil)
					execabort(er->pid);
				tool_aborted = 1;
			}

			if(tool_aborted) {
				execresultfree(er);
				free(result);
				emitandsave(cfg, "turn_end", "aborted", nil);
				if(cfg->sess_bio != nil)
					Bflush(cfg->sess_bio);
				oauthtokenfree(tok);
				free(textbuf); free(argsbuf);
				werrstr("agentrun: aborted");
				return -1;
			}

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
				execresultstr(er, result, maxout + 64);

				emitandsave(cfg, "tool_end",
				    is_error ? "err" : "ok",
				    result, nil);
				if(cfg->sess_bio != nil) Bflush(cfg->sess_bio);

				/*
				 * Cap the result stored in history at HIST_MAXOUT,
				 * keeping the tail: the end of output is where the
				 * conclusion or error lives.  The model already saw
				 * the full output this turn; subsequent turns only
				 * need enough to know what happened.
				 */
				{
					long rlen = (long)strlen(result);
					if(rlen > HIST_MAXOUT) {
						static const char histmark[] = "[...history truncated...]\n";
						long marklen = sizeof(histmark) - 1;
						long tail_start = rlen - HIST_MAXOUT;
						memmove(result + marklen, result + tail_start, HIST_MAXOUT);
						memmove(result, histmark, marklen);
						result[marklen + HIST_MAXOUT] = '\0';
					}
				}
				oaireqaddmsg(req, oaimsgtoolresult(
				    tool_id[0] ? tool_id : "call_unknown",
				    result, is_error));
				execresultfree(er);
			}
			free(result);
		}

		/* loop: POST again with tool result in history */
	}
	return -1;	/* not reached */
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
	char  *argsbuf;
	long   argscap, argslen;

	enum { TEXTCAP_INIT = 8192, ARGSCAP_INIT = 8192 };

	argscap = ARGSCAP_INIT;
	argsbuf = mallocz(argscap + 1, 1);
	if(argsbuf == nil)
		return -1;

	/* ── load refresh token and get session token ── */
	refresh = loadrefresh(cfg);
	if(refresh == nil) {
		free(argsbuf);
		return -1;
	}

	tok = oauthsession(refresh, cfg->sockpath);
	free(refresh);
	if(tok == nil) {
		werrstr("agentrunant: oauthsession: %r");
		free(argsbuf);
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
	for(iteration = 0; ; iteration++) {

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
			free(textbuf); free(argsbuf);
			werrstr("agentrunant: antreqjson: %r");
			return -1;
		}

		c = portdial("api.individual.githubcopilot.com", "443", cfg->sockpath);
		if(c == nil) {
			free(body);
			oauthtokenfree(tok);
			free(textbuf); free(argsbuf);
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
				if(isctxoverflow(r->body))
					werrstr("agentrunant: context too large — write 'clear' to /ctl to start a new session");
				else
					werrstr("agentrunant: HTTP %d: %s",
					        r->code, r->body ? r->body : "(no body)");
				httprespfree(r);
			} else {
				werrstr("agentrunant: httppost: %r");
			}
			httpclose(c);
			oauthtokenfree(tok);
			free(textbuf); free(argsbuf);
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
		int aborted = 0;

		while((rc = antdelta(&parser, &d)) == ANT_OK) {
			/* check for abort signal between deltas */
			if(cfg->abortchan != nil && nbrecvp(cfg->abortchan) != nil) {
				aborted = 1;
				break;
			}
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
					if(argslen + dlen >= argscap) {
						long newcap = argscap * 2 + dlen + 1;
						char *tmp = realloc(argsbuf, newcap + 1);
						if(tmp == nil)
							break;
						argsbuf = tmp;
						argscap = newcap;
					}
					memmove(argsbuf + argslen, d.text, dlen);
					argslen += dlen;
					argsbuf[argslen] = '\0';
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
		antterm(&parser);
		httprespfree(r);
		httpclose(c);

		if(aborted) {
			emitandsave(cfg, "turn_end", "aborted", nil);
			if(cfg->sess_bio != nil)
				Bflush(cfg->sess_bio);
			oauthtokenfree(tok);
			free(textbuf); free(argsbuf);
			werrstr("agentrunant: aborted");
			return -1;
		}

		if(rc == ANT_EOF && stop_reason[0] == '\0') {
			oauthtokenfree(tok);
			free(textbuf); free(argsbuf);
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
			free(textbuf); free(argsbuf);
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
				char  *buf;
				char  **flds;
				long  len;
				int   j;

				flds = malloc((tsargc + 3) * sizeof(char*));
				if(flds == nil) {
					for(j = 0; j < tsargc; j++) free(tsargv[j]);
					break;
				}
				flds[0] = "tool_start";
				flds[1] = tool_name;
				flds[2] = tool_id;
				for(j = 0; j < tsargc; j++)
					flds[3 + j] = tsargv[j];

				buf = fmtrecfields(flds, tsargc + 3, &len);
				if(buf != nil) {
					if(cfg->onevent != nil) cfg->onevent(buf, len, cfg->aux);
					if(cfg->sess_bio != nil) Bwrite(cfg->sess_bio, buf, len);
					free(buf);
				}
				free(flds);

				for(j = 0; j < tsargc; j++) free(tsargv[j]);
			} else {
				emitandsave(cfg, "tool_start", tool_name, tool_id, nil);
			}
		}

		/* execute the tool */
		{
			ExecResult *er;
			char       *result;
			int         is_error;
			int         tool_aborted = 0;
			long        maxout;

			maxout = cfg->exec_maxout > 0 ? cfg->exec_maxout : EXEC_MAXOUT_DEFAULT;

			result = mallocz(maxout + 64, 1);
			if(result == nil) {
				oauthtokenfree(tok);
				free(textbuf); free(argsbuf);
				werrstr("agentrunant: malloc result: %r");
				return -1;
			}

			er = execrun(argsbuf, argslen, maxout);

			/* check for abort that arrived during exec */
			if(cfg->abortchan != nil && nbrecvp(cfg->abortchan) != nil) {
				if(er != nil)
					execabort(er->pid);
				tool_aborted = 1;
			}

			if(tool_aborted) {
				execresultfree(er);
				free(result);
				emitandsave(cfg, "turn_end", "aborted", nil);
				if(cfg->sess_bio != nil)
					Bflush(cfg->sess_bio);
				oauthtokenfree(tok);
				free(textbuf); free(argsbuf);
				werrstr("agentrunant: aborted");
				return -1;
			}

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
				execresultstr(er, result, maxout + 64);

				emitandsave(cfg, "tool_end",
				    is_error ? "err" : "ok",
				    result, nil);
				if(cfg->sess_bio != nil) Bflush(cfg->sess_bio);

				/*
				 * Cap the result stored in history at HIST_MAXOUT,
				 * keeping the tail: the end of output is where the
				 * conclusion or error lives.  The model already saw
				 * the full output this turn; subsequent turns only
				 * need enough to know what happened.
				 */
				{
					long rlen = (long)strlen(result);
					if(rlen > HIST_MAXOUT) {
						static const char histmark[] = "[...history truncated...]\n";
						long marklen = sizeof(histmark) - 1;
						long tail_start = rlen - HIST_MAXOUT;
						memmove(result + marklen, result + tail_start, HIST_MAXOUT);
						memmove(result, histmark, marklen);
						result[marklen + HIST_MAXOUT] = '\0';
					}
				}
				antreqaddmsg(req, antmsgtoolresult(
				    tool_id[0] ? tool_id : "toolu_unknown",
				    result, is_error));
				execresultfree(er);
			}
			free(result);
		}

		/* loop: POST again with tool result in history */
	}

	return -1;	/* not reached */
}
