/*
 * skill.h — skill listing for 9ai
 *
 * Skills live in ~/lib/9ai/skills/.  Each skill is a plain text file:
 *   - Line 1:    description (shown to the model in the system prompt)
 *   - Lines 2+:  skill body (loaded by the agent on demand via exec cat)
 *
 * The agent is told the skill directory path and the name+description of
 * each available skill.  It can load a skill body itself using the exec
 * tool (e.g. cat ~/lib/9ai/skills/<name>).
 */

/*
 * skilllist — build a listing of all available skills.
 *
 * Reads ~/lib/9ai/skills/ and, for each regular file, reads the first
 * line as the description.
 *
 * Returns a malloc'd string of the form:
 *
 *   <name>\t<description>\n
 *   ...
 *
 * Returns nil if the skills directory does not exist or cannot be read
 * (not an error — the caller omits the skills section from the system
 * prompt).  Returns an empty string "" if the directory exists but
 * contains no skill files.
 *
 * Caller must free the returned string.
 */
char *skilllist(void);

/*
 * skillsdir — return the path to the skills directory.
 * Returns a malloc'd string; caller must free.
 */
char *skillsdir(void);
