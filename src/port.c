/*
 * port.c — platform portability helpers
 *
 * Abstracts the small differences between native Plan 9 / 9front and
 * plan9port (Linux/macOS) so the rest of 9ai need not ifdef anything.
 *
 * Conventions:
 *   - Plan 9 / 9front: home is $home, config lives in ~/lib/9ai/
 *   - plan9port on Unix: home is $HOME, config lives in ~/lib/9ai/
 *     (Plan 9 convention kept deliberately; not ~/.config/9ai)
 *
 * Platform detection:
 *   plan9port defines PLAN9PORT=1 via its build system (we add
 *   -DPLAN9PORT to CFLAGS in the plan9port mkfile).
 *   9front does not define it.
 *
 * All returned strings are smprint/strdup-allocated; caller must free.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <libsec.h>

#include "9ai.h"
#include "http.h"

/*
 * homedir — return the user's home directory path.
 *
 * Checks $home (Plan 9 native) then $HOME (Unix).
 * Falls back to /usr/<user> which is correct on native Plan 9.
 * The returned string is a private copy; free with free().
 */
char *
homedir(void)
{
	char *h;

	if((h = getenv("home")) != nil && h[0] != '\0')
		return h;
	free(h);
	if((h = getenv("HOME")) != nil && h[0] != '\0')
		return h;
	free(h);
	return smprint("/usr/%s", getuser());
}

/*
 * configpath — return ~/lib/9ai/<name>.
 *
 * Examples:
 *   configpath("token")        → /home/alice/lib/9ai/token
 *   configpath("sessions/")    → /home/alice/lib/9ai/sessions/
 *
 * The returned string is smprint-allocated; caller must free().
 */
char *
configpath(char *name)
{
	char *h, *p;

	h = homedir();
	p = smprint("%s/lib/9ai/%s", h, name);
	free(h);
	return p;
}

/*
 * portdial — dial a TLS connection to host:port.
 *
 * plan9port: connects to the 9aitls proxy at sockpath (unix socket).
 *            host and port are ignored — the proxy routes on Host header.
 * 9front:    dials tcp!host!port directly and wraps with tlsClient().
 *            sockpath is ignored.
 *
 * Returns a new HTTPConn on success, nil on failure (werrstr set).
 */
HTTPConn *
portdial(char *host, char *port, char *sockpath)
{
#ifdef PLAN9PORT
	USED(host); USED(port);
	return httpdial(sockpath);
#else
	HTTPConn *c;
	TLSconn   conn;
	char      addr[256];
	int       fd;

	USED(sockpath);

	snprint(addr, sizeof addr, "tcp!%s!%s", host, port);
	fd = dial(addr, nil, nil, nil);
	if(fd < 0) {
		werrstr("portdial: dial %s: %r", addr);
		return nil;
	}

	memset(&conn, 0, sizeof conn);
	conn.serverName = smprint("%s", host);
	fd = tlsClient(fd, &conn);
	if(fd < 0) {
		werrstr("portdial: tlsClient %s: %r", host);
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
#endif
}
