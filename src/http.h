/*
 * http.h — HTTP/1.1 client interface for 9ai
 *
 * One HTTPConn = one TLS connection = one request/response.
 * After httprespfree(), call httpclose() and tlsdial() again for the
 * next request.
 *
 * Usage:
 *
 *   HTTPConn *c = tlsdial("api.individual.githubcopilot.com", "443");
 *   if(c == nil) sysfatal("tlsdial: %r");
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
 *   HTTPConn *c = tlsdial(host, "443");
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
	int      fd;    /* TLS file descriptor */
	char    *host;  /* strdup of host name */
	Biobuf  *bio;   /* single read Biobuf, lives for the connection */
};

struct HTTPResp {
	int       code;
	char     *body;     /* nil until httpreadbody() */
	long      bodylen;
	int       chunked;  /* 1 if Transfer-Encoding: chunked */
	HTTPConn *conn;     /* back-pointer; response does not own conn */
};

HTTPConn *tlsdial(char *host, char *port);
void      httpclose(HTTPConn *c);
HTTPResp *httpget(HTTPConn *c, char *path, char *host,
                  HTTPHdr *hdrs, int nhdrs);
HTTPResp *httppost(HTTPConn *c, char *path, char *host,
                   HTTPHdr *hdrs, int nhdrs, char *body, long bodylen);
int       httpreadbody(HTTPResp *r);
char     *httpreadline(HTTPResp *r);
void      httprespfree(HTTPResp *r);
