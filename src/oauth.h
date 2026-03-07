/*
 * oauth.h — GitHub Copilot OAuth device flow for 9ai
 *
 * Two-phase authentication:
 *
 * Phase 1: Device flow (github.com)
 *   POST /login/device/code  → device_code, user_code, verification_uri,
 *                              interval, expires_in
 *   User visits verification_uri and enters user_code.
 *   Poll POST /login/oauth/access_token until access_token appears.
 *   Persist access_token to ~/lib/9ai/token  (the "refresh token").
 *
 * Phase 2: Copilot session token (api.github.com)
 *   GET /copilot_internal/v2/token  with Bearer <refresh_token>
 *   → token (tid=...;exp=...;...), expires_at
 *   Use token as Bearer for all Copilot API calls.
 *   Refresh when expires_at approaches (refresh_in seconds hint).
 *
 * The C client only needs Phase 1 for login and Phase 2 before each
 * API request.  Phase 1 is interactive; Phase 2 is automatic.
 */

typedef struct OAuthToken OAuthToken;

/*
 * OAuthToken — a live Copilot session token.
 * All strings are smprint/strdup-allocated; free with oauthtokenfree().
 */
struct OAuthToken {
	char *token;      /* tid=...;exp=...;... — Bearer value for API */
	long  expires_at; /* unix timestamp; refresh before this */
	long  refresh_in; /* seconds until recommended refresh */
};

/*
 * oauthlogin — run the full device code flow interactively.
 *
 * Writes the obtained GitHub access token (refresh token) to tokpath.
 * Prints the verification URL and user code to stdout for the user.
 * Polls until authorised or expired.
 *
 * sockpath  — path to the 9aitls Unix socket
 * tokpath   — destination file for the token (e.g. ~/lib/9ai/token)
 *
 * Returns 0 on success, -1 on error (sets errstr).
 */
int oauthlogin(char *sockpath, char *tokpath);

/*
 * oauthsession — exchange a refresh token for a Copilot session token.
 *
 * refresh   — the GitHub access token (ghu_...)
 * sockpath  — path to the 9aitls Unix socket
 *
 * Returns a heap-allocated OAuthToken on success, nil on error.
 */
OAuthToken *oauthsession(char *refresh, char *sockpath);

/*
 * oauthtokenfree — release an OAuthToken.
 */
void oauthtokenfree(OAuthToken *t);
