/*
 * record.h — RS/FS record parser
 */

/* RS byte (record separator) and FS byte (field separator) */
#define RS 0x1e
#define FS 0x1f

/*
 * splitrec — split a RS-terminated record into FS-separated fields.
 * Modifies rec in place (NUL-patches field boundaries).
 * Returns the number of fields placed in fields[].
 */
int splitrec(char *rec, int reclen, char **fields, int maxfields);
