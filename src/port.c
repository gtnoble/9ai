/*
 * port.c — platform portability helpers
 *
 * Abstracts the small differences between native Plan 9 and plan9port
 * (Linux/macOS) so the rest of 9ai need not ifdef anything.
 *
 * Conventions:
 *   - Plan 9: home is $home, config lives in ~/lib/9ai/
 *   - plan9port on Unix: home is $HOME, config lives in ~/lib/9ai/
 *     (Plan 9 convention kept deliberately; not ~/.config/9ai)
 *
 * All returned strings are smprint-allocated; caller must free.
 */

#include <u.h>
#include <libc.h>

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
