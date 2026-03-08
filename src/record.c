/*
 * record.c — RS/FS record parser for 9ai event format
 */

#include <u.h>
#include <libc.h>
#include "record.h"

/*
 * splitrec — split a RS-terminated record (0x1E) into FS-separated
 * (0x1F) fields.
 *
 * Returns number of fields in fields[]; each is a nul-terminated
 * string pointing into rec (fields are NUL-patched in place).
 *
 * The record must be writable.
 */
int
splitrec(char *rec, int reclen, char **fields, int maxfields)
{
	char *p, *end, *start;
	int   nf = 0;

	end = rec + reclen;
	/* strip trailing RS */
	if(end > rec && (uchar)*(end-1) == 0x1e)
		end--;

	p = rec;
	while(p < end && nf < maxfields) {
		start = p;
		while(p < end && (uchar)*p != 0x1f)
			p++;
		/* NUL-terminate the field in place */
		if(p < end)
			*p++ = '\0';
		else
			*p = '\0';
		fields[nf++] = start;
	}
	return nf;
}
