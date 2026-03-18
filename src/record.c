/*
 * record.c — RS/FS/ESC record codec for 9ai
 *
 * See record.h for the format description.
 */

#include <u.h>
#include <libc.h>
#include "record.h"

/*
 * splitrec — split a RS-terminated record into ESC-decoded FS-separated fields.
 *
 * ESC (0x1B) followed by any byte yields that byte literally.
 * A bare FS (0x1F) is a field boundary; a bare RS (0x1E) is the record end.
 * Decodes in place: since ESC-encoding never expands data, the decoded field
 * is always ≤ the encoded length, so we write back into the same buffer.
 *
 * Returns the number of fields placed in fields[].
 * rec must be writable; fields[] entries point into rec after decoding.
 */
int
splitrec(char *rec, int reclen, char **fields, int maxfields)
{
	char *p, *end;
	int   nf = 0;

	end = rec + reclen;
	/* strip trailing RS */
	if(end > rec && (uchar)*(end-1) == RS)
		end--;

	p = rec;
	while(p <= end && nf < maxfields) {
		char *field_start = p;
		char *out         = p;

		while(p < end) {
			uchar c = (uchar)*p;
			if(c == ESC) {
				/* escape: emit the next byte literally */
				p++;
				if(p < end)
					*out++ = *p++;
			} else if(c == FS) {
				/* bare FS: field boundary */
				p++;
				break;
			} else if(c == RS) {
				/* bare RS: record end (shouldn't appear after strip) */
				p++;
				break;
			} else {
				*out++ = *p++;
			}
		}
		*out = '\0';
		fields[nf++] = field_start;

		if(p >= end)
			break;
	}
	return nf;
}

/*
 * fmtrecfields — ESC-encode a char*[] field list into a heap-allocated record.
 *
 * Format: field₀ FS field₁ FS … FS fieldₙ RS NUL
 * Bytes 0x1B, 0x1F, 0x1E within a field value are prefixed with ESC (0x1B).
 *
 * Returns a malloc'd buffer; *lenp = byte count including RS, before NUL.
 * Returns nil on allocation failure.
 */
char *
fmtrecfields(char **fields, int nfields, long *lenp)
{
	long       total = 0;
	long       wpos  = 0;
	int        i;
	char      *buf;
	const char *p;

	/* measure: count encoded bytes for each field */
	for(i = 0; i < nfields; i++) {
		if(i > 0) total++;          /* FS separator */
		for(p = fields[i]; *p != '\0'; p++) {
			uchar c = (uchar)*p;
			if(c == ESC || c == FS || c == RS)
				total++;    /* ESC prefix */
			total++;
		}
	}
	total++;  /* RS terminator */

	buf = malloc(total + 1);
	if(buf == nil)
		return nil;

	/* write */
	for(i = 0; i < nfields; i++) {
		if(i > 0) buf[wpos++] = FS;
		for(p = fields[i]; *p != '\0'; p++) {
			uchar c = (uchar)*p;
			if(c == ESC || c == FS || c == RS)
				buf[wpos++] = ESC;
			buf[wpos++] = (char)c;
		}
	}
	buf[wpos++] = RS;
	buf[wpos]   = '\0';

	*lenp = wpos;
	return buf;
}

/*
 * fmtrec — varargs wrapper for fmtrecfields.
 *
 * Takes a nil-terminated list of char* fields.
 * Returns a malloc'd record buffer, or nil on failure.
 */
char *
fmtrec(long *lenp, ...)
{
	va_list ap;
	char   *tmp[64];
	int     n = 0;
	char   *f;

	va_start(ap, lenp);
	while((f = va_arg(ap, char *)) != nil && n < 64)
		tmp[n++] = f;
	va_end(ap);

	return fmtrecfields(tmp, n, lenp);
}
