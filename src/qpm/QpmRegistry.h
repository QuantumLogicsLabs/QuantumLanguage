#pragma once
// Registry location + credentials shared by install, publish and login.
// The registry defaults to the local QPM backend and can be overridden with
// the QPM_REGISTRY environment variable. `qpm login` stores one token per
// registry URL in ~/.qpmrc (JSON), like npm's per-registry _authToken.

#include <string>

namespace qpm
{

    // Registry API base, always ending in '/' (e.g. "http://localhost:8000/api/registry/").
    std::string registryBaseUrl();

    // Token for the current registry: the QPM_TOKEN environment variable if
    // set, else the one `qpm login` saved, else "".
    std::string authToken();

    // Asks the registry who `token` belongs to. Returns false and fills
    // `error` if the token is rejected or the registry can't be reached.
    bool lookupUser(const std::string &token, std::string &username, std::string &error);

    // `qpm login` / `qpm logout` / `qpm whoami`. Return a process exit code.
    int runLogin();
    int runLogout();
    int runWhoami();

} // namespace qpm
