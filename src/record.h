/*
 * record.h — RS/FS/ESC record codec for 9ai
 *
 * Every record is:  field₀ FS field₁ … FS fieldₙ RS
 *
 *   FS  = 0x1F  (field separator)
 *   RS  = 0x1E  (record separator / terminator)
 *   ESC = 0x1B  (escape: next byte is literal within a field value)
 *
 * ESC-encoding: 0x1B, 0x1F, or 0x1E within a field value are written as
 * ESC followed by the byte.  A bare FS always means field boundary; a bare
 * RS always means record end.  Empty fields are representable as two
 * consecutive bare FS bytes (or a leading/trailing bare FS).
 */

#define RS  0x1e
#define FS  0x1f
#define ESC 0x1b

/* String literal versions for use in format strings / smprint calls */
#define AIRS "\x1e"
#define AIFS "\x1f"
#define AESC "\x1b"

/*
 * splitrec — split a RS-terminated record into ESC-decoded FS-separated fields.
 *
 * Decodes ESC-encoding in place (decoded length ≤ encoded length) and
 * NUL-terminates each field.  fields[] entries point into rec.
 * rec must be writable.
 *
 * Returns the number of fields extracted.
 */
int splitrec(char *rec, int reclen, char **fields, int maxfields);

/*
 * fmtrecfields — ESC-encode a char*[] field list into a heap-allocated record.
 *
 * Returns a malloc'd buffer: field₀ FS … FS fieldₙ RS NUL
 * *lenp is set to the number of bytes including the trailing RS (before NUL).
 * Returns nil on allocation failure.
 */
char *fmtrecfields(char **fields, int nfields, long *lenp);

/*
 * fmtrec — varargs wrapper: nil-terminated list of char* fields.
 * Returns a malloc'd record buffer, or nil on failure.
 */
char *fmtrec(long *lenp, ...);
