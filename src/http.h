/*
 * http.h — HTTP/1.1 client interface for 9ai
 *
 * 9ai never speaks TLS directly.  All connections are made to 9aitls,
 * a local proxy that accepts plain HTTP on a Unix domain socket and
 * forwards to the upstream API over TLS.
 *
 * One HTTPConn = one Unix socket connection = one request/response.
 * After httprespfree(), call httpclose() and httpdial() again for the
 * next request.
 *
 * Usage:
 *
 *   HTTPConn *c = httpdial("/home/user/lib/9ai/proxy.sock");
 *   if(c == nil) sysfatal("dial: %r");
 *
 *   HTTPResp *r = httpget(c, "/models", "api.individual.githubcopilot.com",
 *                         hdrs, nhdrs);
 *   if(r == nil || r->code != 200) { ... }
 *   httpreadbody(r);
 *   print("%s\n", r->body);
 *   httprespfree(r);
 *   httpclose(c);
 *
 *   // Streaming SSE:
 *   HTTPConn *c = httpdial(sockpath);
 *   HTTPResp *r = httppost(c, "/chat/completions", host, hdrs, nhdrs,
 *                          body, bodylen);
 *   char *line;
 *   while((line = httpreadline(r)) != nil) { ... }
 *   httprespfree(r);
 *   httpclose(c);
 */

typedef struct HTTPConn  HTTPConn;
typedef struct HTTPResp  HTTPResp;
typedef struct HTTPHdr   HTTPHdr;

struct HTTPHdr {
	char *name;
	char *value;
};

struct HTTPConn {
	int      fd;    /* Unix socket file descriptor */
	char    *host;  /* strdup of sockpath */
	Biobuf  *bio;   /* single read Biobuf, lives for the connection */
};

struct HTTPResp {
	int       code;
	char     *body;     /* nil until httpreadbody() */
	long      bodylen;
	HTTPConn *conn;     /* back-pointer; response does not own conn */
};

/* http.c */
HTTPConn *httpdial(char *sockpath);
void      httpclose(HTTPConn *c);
HTTPResp *httpget(HTTPConn *c, char *path, char *host,
                  HTTPHdr *hdrs, int nhdrs);
HTTPResp *httppost(HTTPConn *c, char *path, char *host,
                   HTTPHdr *hdrs, int nhdrs, char *body, long bodylen);
int       httpreadbody(HTTPResp *r);
char     *httpreadline(HTTPResp *r);
void      httprespfree(HTTPResp *r);
