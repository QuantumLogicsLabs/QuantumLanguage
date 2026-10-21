#include "QpmRegistry.h"
#include "QpmColor.h"
#include "QpmHttp.h"
#include "QpmJson.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <conio.h>
#include <shellapi.h>
#endif

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace qpm
{
    const char *const kDefaultRegistry = "https://qpm.quantumlogicslimited.com/api/registry/";

    namespace
    {
        std::string g_registryOverride;

        std::string envOr(const char *name, const std::string &def)
        {
            const char *v = std::getenv(name);
            return (v && *v) ? std::string(v) : def;
        }

        fs::path rcPath()
        {
            std::string home = envOr("USERPROFILE", envOr("HOME", "."));
            return fs::path(home) / ".qpmrc";
        }

        JsonValue loadRc()
        {
            std::ifstream f(rcPath(), std::ios::binary);
            if (!f)
                return JsonValue::makeObject();
            std::ostringstream ss;
            ss << f.rdbuf();
            try
            {
                JsonValue rc = JsonValue::parse(ss.str());
                if (rc.isObject())
                    return rc;
            }
            catch (...)
            {
            }
            return JsonValue::makeObject();
        }

        bool saveRc(const JsonValue &rc)
        {
            std::ofstream f(rcPath(), std::ios::binary | std::ios::trunc);
            if (!f)
                return false;
            f << rc.stringify(2) << "\n";
            return static_cast<bool>(f);
        }

        void setSavedToken(const std::string &registry, const std::string &token)
        {
            JsonValue rc = loadRc();
            JsonValue tokens = rc.get("tokens");
            if (!tokens.isObject())
                tokens = JsonValue::makeObject();

            if (token.empty())
            {
                JsonValue kept = JsonValue::makeObject();
                for (const auto &m : tokens.object())
                    if (m.first != registry)
                        kept.set(m.first, m.second);
                tokens = kept;
            }
            else
            {
                tokens.set(registry, token);
            }
            rc.set("tokens", tokens);
            if (!saveRc(rc))
                std::cerr << "[qpm] warning: could not write " << rcPath().string() << "\n";
        }

        // The auth API is the registry's sibling: ".../api/registry/" -> ".../api/auth/".
        std::string authBaseUrl()
        {
            std::string base = registryBaseUrl();
            base.pop_back(); // trailing '/'
            size_t slash = base.find_last_of('/');
            return base.substr(0, slash + 1) + "auth/";
        }

        // One line from stdin without its line ending; false at the end of input.
        bool readLine(std::string &line)
        {
            if (!std::getline(std::cin, line))
                return false;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            return true;
        }

        bool stdinIsConsole()
        {
#ifdef _WIN32
            DWORD mode = 0;
            return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
#else
            return false;
#endif
        }

        // A password typed at the console, echoed as '*' so each keystroke shows
        // (Backspace works). Piped input is read as a plain line.
        bool readMasked(std::string &out)
        {
#ifdef _WIN32
            if (stdinIsConsole())
            {
                std::wstring typed;
                for (;;)
                {
                    wint_t c = _getwch();
                    if (c == L'\r' || c == L'\n')
                        break;
                    if (c == 3) // Ctrl+C
                    {
                        std::cout << "\n";
                        std::exit(130);
                    }
                    if (c == 0 || c == 0xE0) // arrow and function keys send a second code
                    {
                        _getwch();
                        continue;
                    }
                    if (c == L'\b')
                    {
                        if (!typed.empty())
                        {
                            typed.pop_back();
                            std::cout << "\b \b" << std::flush;
                        }
                        continue;
                    }
                    typed += static_cast<wchar_t>(c);
                    std::cout << '*' << std::flush;
                }
                std::cout << "\n";
                int len = WideCharToMultiByte(CP_UTF8, 0, typed.data(), static_cast<int>(typed.size()), nullptr, 0,
                                              nullptr, nullptr);
                out.assign(static_cast<size_t>(len), '\0');
                WideCharToMultiByte(CP_UTF8, 0, typed.data(), static_cast<int>(typed.size()), &out[0], len, nullptr,
                                    nullptr);
                return true;
            }
#endif
            return readLine(out);
        }

        // A pasted token without the spaces, quotes or "Bearer " that came along with it.
        std::string cleanToken(const std::string &raw)
        {
            std::string token;
            for (char c : raw)
                if (!std::isspace(static_cast<unsigned char>(c)) && c != '"' && c != '\'')
                    token += c;
            if (token.size() > 6 && (token.compare(0, 6, "Bearer") == 0 || token.compare(0, 6, "bearer") == 0))
                token.erase(0, 6);
            return token;
        }

        // The claims inside a JWT ("header.claims.signature"), or Null if `token` isn't one.
        JsonValue jwtClaims(const std::string &token)
        {
            size_t first = token.find('.');
            size_t second = first == std::string::npos ? std::string::npos : token.find('.', first + 1);
            if (second == std::string::npos)
                return JsonValue();
            std::string json; // base64url-decoded
            unsigned buffer = 0;
            int bits = 0;
            for (size_t i = first + 1; i < second; ++i)
            {
                char c = token[i];
                int v = c >= 'A' && c <= 'Z'   ? c - 'A'
                        : c >= 'a' && c <= 'z' ? c - 'a' + 26
                        : c >= '0' && c <= '9' ? c - '0' + 52
                        : c == '-' || c == '+' ? 62
                        : c == '_' || c == '/' ? 63
                                               : -1;
                if (v < 0)
                    break; // '=' padding
                buffer = (buffer << 6) | static_cast<unsigned>(v);
                bits += 6;
                if (bits >= 8)
                {
                    bits -= 8;
                    json += static_cast<char>((buffer >> bits) & 0xFF);
                }
            }
            try
            {
                JsonValue claims = JsonValue::parse(json);
                return claims.isObject() ? claims : JsonValue();
            }
            catch (...)
            {
                return JsonValue();
            }
        }

        // "expires in 6 days (2026-10-25)", or "expired on 2026-10-11".
        std::string expiryText(double exp)
        {
            std::time_t when = static_cast<std::time_t>(exp);
            char date[32] = "";
            if (std::tm *local = std::localtime(&when))
                std::strftime(date, sizeof date, "%Y-%m-%d", local);
            double left = exp - static_cast<double>(std::time(nullptr));
            if (left <= 0)
                return std::string("expired on ") + date;
            long long days = static_cast<long long>(left / 86400);
            if (days < 1)
                return std::string("expires within a day (") + date + ")";
            return "expires in " + std::to_string(days) + (days == 1 ? " day (" : " days (") + date + ")";
        }

        void openInBrowser(const std::string &url)
        {
#ifdef _WIN32
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
        }

        // "API token, never expires" / "sign-in token, expires in 6 days (2026-10-25)",
        // the expiry green, yellow or red by how much time is left.
        std::string tokenKindText(const std::string &token)
        {
            JsonValue claims = jwtClaims(token);
            if (claims.get("kind").asString() == "api")
                return "API token, " + paint("never expires", "32");
            double exp = claims.get("exp").asNumber(0);
            if (exp <= 0)
                return "token";
            bool expired = exp <= static_cast<double>(std::time(nullptr));
            return "sign-in token, " + paint(expiryText(exp), expired ? "1;31" : "33");
        }

        // The account `token` belongs to, from GET <auth>/me. On failure fills
        // `error`, and `rejected` says the registry turned the token down (as
        // opposed to not answering).
        bool fetchAccount(const std::string &token, JsonValue &user, std::string &error, bool &rejected)
        {
            HttpResponse resp = httpRequest("GET", authBaseUrl() + "me", "",
                                            {"Accept: application/json", "Authorization: Bearer " + token});
            if (!resp.ok())
            {
                error = describeFailure(resp);
                rejected = resp.error.empty() && (resp.status == 401 || resp.status == 403);
                return false;
            }
            try
            {
                user = JsonValue::parse(resp.body).get("user");
            }
            catch (...)
            {
            }
            return true;
        }

        // The site that lists a registry's packages: the registry's own origin,
        // except where the API is served from another host than the website.
        std::string websiteBaseUrl(const std::string &registry)
        {
            if (!isQpmRegistry(registry))
                return registry.find("//registry.npmjs.org/") != std::string::npos ? "https://www.npmjs.com/" : "";
            std::string origin = registry.substr(0, registry.size() - std::strlen("api/registry/"));
            if (origin == "https://qpm-website-backend.vercel.app/")
                return "https://qpm.quantumlogicslimited.com/";
            // The local backend's UI is the Vite dev server, which proxies /api to it.
            if (origin == "http://localhost:8000/" || origin == "http://127.0.0.1:8000/")
                return "http://localhost:3000/";
            return origin;
        }
    } // namespace

    void setRegistryOverride(const std::string &url)
    {
        g_registryOverride = url;
    }

    std::string registryBaseUrl()
    {
        std::string base = g_registryOverride.empty() ? envOr("QPM_REGISTRY", kDefaultRegistry) : g_registryOverride;
        if (base.back() != '/')
            base += '/';
        return base;
    }

    bool isQpmRegistry(const std::string &registry)
    {
        const std::string suffix = "/api/registry/";
        return registry.size() > suffix.size() &&
               registry.compare(registry.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    std::string packagePageUrl(const std::string &registry, const std::string &name)
    {
        std::string site = websiteBaseUrl(registry);
        return site.empty() ? "" : site + "package/" + name;
    }

    std::string upstreamRegistryUrl()
    {
        std::string base = envOr("QPM_UPSTREAM", "https://registry.npmjs.org/");
        if (base == "none")
            return "";
        if (base.back() != '/')
            base += '/';
        return base;
    }

    std::string authToken()
    {
        std::string fromEnv = envOr("QPM_TOKEN", "");
        if (!fromEnv.empty())
            return fromEnv;
        return loadRc().get("tokens").get(registryBaseUrl()).asString();
    }

    double tokenExpiry(const std::string &token)
    {
        return jwtClaims(token).get("exp").asNumber(0);
    }

    int runLogin(const LoginOptions &opts)
    {
        std::string registry = registryBaseUrl();
        std::string site = websiteBaseUrl(registry);
        std::string token = cleanToken(opts.token);

        if (opts.password)
        {
            std::cout << "[qpm] log in to " << (site.empty() ? registry : site) << "\n";
            std::string user, password;
            std::cout << "Username or email: " << std::flush;
            readLine(user);
            std::cout << "Password: " << std::flush;
            readMasked(password);
            if (user.empty() || password.empty())
            {
                std::cerr << "[qpm] username and password are required\n";
                return 1;
            }

            JsonValue body = JsonValue::makeObject();
            body.set("emailOrUsername", user);
            body.set("password", password);
            HttpResponse resp = httpRequest("POST", authBaseUrl() + "login", body.stringify(),
                                            {"Content-Type: application/json", "Accept: application/json"});
            if (!resp.ok())
            {
                std::cerr << "[qpm] login failed (" << describeFailure(resp) << ")\n";
                if (resp.status == 401 && !site.empty())
                    std::cerr << "[qpm] no account yet? sign up at " << site
                              << " (or run `qpm login` and paste the API token from your profile)\n";
                return 1;
            }
            try
            {
                token = JsonValue::parse(resp.body).get("token").asString();
            }
            catch (...)
            {
            }
            if (token.empty())
            {
                std::cerr << "[qpm] login failed (registry returned no token)\n";
                return 1;
            }
        }
        else if (token.empty())
        {
            std::cout << "[qpm] log in to " << (site.empty() ? registry : site) << "\n";
            if (!site.empty())
                std::cout << "[qpm] copy your API token from " << site << "profile (Copy token)"
                          << (stdinIsConsole() ? ", or press Enter to open that page" : "") << "\n";
            for (bool opened = false;;)
            {
                std::cout << "Enter your API token: " << std::flush;
                std::string line;
                if (!readLine(line))
                {
                    std::cout << "\n";
                    std::cerr << "[qpm] no token entered\n";
                    return 1;
                }
                token = cleanToken(line);
                if (!token.empty())
                    break;
                if (site.empty() || opened || !stdinIsConsole())
                {
                    std::cerr << "[qpm] no token entered\n";
                    return 1;
                }
                openInBrowser(site + "profile");
                opened = true;
                std::cout << "[qpm] opened " << site << "profile in your browser: sign in, click Copy token, "
                          << "and paste it here\n";
            }
        }

        std::string username, error;
        if (!lookupUser(token, username, error))
        {
            std::cerr << "[qpm] the registry didn't accept that token (" << error << ")\n";
            if (!site.empty())
                std::cerr << "[qpm] copy it again from " << site << "profile; Copy token copies all of it\n";
            return 1;
        }

        // A website session token expires a week after signing in. Trade it for the
        // account's API token, which doesn't, on registries that hand those out.
        if (jwtClaims(token).get("kind").asString() != "api")
        {
            HttpResponse resp = httpRequest("GET", authBaseUrl() + "api-token", "",
                                            {"Accept: application/json", "Authorization: Bearer " + token});
            if (resp.ok())
            {
                try
                {
                    std::string apiToken = JsonValue::parse(resp.body).get("token").asString();
                    if (!apiToken.empty())
                        token = apiToken;
                }
                catch (...)
                {
                }
            }
        }

        setSavedToken(registry, token);
        std::cout << "[qpm] logged in as " << username << "\n";
        double exp = tokenExpiry(token);
        if (exp > 0)
            std::cout << "[qpm] this token " << expiryText(exp) << "; run `qpm login` again after that\n";
        else
            std::cout << "[qpm] saved in " << rcPath().string()
                      << ", so `qpm init` and `qpm publish` work from now on without logging in again\n";
        if (!envOr("QPM_TOKEN", "").empty())
            std::cerr << "[qpm] note: QPM_TOKEN is set and is used instead of this login until you clear it\n";
        return 0;
    }

    int runLogout()
    {
        std::string registry = registryBaseUrl();
        if (loadRc().get("tokens").get(registry).asString().empty())
        {
            std::cout << "[qpm] not logged in to " << registry << "\n";
            return 0;
        }
        setSavedToken(registry, "");
        std::cout << "[qpm] logged out of " << registry << "\n";
        return 0;
    }

    bool lookupUser(const std::string &token, std::string &username, std::string &error)
    {
        JsonValue user;
        bool rejected = false;
        if (!fetchAccount(token, user, error, rejected))
            return false;
        username = user.get("username").asString("(unknown user)");
        return true;
    }

    int runAuthStatus()
    {
        std::string registry = registryBaseUrl();
        std::string site = websiteBaseUrl(registry);
        bool fromEnv = !envOr("QPM_TOKEN", "").empty();
        std::string token = authToken();
        int status = 0;

        // Colors follow the website: orange brand and token, cyan links, gray labels.
        const char *orange = "38;5;208", *gray = "90", *link = "4;36";
        const std::string tick = paint("\xE2\x9C\x93", "1;32"), cross = paint("\xE2\x9C\x97", "1;31");
        const std::string loginCommand = paint("qpm login", "1;36");
        auto label = [&](const char *name)
        { return "    " + paint(std::string(name) + std::string(11 - std::strlen(name), ' '), gray); };

        std::cout << paint(site.empty() ? registry : site, ("1;" + std::string(orange)).c_str()) << "\n";
        if (token.empty())
        {
            std::cout << "  " << cross << " Not logged in. Run " << loginCommand << " and paste the API token from "
                      << (site.empty() ? std::string("your account") : paint(site + "profile", link)) << "\n";
            status = 1;
        }
        else
        {
            JsonValue user;
            std::string error;
            bool rejected = false;
            if (fetchAccount(token, user, error, rejected))
            {
                std::string email = user.get("email").asString();
                std::cout << "  " << tick << " Logged in as "
                          << paint(user.get("username").asString("(unknown user)"), "1;32")
                          << (email.empty() ? "" : " " + paint("(" + email + ")", gray)) << "\n";
                if (!site.empty())
                    std::cout << label("Profile") << paint(site + "profile", link) << "\n";

                // The packages this account owns, as the profile page lists them.
                HttpResponse resp = isQpmRegistry(registry)
                                        ? httpRequest("GET", registry + "user/my-packages", "",
                                                      {"Accept: application/json", "Authorization: Bearer " + token})
                                        : HttpResponse{};
                JsonValue packages;
                try
                {
                    if (resp.ok())
                        packages = JsonValue::parse(resp.body).get("packages");
                }
                catch (...)
                {
                }
                if (packages.isArray())
                {
                    const auto &list = packages.array();
                    std::cout << label("Packages");
                    if (list.empty())
                        std::cout << "none yet " << paint("(qpm publish adds one)", gray);
                    else
                        std::cout << paint(std::to_string(list.size()), "1") << paint(":", gray) << " ";
                    for (size_t i = 0; i < list.size() && i < 8; ++i)
                        std::cout << (i ? paint(", ", gray) : "") << paint(list[i].get("name").asString(), "36") << " "
                                  << paint(list[i].get("latestVersion").asString(), gray);
                    if (list.size() > 8)
                        std::cout << paint(" and " + std::to_string(list.size() - 8) + " more", gray);
                    std::cout << "\n";
                }
            }
            else if (rejected)
            {
                std::cout << "  " << cross << " The saved token doesn't work anymore " << paint("(" + error + ")", gray)
                          << "\n    Run " << loginCommand << " to attach your profile again\n";
                status = 1;
            }
            else
            {
                std::cout << "  " << cross << " Couldn't reach the registry to check the token "
                          << paint("(" + error + ")", gray) << "\n";
                status = 1;
            }
            std::string masked = token.size() > 12
                                     ? "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" +
                                           token.substr(token.size() - 6)
                                     : "(short)";
            std::cout << label("Token") << paint(masked, orange) << paint(", ", gray) << tokenKindText(token) << "\n"
                      << label("Stored in")
                      << paint(fromEnv ? "the QPM_TOKEN environment variable" : rcPath().string(), gray) << "\n";
        }

        // Logins saved for other registries (a local backend, say).
        std::vector<std::string> others;
        JsonValue saved = loadRc().get("tokens");
        for (const auto &m : saved.object())
            if (m.first != registry && !m.second.asString().empty())
                others.push_back(m.first);
        if (!others.empty())
        {
            std::cout << "\n" << paint("Also logged in to:", "1") << "\n";
            for (const auto &other : others)
                std::cout << "  " << paint(other, "36") << "  "
                          << paint("(qpm auth --registry " + other + " checks it)", gray) << "\n";
        }
        return status;
    }

    int runWhoami()
    {
        std::string token = authToken();
        if (token.empty())
        {
            std::cerr << "[qpm] not logged in (run `qpm login`)\n";
            return 1;
        }

        std::string username, error;
        if (!lookupUser(token, username, error))
        {
            std::cerr << "[qpm] " << error << " — run `qpm login` again\n";
            return 1;
        }
        std::cout << username << "\n";
        double exp = tokenExpiry(token);
        if (exp > 0)
            std::cerr << "[qpm] this login " << expiryText(exp) << "\n";
        return 0;
    }

} // namespace qpm
