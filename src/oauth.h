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
 * OAuthDeviceCode — in-progress device-code flow state.
 * Returned by oauthdevicestart(); passed to oauthdevicepoll().
 * Free with oauthdevcodefree() when done.
 */
typedef struct OAuthDeviceCode OAuthDeviceCode;
struct OAuthDeviceCode {
	char device_code[256];
	char user_code[64];
	char verification_uri[256];
	long interval;
	long expires_in;
};

/*
 * oauthdevicestart — start the device-code flow, return codes for display.
 *
 * sockpath — path to the 9aitls Unix socket
 *
 * Returns a heap-allocated OAuthDeviceCode on success, nil on error.
 * The caller displays dc->verification_uri and dc->user_code, then
 * calls oauthdevicepoll() in a loop to wait for the user to authorise.
 */
OAuthDeviceCode *oauthdevicestart(char *sockpath);

/*
 * oauthdevicepoll — poll once for the access token.
 *
 * dc       — the OAuthDeviceCode from oauthdevicestart()
 * sockpath — path to the 9aitls Unix socket
 * tokpath  — where to persist the token on success
 * done     — set to 1 if flow completed (success or terminal error);
 *             set to 0 if still pending (caller should sleep dc->interval)
 *
 * Returns 0 if the token was obtained and written to tokpath.
 * Returns -1 on terminal error (sets errstr).
 * When done=0 and return value is 0, the caller should sleep and retry.
 * When done=1, the return value indicates success (0) or failure (-1).
 */
int oauthdevicepoll(OAuthDeviceCode *dc, char *sockpath, char *tokpath, int *done);

/*
 * oauthenablemodels — POST /models/<id>/policy {"state":"enabled"} for
 * every model returned by the Copilot /models endpoint.
 *
 * Required after first login to unlock Claude and Grok models.
 * Errors from individual POSTs are silently ignored.
 *
 * session  — Copilot session token (Bearer)
 * sockpath — path to the 9aitls Unix socket
 */
void oauthenablemodels(char *session, char *sockpath);

/*
 * oauthtokenexists — return 1 if tokpath exists and is non-empty, 0 otherwise.
 */
int oauthtokenexists(char *tokpath);

/*
 * oauthdevcodefree — free an OAuthDeviceCode.
 */
void oauthdevcodefree(OAuthDeviceCode *dc);

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
