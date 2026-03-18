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
 */

typedef struct OAuthToken OAuthToken;

struct OAuthToken {
	char *token;      /* tid=...;exp=...;... — Bearer value for API */
	long  expires_at; /* unix timestamp; refresh before this */
	long  refresh_in; /* seconds until recommended refresh */
};

/*
 * oauthlogin — run the full device code flow interactively.
 *
 * Prints the verification URL and user code, polls until authorised,
 * and writes the GitHub access token to tokpath.
 *
 * Returns 0 on success, -1 on error (sets errstr).
 */
int oauthlogin(char *tokpath);

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
 * Returns a heap-allocated OAuthDeviceCode on success, nil on error.
 * Caller displays dc->verification_uri and dc->user_code, then calls
 * oauthdevicepoll() in a loop.
 */
OAuthDeviceCode *oauthdevicestart(void);

/*
 * oauthdevicepoll — poll once for the access token.
 *
 * dc      — the OAuthDeviceCode from oauthdevicestart()
 * tokpath — where to persist the token on success
 * done    — set to 1 if flow completed (success or terminal error)
 *
 * Returns 0 on success or still-pending; -1 on terminal error.
 */
int oauthdevicepoll(OAuthDeviceCode *dc, char *tokpath, int *done);

/*
 * oauthenablemodels — POST /models/<id>/policy {"state":"enabled"} for
 * every model returned by the Copilot /models endpoint.
 *
 * Required after first login to unlock Claude and Grok models.
 * Errors are silently ignored.
 *
 * session — Copilot session token (Bearer)
 */
void oauthenablemodels(char *session);

/*
 * oauthtokenexists — return 1 if tokpath exists and is non-empty.
 */
int oauthtokenexists(char *tokpath);

/*
 * oauthdevcodefree — free an OAuthDeviceCode.
 */
void oauthdevcodefree(OAuthDeviceCode *dc);

/*
 * oauthsession — exchange a refresh token for a Copilot session token.
 *
 * refresh — the GitHub access token (ghu_...)
 *
 * Returns a heap-allocated OAuthToken on success, nil on error.
 */
OAuthToken *oauthsession(char *refresh);

/*
 * oauthtokenfree — release an OAuthToken.
 */
void oauthtokenfree(OAuthToken *t);
