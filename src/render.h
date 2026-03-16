/*
 * render.h — format 9ai event records for display in the acme window
 */

/*
 * render_tool_start — format a tool_start record.
 *
 * fields[0] = "tool_start"
 * fields[1] = tool name
 * fields[2] = tool id (unused in display)
 * fields[3..nf-3] = argv
 * fields[nf-2] = stdin (empty string if none)
 * fields[nf-1] = timeout in seconds as a decimal string, or "" for default
 *
 * Old records without the timeout field (nf <= original layout) are
 * handled gracefully — the timeout annotation is simply omitted.
 *
 * Displays the full command (with timeout annotation when non-default),
 * followed by stdin (if non-empty).
 * Returns a malloc'd string, or nil on error.
 */
char *render_tool_start(char **fields, int nf);

/*
 * render_tool_end — format a tool_end record.
 *
 * fields[0] = "tool_end"
 * fields[1] = "ok" or "err"
 * fields[2] = error text (if err)
 *
 * Returns a malloc'd string, or nil on error.
 */
char *render_tool_end(char **fields, int nf);

/*
 * render_thinking — format a thinking chunk for display.
 * Each line is prefixed with "│ " (U+2502 BOX DRAWINGS LIGHT VERTICAL).
 *
 * Returns a malloc'd string, or nil on error.
 */
char *render_thinking(char *chunk);
