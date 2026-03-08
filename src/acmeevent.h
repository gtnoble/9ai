/*
 * acmeevent.h — acme event file parser and helpers
 */

typedef struct AcmeEvent AcmeEvent;
struct AcmeEvent {
	int  c1, c2;
	int  q0, q1;
	int  eq0, eq1;  /* expanded range (from flag & 2 follow-up) */
	int  flag, nr;
	char text[512];
};

/*
 * getevent — read one event from fd (an open acme event file).
 * buf/bufp/nbuf are a caller-managed read buffer of size ACMEEVENT_BUFSZ.
 * Returns 1 on success, 0 on EOF/error.
 */
enum { ACMEEVENT_BUFSZ = 8192 };
int getevent(int fd, char *buf, int *bufp, int *nbuf, AcmeEvent *e);

/*
 * writeevent — write an event back to acme so it handles it natively.
 */
void writeevent(int evfd, AcmeEvent *e);

/*
 * isuuid — true if text is a well-formed UUID (8-4-4-4-12).
 */
int isuuid(char *text);

/*
 * ismodelid — true if text looks like a model id (no spaces, tabs, or '#').
 */
int ismodelid(char *text);
