/*
 * acmeevent.c — acme event file parser
 */

#include <u.h>
#include <libc.h>
#include "acmeevent.h"

/* ── internal helpers ── */

static int
evgetc(int fd, char *buf, int *bufp, int *nbuf)
{
	if(*bufp >= *nbuf) {
		*nbuf = read(fd, buf, ACMEEVENT_BUFSZ);
		*bufp = 0;
		if(*nbuf <= 0)
			return -1;
	}
	return (uchar)buf[(*bufp)++];
}

static int
evgetn(int fd, char *buf, int *bufp, int *nbuf)
{
	int c, n = 0;
	while((c = evgetc(fd, buf, bufp, nbuf)) >= '0' && c <= '9')
		n = n * 10 + c - '0';
	if(c != ' ')
		return -1;
	return n;
}

static Rune
evgetrune(int fd, char *buf, int *bufp, int *nbuf, char *dst, int *dlen, int dsz)
{
	Rune r;
	int  c, nb;

	if(*dlen >= dsz - 4)
		return -1;

	c = evgetc(fd, buf, bufp, nbuf);
	if(c < 0)
		return -1;
	dst[*dlen] = c;
	nb = 1;
	if(c >= Runeself) {
		while(!fullrune(dst + *dlen, nb)) {
			c = evgetc(fd, buf, bufp, nbuf);
			if(c < 0)
				return -1;
			dst[*dlen + nb++] = c;
		}
	}
	chartorune(&r, dst + *dlen);
	*dlen += nb;
	return r;
}

/* ── getevent ── */

int
getevent(int fd, char *buf, int *bufp, int *nbuf, AcmeEvent *e)
{
	int  i, c;
	char tbuf[512];
	int  tlen;

	e->c1 = evgetc(fd, buf, bufp, nbuf);
	if(e->c1 < 0)
		return 0;
	e->c2 = evgetc(fd, buf, bufp, nbuf);
	if(e->c2 < 0)
		return 0;

	e->q0   = evgetn(fd, buf, bufp, nbuf);
	e->q1   = evgetn(fd, buf, bufp, nbuf);
	e->flag = evgetn(fd, buf, bufp, nbuf);
	e->nr   = evgetn(fd, buf, bufp, nbuf);

	if(e->q0 < 0 || e->q1 < 0 || e->flag < 0 || e->nr < 0)
		return 0;

	e->eq0 = e->q0;
	e->eq1 = e->q1;

	if(e->nr > 256)
		e->nr = 256;

	tlen = 0;
	for(i = 0; i < e->nr; i++) {
		if(evgetrune(fd, buf, bufp, nbuf, e->text, &tlen,
		             sizeof e->text) < 0)
			return 0;
	}
	e->text[tlen] = '\0';

	/* consume trailing newline */
	c = evgetc(fd, buf, bufp, nbuf);
	if(c != '\n')
		return 0;

	/* flag & 2: expansion event follows */
	if(e->flag & 2) {
		int xq0, xq1, xflag, xnr;
		evgetc(fd, buf, bufp, nbuf);  /* c1 */
		evgetc(fd, buf, bufp, nbuf);  /* c2 */
		xq0   = evgetn(fd, buf, bufp, nbuf);
		xq1   = evgetn(fd, buf, bufp, nbuf);
		xflag = evgetn(fd, buf, bufp, nbuf);
		xnr   = evgetn(fd, buf, bufp, nbuf);
		e->eq0 = xq0;
		e->eq1 = xq1;
		USED(xflag);
		tlen = 0;
		if(xnr > 256) xnr = 256;
		for(i = 0; i < xnr; i++)
			evgetrune(fd, buf, bufp, nbuf, e->text, &tlen, sizeof e->text);
		e->text[tlen] = '\0';
		evgetc(fd, buf, bufp, nbuf);   /* newline */
	}

	/* flag & 8: chorded argument — read and discard two follow-up events */
	if(e->flag & 8) {
		int pass, nr2;
		for(pass = 0; pass < 2; pass++) {
			evgetc(fd, buf, bufp, nbuf);  /* c1 */
			evgetc(fd, buf, bufp, nbuf);  /* c2 */
			evgetn(fd, buf, bufp, nbuf);  /* q0 */
			evgetn(fd, buf, bufp, nbuf);  /* q1 */
			evgetn(fd, buf, bufp, nbuf);  /* flag */
			nr2 = evgetn(fd, buf, bufp, nbuf);
			if(nr2 > 256) nr2 = 256;
			tlen = 0;
			for(i = 0; i < nr2; i++)
				evgetrune(fd, buf, bufp, nbuf, tbuf, &tlen, sizeof tbuf);
			evgetc(fd, buf, bufp, nbuf);  /* newline */
		}
	}

	return 1;
}

/* ── writeevent ── */

void
writeevent(int evfd, AcmeEvent *e)
{
	char buf[32];
	int  n;
	n = snprint(buf, sizeof buf, "%c%c%d %d\n", e->c1, e->c2, e->q0, e->q1);
	write(evfd, buf, n);
}

/* ── predicates ── */
int
isuuid(char *text)
{
	return strlen(text) == 36 &&
	       text[8]  == '-' &&
	       text[13] == '-' &&
	       text[18] == '-' &&
	       text[23] == '-';
}

int
ismodelid(char *text)
{
	char *p;
	if(*text == '\0')
		return 0;
	for(p = text; *p; p++)
		if(*p == ' ' || *p == '#' || *p == '\t')
			return 0;
	return 1;
}
