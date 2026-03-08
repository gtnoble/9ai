/*
 * sessfile.h — session file parser for 9ai-acme Sessions window
 */

enum { SESS_SNIPPET = 80 };

/*
 * parsesessfile — read the first session and prompt records from a
 * session file at path.
 *
 * On success, populates uuid[37], model[64], ts[32], snippet[SESS_SNIPPET+4]
 * and returns 0.  Returns -1 if the file cannot be opened or contains no
 * valid session record.
 */
int parsesessfile(char *path, char *uuid, char *model, char *ts, char *snippet);
