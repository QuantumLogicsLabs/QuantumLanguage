#pragma once
// Registry location + credentials shared by install, publish, search and login.
// The registry defaults to the public QPM registry (the API behind
// https://qpm.quantumlogicslimited.com) and can be overridden with `--registry
// <url>` or the QPM_REGISTRY environment variable. `qpm login` stores one token
// per registry URL in ~/.qpmrc (JSON), like npm's per-registry _authToken.

#include <string>

namespace qpm
{

    // The registry every command uses when nothing overrides it.
    extern const char *const kDefaultRegistry;

    // `--registry <url>` from the command line; wins over QPM_REGISTRY.
    void setRegistryOverride(const std::string &url);

    // Registry API base, always ending in '/' (e.g. "https://qpm.quantumlogicslimited.com/api/registry/").
    std::string registryBaseUrl();

    // Where `qpm install` looks up a package the registry answers 404 for:
    // QPM_UPSTREAM if set, else the npm registry. "" when QPM_UPSTREAM=none.
    std::string upstreamRegistryUrl();

    // True for a QPM backend (an ".../api/registry/" URL) rather than an
    // npm-style registry such as registry.npmjs.org. The two answer search
    // and publish requests differently.
    bool isQpmRegistry(const std::string &registry);

    // The web page that shows `name` for packages from `registry`
    // (https://qpm.quantumlogicslimited.com/package/<name>, npmjs.com for npm),
    // or "" when the registry has no known website.
    std::string packagePageUrl(const std::string &registry, const std::string &name);

    // Token for the current registry: the QPM_TOKEN environment variable if
    // set, else the one `qpm login` saved, else "".
    std::string authToken();

    // Asks the registry who `token` belongs to. Returns false and fills
    // `error` if the token is rejected or the registry can't be reached.
    bool lookupUser(const std::string &token, std::string &username, std::string &error);

    // When `token` (a JWT) expires, in seconds since 1970; 0 if it never does
    // (the website's API tokens) or isn't a JWT.
    double tokenExpiry(const std::string &token);

    struct LoginOptions
    {
        std::string token;     // `qpm login <token>` skips the question
        bool password = false; // `qpm login --password`: username and password instead of a token
    };

    // `qpm login` / `qpm logout` / `qpm whoami`. Return a process exit code.
    int runLogin(const LoginOptions &opts);
    int runLogout();
    int runWhoami();

    // `qpm auth`: which account (website profile) qpm is logged in as, whether
    // its token still works, what kind it is and where it's stored. Exits 1
    // when not logged in or the token doesn't work.
    int runAuthStatus();

} // namespace qpm
