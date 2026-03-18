/*
 * port.c — platform helpers for 9front
 *
 * homedir()    — return the user's home directory path.
 * configpath() — return ~/lib/9ai/<name>.
 *
 * All returned strings are smprint-allocated; caller must free.
 */

#include <u.h>
#include <libc.h>

#include "9ai.h"

/*
 * homedir — return the user's home directory path.
 *
 * Checks $home (Plan 9 convention).
 * Falls back to /usr/<user>.
 */
char *
homedir(void)
{
	char *h;

	if((h = getenv("home")) != nil && h[0] != '\0')
		return h;
	free(h);
	return smprint("/usr/%s", getuser());
}

/*
 * configpath — return ~/lib/9ai/<name>.
 *
 * Examples:
 *   configpath("token")      → /usr/alice/lib/9ai/token
 *   configpath("sessions/")  → /usr/alice/lib/9ai/sessions/
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
