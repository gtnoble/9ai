/*
 * skill.c — skill listing for 9ai
 *
 * See skill.h for the interface.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>

#include "9ai.h"
#include "skill.h"

char *
skillsdir(void)
{
	return configpath("skills/");
}

/*
 * firstline — read the first line of a file.
 *
 * Opens path, reads up to the first newline or maxlen-1 bytes, returns
 * a malloc'd NUL-terminated string with the newline stripped.
 * Returns nil on error or empty file.
 */
static char *
firstline(char *path, int maxlen)
{
	int   fd;
	char *buf;
	int   n, i;

	buf = malloc(maxlen);
	if(buf == nil)
		return nil;

	fd = open(path, OREAD);
	if(fd < 0) {
		free(buf);
		return nil;
	}
	n = read(fd, buf, maxlen - 1);
	close(fd);

	if(n <= 0) {
		free(buf);
		return nil;
	}
	buf[n] = '\0';

	/* truncate at first newline */
	for(i = 0; i < n; i++) {
		if(buf[i] == '\n') {
			buf[i] = '\0';
			break;
		}
	}

	if(buf[0] == '\0') {
		free(buf);
		return nil;
	}
	return buf;
}

char *
skilllist(void)
{
	char  *dir;
	int    fd;
	Dir   *dbuf;
	long   nd, i;
	char  *out;
	long   outcap, outlen;

	dir = skillsdir();
	if(dir == nil)
		return nil;

	fd = open(dir, OREAD);
	if(fd < 0) {
		free(dir);
		return nil;
	}

	outcap = 1024;
	outlen = 0;
	out    = malloc(outcap);
	if(out == nil) {
		close(fd);
		free(dir);
		return nil;
	}
	out[0] = '\0';

	/*
	 * dirreadall reads all directory entries at once into a
	 * single malloc'd buffer; returns count or -1.
	 */
	nd = dirreadall(fd, &dbuf);
	close(fd);

	if(nd < 0) {
		free(dir);
		free(out);
		return nil;
	}

	for(i = 0; i < nd; i++) {
		char *path, *desc, *line;
		long  addlen;

		/* skip directories and dot-files */
		if(dbuf[i].qid.type & QTDIR)
			continue;
		if(dbuf[i].name[0] == '.')
			continue;

		path = smprint("%s%s", dir, dbuf[i].name);
		if(path == nil)
			continue;

		desc = firstline(path, 512);
		free(path);
		if(desc == nil)
			continue;

		/* format: "<name>\t<desc>\n" */
		line = smprint("%s\t%s\n", dbuf[i].name, desc);
		free(desc);
		if(line == nil)
			continue;

		addlen = strlen(line);
		if(outlen + addlen + 1 > outcap) {
			long newcap = outcap * 2 + addlen + 1;
			char *tmp = realloc(out, newcap);
			if(tmp == nil) {
				free(line);
				break;
			}
			out    = tmp;
			outcap = newcap;
		}
		memmove(out + outlen, line, addlen);
		outlen += addlen;
		out[outlen] = '\0';
		free(line);
	}

	free(dbuf);
	free(dir);
	return out;
}
