/*
 * http.c — HTTP/1.1 client for 9ai
 *
 * One Biobuf per HTTPConn, allocated at dial time and used for all
 * reads on that connection.  Writes use a short-lived local Biobuf in
 * OWRITE mode (safe: the read Biobuf hasn't consumed any bytes yet
 * when we write the request).
 *
 * Connection lifetime: one request/response, then close.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <libsec.h>

#include "http.h"

/*
 * tlsdial — open a TLS connection to host:port.
 *
 * Dials tcp!host!port and wraps with tlsClient().
 * Returns a new HTTPConn on success, nil on failure (werrstr set).
 */
HTTPConn *
tlsdial(char *host, char *port)
{
	HTTPConn *c;
	TLSconn   conn;
	char      addr[256];
	int       fd;

	snprint(addr, sizeof addr, "tcp!%s!%s", host, port);
	fd = dial(addr, nil, nil, nil);
	if(fd < 0) {
		werrstr("tlsdial: dial %s: %r", addr);
		return nil;
	}

	memset(&conn, 0, sizeof conn);
	conn.serverName = smprint("%s", host);
	fd = tlsClient(fd, &conn);
	if(fd < 0) {
		werrstr("tlsdial: tlsClient %s: %r", host);
		free(conn.serverName);
		return nil;
	}
	free(conn.cert);
	free(conn.sessionID);
	free(conn.serverName);

	c = mallocz(sizeof *c, 1);
	if(c == nil) {
		close(fd);
		return nil;
	}
	c->fd   = fd;
	c->host = strdup(host);
	c->bio  = mallocz(sizeof(Biobuf), 1);
	if(c->bio == nil) {
		close(fd);
		free(c->host);
		free(c);
		return nil;
	}
	Binit(c->bio, fd, OREAD);
	return c;
}

/*
 * httpclose — tear down a connection.
 */
void
httpclose(HTTPConn *c)
{
	if(c == nil)
		return;
	if(c->bio != nil) {
		Bterm(c->bio);
		free(c->bio);
	}
	if(c->fd >= 0)
		close(c->fd);
	free(c->host);
	free(c);
}

/*
 * writereq — emit request line, headers, and optional body to the fd.
 *
 * Uses a local OWRITE Biobuf, flushed before returning.  Safe to call
 * before the connection's read Biobuf (c->bio) has consumed anything.
 */
static int
writereq(HTTPConn *c, char *method, char *path, char *host,
         HTTPHdr *hdrs, int nhdrs, char *body, long bodylen)
{
	Biobuf *wb;
	int     i, ok;

	wb = mallocz(sizeof(Biobuf), 1);
	if(wb == nil)
		return -1;
	Binit(wb, c->fd, OWRITE);
	Bprint(wb, "%s %s HTTP/1.1\r\n", method, path);
	Bprint(wb, "Host: %s\r\n", host);
	Bprint(wb, "Connection: close\r\n");
	for(i = 0; i < nhdrs; i++)
		Bprint(wb, "%s: %s\r\n", hdrs[i].name, hdrs[i].value);
	if(body != nil && bodylen > 0)
		Bprint(wb, "Content-Length: %ld\r\n", bodylen);
	Bprint(wb, "\r\n");
	if(body != nil && bodylen > 0)
		Bwrite(wb, body, bodylen);
	ok = Bflush(wb) != Beof;
	Bterm(wb);
	free(wb);
	return ok ? 0 : -1;
}

/*
 * readstatus — read "HTTP/1.1 NNN ...\r\n" from c->bio, return status code.
 */
static int
readstatus(HTTPConn *c)
{
	char *line, *p, save;
	int   n, code;

	line = Brdline(c->bio, '\n');
	if(line == nil) {
		werrstr("http: EOF reading status line");
		return -1;
	}
	n = Blinelen(c->bio);

	save = line[n-1];
	line[n-1] = '\0';
	if(n >= 2 && line[n-2] == '\r')
		line[n-2] = '\0';

	if(strncmp(line, "HTTP/", 5) != 0) {
		werrstr("http: bad status line: %.40s", line);
		line[n-1] = save;
		return -1;
	}
	p = strchr(line, ' ');
	if(p == nil) {
		werrstr("http: malformed status line");
		line[n-1] = save;
		return -1;
	}
	code = atoi(p + 1);
	line[n-1] = save;

	if(code <= 0) {
		werrstr("http: invalid status code %d", code);
		return -1;
	}
	return code;
}

/*
 * skipheaders — consume HTTP response headers up to the blank line.
 * Sets *chunkedp = 1 if Transfer-Encoding: chunked is present.
 */
static int
skipheaders(HTTPConn *c, int *chunkedp)
{
	char *line;
	int   n;

	*chunkedp = 0;
	for(;;) {
		line = Brdline(c->bio, '\n');
		if(line == nil) {
			werrstr("http: EOF in headers");
			return -1;
		}
		n = Blinelen(c->bio);
		if(n == 1 || (n == 2 && line[0] == '\r'))
			break;
		if(n >= 2 && line[n-2] == '\r') line[n-2] = '\0';
		else line[n-1] = '\0';
		if(cistrstr(line, "Transfer-Encoding") != nil &&
		   cistrstr(line, "chunked") != nil)
			*chunkedp = 1;
	}
	return 0;
}

/*
 * sendreq — write request, read status + headers, return response struct.
 * Body is left unread in c->bio.
 */
static HTTPResp *
sendreq(HTTPConn *c, char *method, char *path, char *host,
        HTTPHdr *hdrs, int nhdrs, char *body, long bodylen)
{
	HTTPResp *r;
	int       code, chunked;

	if(writereq(c, method, path, host, hdrs, nhdrs, body, bodylen) < 0)
		return nil;
	code = readstatus(c);
	if(code < 0)
		return nil;
	if(skipheaders(c, &chunked) < 0)
		return nil;

	r = mallocz(sizeof *r, 1);
	if(r == nil)
		return nil;
	r->code    = code;
	r->conn    = c;
	r->chunked = chunked;
	return r;
}

/*
 * httpget — send GET, return response.
 */
HTTPResp *
httpget(HTTPConn *c, char *path, char *host, HTTPHdr *hdrs, int nhdrs)
{
	return sendreq(c, "GET", path, host, hdrs, nhdrs, nil, 0);
}

/*
 * httppost — send POST with body, return response.
 */
HTTPResp *
httppost(HTTPConn *c, char *path, char *host, HTTPHdr *hdrs, int nhdrs,
         char *body, long bodylen)
{
	return sendreq(c, "POST", path, host, hdrs, nhdrs, body, bodylen);
}

/*
 * httpreadbody — slurp entire response body into r->body.
 * Sets r->body (nil-terminated) and r->bodylen.
 * Handles both identity and chunked transfer encoding.
 */
int
httpreadbody(HTTPResp *r)
{
	enum { CHUNK = 8192 };
	char *buf;
	long  cap, len, n;

	cap = CHUNK;
	len = 0;
	buf = malloc(cap + 1);
	if(buf == nil)
		return -1;

	if(r->chunked) {
		char *line;
		int   llen;
		long  chunklen;

		for(;;) {
			line = Brdline(r->conn->bio, '\n');
			if(line == nil)
				break;
			llen = Blinelen(r->conn->bio);
			if(llen >= 2 && line[llen-2] == '\r') line[llen-2] = '\0';
			else line[llen-1] = '\0';

			chunklen = strtol(line, nil, 16);
			if(chunklen <= 0)
				break;

			if(len + chunklen + 1 > cap) {
				long newcap = cap * 2 + chunklen + 1;
				char *tmp = realloc(buf, newcap + 1);
				if(tmp == nil) { free(buf); return -1; }
				buf = tmp;
				cap = newcap;
			}

			n = Bread(r->conn->bio, buf + len, chunklen);
			if(n < chunklen) {
				if(n > 0) len += n;
				break;
			}
			len += n;
			Brdline(r->conn->bio, '\n');  /* consume trailing \r\n */
		}
	} else {
		for(;;) {
			if(len + CHUNK > cap) {
				cap *= 2;
				buf = realloc(buf, cap + 1);
				if(buf == nil)
					return -1;
			}
			n = Bread(r->conn->bio, buf + len, CHUNK);
			if(n <= 0)
				break;
			len += n;
		}
	}

	buf[len] = '\0';
	r->body    = buf;
	r->bodylen = len;
	return 0;
}

/*
 * httpreadline — read one line from the response body (SSE).
 *
 * Returns pointer into Biobuf's internal buffer; valid until next call.
 * Strips trailing \r\n.  Returns nil at EOF.
 * Caller must NOT free the returned pointer.
 */
char *
httpreadline(HTTPResp *r)
{
	char *line;
	int   n;

	line = Brdline(r->conn->bio, '\n');
	if(line == nil)
		return nil;
	n = Blinelen(r->conn->bio);
	if(n >= 2 && line[n-2] == '\r')
		line[n-2] = '\0';
	else
		line[n-1] = '\0';
	return line;
}

/*
 * httprespfree — free a response.  Does not close the connection.
 */
void
httprespfree(HTTPResp *r)
{
	if(r == nil)
		return;
	free(r->body);
	free(r);
}
