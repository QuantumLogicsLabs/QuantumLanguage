#include "QpmSearch.h"
#include "QpmColor.h"
#include "QpmHttp.h"
#include "QpmJson.h"
#include "QpmRegistry.h"
#include "QpmSemver.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace qpm
{
    namespace
    {
        // One search result, whichever kind of registry it came from.
        struct Hit
        {
            std::string name, version, description, author;
            std::string date; // ISO-8601, as the registry sent it
            std::vector<std::string> keywords;
            double downloads = -1; // -1 when the registry doesn't report them
        };

        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // Control characters (a description's newlines and tabs) become spaces,
        // and runs of spaces collapse to one.
        std::string oneLine(const std::string &s)
        {
            std::string out;
            for (unsigned char c : s)
            {
                if (c < 0x20 || c == 0x7f)
                    c = ' ';
                if (c == ' ' && (out.empty() || out.back() == ' '))
                    continue;
                out += static_cast<char>(c);
            }
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            return out;
        }

        std::string join(const std::vector<std::string> &parts, const std::string &sep)
        {
            std::string out;
            for (size_t i = 0; i < parts.size(); ++i)
                out += (i ? sep : "") + parts[i];
            return out;
        }

        // Keywords arrive as an array, or from some manifests as one comma-separated string.
        std::vector<std::string> stringList(const JsonValue &v)
        {
            std::vector<std::string> out;
            if (v.isArray())
            {
                for (const auto &e : v.array())
                    if (!oneLine(e.asString()).empty())
                        out.push_back(oneLine(e.asString()));
            }
            else if (v.isString())
            {
                std::istringstream in(v.asString());
                std::string part;
                while (std::getline(in, part, ','))
                    if (!oneLine(part).empty())
                        out.push_back(oneLine(part));
            }
            return out;
        }

        // Columns of the console stdout writes to; through a pipe or into a file,
        // COLUMNS if set (terminals like mintty export it), else 0 = no limit.
        size_t consoleWidth()
        {
#ifdef _WIN32
            CONSOLE_SCREEN_BUFFER_INFO info;
            if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
                return static_cast<size_t>(info.srWindow.Right - info.srWindow.Left + 1);
#endif
            const char *columns = std::getenv("COLUMNS");
            int n = columns ? std::atoi(columns) : 0;
            return n > 0 ? static_cast<size_t>(n) : 0;
        }

        // UTF-8 code points: a good-enough stand-in for terminal columns.
        size_t displayWidth(const std::string &s)
        {
            size_t n = 0;
            for (unsigned char c : s)
                if ((c & 0xC0) != 0x80)
                    ++n;
            return n;
        }

        // `s` cut to `width` columns, ending in "…" when something was cut.
        std::string fit(const std::string &s, size_t width)
        {
            if (displayWidth(s) <= width)
                return s;
            if (width == 0)
                return "";
            size_t keep = width - 1, count = 0, i = 0;
            for (; i < s.size(); ++i)
            {
                if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80)
                {
                    if (count == keep)
                        break;
                    ++count;
                }
            }
            return s.substr(0, i) + "\xE2\x80\xA6";
        }

        // Wraps every case-insensitive occurrence of a search term in `on` ... `off`.
        std::string highlight(const std::string &text, const std::vector<std::string> &termsLower,
                              const char *on, const char *off)
        {
            std::string lowerText = lower(text);
            std::vector<bool> marked(text.size(), false);
            for (const auto &t : termsLower)
            {
                if (t.empty() || t[0] == '=')
                    continue;
                for (size_t pos = lowerText.find(t); pos != std::string::npos; pos = lowerText.find(t, pos + 1))
                    std::fill(marked.begin() + static_cast<std::ptrdiff_t>(pos),
                              marked.begin() + static_cast<std::ptrdiff_t>(pos + t.size()), true);
            }
            std::string out;
            for (size_t i = 0; i < text.size(); ++i)
            {
                if (marked[i] && (i == 0 || !marked[i - 1]))
                    out += on;
                out += text[i];
                if (marked[i] && (i + 1 == text.size() || !marked[i + 1]))
                    out += off;
            }
            return out;
        }

        std::string formatCount(double n)
        {
            std::string digits = std::to_string(static_cast<long long>(n));
            std::string out;
            for (size_t i = 0; i < digits.size(); ++i)
            {
                if (i && (digits.size() - i) % 3 == 0)
                    out += ',';
                out += digits[i];
            }
            return out;
        }

        std::string formatSize(double bytes)
        {
            std::ostringstream ss;
            if (bytes < 1000)
                ss << static_cast<long long>(bytes) << " B";
            else if (bytes < 1000 * 1000)
                ss << std::fixed << std::setprecision(1) << bytes / 1000.0 << " kB";
            else
                ss << std::fixed << std::setprecision(1) << bytes / 1000000.0 << " MB";
            return ss.str();
        }

        bool parseIsoUtc(const std::string &iso, std::time_t &out)
        {
            int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
            if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) < 3)
                return false;
            std::tm tm{};
            tm.tm_year = year - 1900;
            tm.tm_mon = month - 1;
            tm.tm_mday = day;
            tm.tm_hour = hour;
            tm.tm_min = minute;
            tm.tm_sec = second;
#ifdef _WIN32
            out = _mkgmtime(&tm);
#else
            out = timegm(&tm);
#endif
            return out != static_cast<std::time_t>(-1);
        }

        // "3 days ago", or "" if `iso` isn't a date.
        std::string timeAgo(const std::string &iso)
        {
            std::time_t then;
            if (!parseIsoUtc(iso, then))
                return "";
            double secs = std::max(0.0, std::difftime(std::time(nullptr), then));
            static const struct
            {
                double seconds;
                const char *unit;
            } units[] = {{31536000, "year"}, {2592000, "month"}, {604800, "week"},
                         {86400, "day"},     {3600, "hour"},     {60, "minute"}};
            for (const auto &u : units)
            {
                if (secs >= u.seconds)
                {
                    long long n = static_cast<long long>(secs / u.seconds);
                    return std::to_string(n) + " " + u.unit + (n == 1 ? "" : "s") + " ago";
                }
            }
            return "just now";
        }

        // ---- search ---------------------------------------------------------

        // A term matches when it appears in the name, the description or a
        // keyword; "=user" matches the packages that user published.
        bool matchesTerm(const Hit &h, const std::string &termLower)
        {
            if (termLower.size() > 1 && termLower[0] == '=')
                return lower(h.author) == termLower.substr(1);
            if (lower(h.name).find(termLower) != std::string::npos ||
                lower(h.description).find(termLower) != std::string::npos)
                return true;
            for (const auto &k : h.keywords)
                if (lower(k).find(termLower) != std::string::npos)
                    return true;
            return false;
        }

        // 0 = a term is the whole name, 1 = the name starts with one,
        // 2 = the name contains one, 3 = only the description or keywords match.
        int relevance(const Hit &h, const std::vector<std::string> &termsLower)
        {
            std::string name = lower(h.name);
            int best = 3;
            for (const auto &t : termsLower)
            {
                if (t.empty() || t[0] == '=')
                    continue;
                if (name == t)
                    best = 0;
                else if (name.compare(0, t.size(), t) == 0)
                    best = std::min(best, 1);
                else if (name.find(t) != std::string::npos)
                    best = std::min(best, 2);
            }
            return best;
        }

        // The QPM registry matches `q` as a case-insensitive regular expression,
        // so it only gets a run of characters that stand for themselves in one
        // (letters, digits, '-', '_' and non-ASCII); every term is then matched
        // literally here. The longest run narrows the server's answer the most.
        std::string serverQuery(const std::vector<std::string> &termsLower)
        {
            std::string best;
            for (const auto &t : termsLower)
            {
                if (!t.empty() && t[0] == '=')
                    continue;
                std::string run;
                for (size_t i = 0; i <= t.size(); ++i)
                {
                    unsigned char c = i < t.size() ? static_cast<unsigned char>(t[i]) : 0;
                    if (c && (std::isalnum(c) || c == '-' || c == '_' || c >= 0x80))
                    {
                        run += static_cast<char>(c);
                        continue;
                    }
                    if (run.size() > best.size())
                        best = run;
                    run.clear();
                }
            }
            return best;
        }

        bool parseBody(const HttpResponse &resp, JsonValue &doc, std::string &error)
        {
            try
            {
                doc = JsonValue::parse(resp.body);
                return true;
            }
            catch (const std::exception &e)
            {
                error = std::string("the registry sent something that isn't JSON: ") + e.what();
                return false;
            }
        }

        // GET <registry>search?q=  ->  {"objects": [{name, description, keywords,
        // latest_version, downloads, updated_at, owner}], "total": n}
        bool searchQpm(const std::string &registry, const std::vector<std::string> &termsLower,
                       std::vector<Hit> &hits, std::string &error)
        {
            std::string q = serverQuery(termsLower);
            HttpResponse resp = httpGet(registry + "search" + (q.empty() ? "" : "?q=" + urlEncodeComponent(q)),
                                        "application/json");
            if (!resp.ok())
            {
                error = describeFailure(resp);
                return false;
            }
            JsonValue doc;
            if (!parseBody(resp, doc, error))
                return false;
            const JsonValue *objects = doc.find("objects");
            if (!objects || !objects->isArray())
            {
                error = "the registry's search answer has no \"objects\" list";
                return false;
            }
            for (const auto &o : objects->array())
            {
                Hit h;
                h.name = o.get("name").asString();
                if (h.name.empty())
                    continue;
                h.version = o.get("latest_version").asString();
                h.description = oneLine(o.get("description").asString());
                h.keywords = stringList(o.get("keywords"));
                h.author = o.get("owner").asString();
                if (h.author == "anonymous") // published by a guest
                    h.author.clear();
                h.date = o.get("updated_at").asString();
                h.downloads = o.get("downloads").asNumber(0);
                hits.push_back(std::move(h));
            }

            std::vector<Hit> kept;
            for (auto &h : hits)
            {
                bool all = true;
                for (const auto &t : termsLower)
                    all = all && matchesTerm(h, t);
                if (all)
                    kept.push_back(std::move(h));
            }
            // The registry already ranks by downloads; put name matches first.
            std::stable_sort(kept.begin(), kept.end(), [&](const Hit &a, const Hit &b)
                             { return relevance(a, termsLower) < relevance(b, termsLower); });
            hits.swap(kept);
            return true;
        }

        // GET <registry>-/v1/search?text=&size=  (npm's search API)
        bool searchNpm(const std::string &registry, const std::vector<std::string> &termsLower, int limit,
                       std::vector<Hit> &hits, double &total, std::string &error)
        {
            std::vector<std::string> words;
            for (const auto &t : termsLower)
                words.push_back(t.size() > 1 && t[0] == '=' ? "maintainer:" + t.substr(1) : t);
            HttpResponse resp = httpGet(registry + "-/v1/search?text=" + urlEncodeComponent(join(words, " ")) +
                                            "&size=" + std::to_string(std::min(limit, 250)),
                                        "application/json");
            if (!resp.ok())
            {
                error = describeFailure(resp);
                return false;
            }
            JsonValue doc;
            if (!parseBody(resp, doc, error))
                return false;
            const JsonValue *objects = doc.find("objects");
            if (!objects || !objects->isArray())
            {
                error = "the registry's search answer has no \"objects\" list";
                return false;
            }
            for (const auto &o : objects->array())
            {
                JsonValue p = o.get("package");
                Hit h;
                h.name = p.get("name").asString();
                if (h.name.empty())
                    continue;
                h.version = p.get("version").asString();
                h.description = oneLine(p.get("description").asString());
                h.keywords = stringList(p.get("keywords"));
                h.author = p.get("publisher").get("username").asString();
                h.date = p.get("date").asString();
                h.downloads = o.get("downloads").get("weekly").asNumber(-1);
                hits.push_back(std::move(h));
            }
            total = doc.get("total").asNumber(static_cast<double>(hits.size()));
            return true;
        }

        struct Column
        {
            explicit Column(std::string h) : header(std::move(h)) {}
            std::string header;
            std::vector<std::string> cells;
            bool flexible = false;    // shrinks (or goes) when the console is too narrow
            size_t floor = 16;        // narrowest it shrinks to while another flexible column is shown
            bool rightAlign = false;
            bool highlighted = false; // search terms are highlighted in it
            size_t width = 0;
        };

        // npm-style "A | B | C" table. With a console width, flexible columns
        // shrink (rightmost first) and then drop out until the rows fit; the
        // last flexible column left may shrink to 16 columns.
        void printTable(std::vector<Column> cols, size_t consoleCols, const std::vector<std::string> &termsLower,
                        bool color)
        {
            const size_t sep = 3; // " | "
            auto natural = [](Column &c)
            {
                c.width = displayWidth(c.header);
                for (const auto &cell : c.cells)
                    c.width = std::max(c.width, displayWidth(cell));
            };
            for (auto &c : cols)
                natural(c);

            while (consoleCols > 0)
            {
                size_t used = sep * (cols.size() - 1);
                for (const auto &c : cols)
                    used += c.width;
                if (used < consoleCols)
                    break;
                size_t over = used - consoleCols + 1;
                size_t flexibleCount = 0;
                for (const auto &c : cols)
                    flexibleCount += c.flexible ? 1 : 0;
                for (size_t i = cols.size(); i-- > 0 && over > 0;)
                {
                    if (!cols[i].flexible)
                        continue;
                    size_t floor = std::max<size_t>(displayWidth(cols[i].header), flexibleCount > 1 ? cols[i].floor : 16);
                    size_t cut = cols[i].width > floor ? std::min(over, cols[i].width - floor) : 0;
                    cols[i].width -= cut;
                    over -= cut;
                }
                if (over == 0)
                    break;
                // Still too wide: drop the rightmost flexible column and let the
                // others have their space back.
                size_t drop = cols.size();
                for (size_t i = cols.size(); i-- > 0;)
                    if (cols[i].flexible)
                    {
                        drop = i;
                        break;
                    }
                if (drop == cols.size())
                    break; // nothing left to give up; let the console wrap
                cols.erase(cols.begin() + static_cast<std::ptrdiff_t>(drop));
                for (auto &c : cols)
                    if (c.flexible)
                        natural(c);
            }

            const char *hiOn = "\x1b[1;33m", *hiOff = "\x1b[22;39m";
            auto line = [&](auto cellAt, bool header)
            {
                std::string out;
                for (size_t j = 0; j < cols.size(); ++j)
                {
                    const Column &c = cols[j];
                    std::string text = fit(cellAt(c), c.width);
                    size_t padding = c.width - displayWidth(text);
                    if (color && !header && c.highlighted)
                        text = highlight(text, termsLower, hiOn, hiOff);
                    if (j)
                        out += " | ";
                    if (c.rightAlign)
                        out += std::string(padding, ' ') + text;
                    else
                        out += text + (j + 1 < cols.size() ? std::string(padding, ' ') : "");
                }
                while (!out.empty() && out.back() == ' ')
                    out.pop_back();
                std::cout << out << "\n";
            };
            line([](const Column &c)
                 { return c.header; }, true);
            size_t rows = cols.empty() ? 0 : cols[0].cells.size();
            for (size_t i = 0; i < rows; ++i)
                line([i](const Column &c)
                     { return c.cells[i]; }, false);
        }

        // ---- view -----------------------------------------------------------

        // Looks up a dotted path ("dist.tarball", "time.1.0.0"); a key may itself
        // contain dots, so the longest key that exists at each level wins.
        const JsonValue *lookupPath(const JsonValue &root, const std::vector<std::string> &segments, size_t from)
        {
            if (from == segments.size())
                return &root;
            for (size_t to = segments.size(); to > from; --to)
            {
                std::string key = segments[from];
                for (size_t k = from + 1; k < to; ++k)
                    key += "." + segments[k];
                if (const JsonValue *child = root.find(key))
                    if (const JsonValue *found = lookupPath(*child, segments, to))
                        return found;
            }
            return nullptr;
        }

        const JsonValue *lookupField(const JsonValue &root, const std::string &field)
        {
            std::vector<std::string> segments;
            std::istringstream in(field);
            std::string part;
            while (std::getline(in, part, '.'))
                segments.push_back(part);
            return lookupPath(root, segments, 0);
        }

        // A string prints as itself; anything else as JSON.
        std::string showValue(const JsonValue &v)
        {
            return v.isString() ? v.asString() : v.stringify(2);
        }

        std::string personName(const JsonValue &v)
        {
            if (v.isString())
                return v.asString();
            std::string name = v.get("name").asString(v.get("username").asString());
            std::string email = v.get("email").asString();
            return email.empty() ? name : name + " <" + email + ">";
        }
    } // namespace

    int runSearch(const SearchOptions &opts)
    {
        std::string registry = opts.npm ? upstreamRegistryUrl() : registryBaseUrl();
        if (registry.empty())
        {
            std::cerr << "[qpm] --npm searches the upstream registry, but QPM_UPSTREAM is \"none\"\n";
            return 1;
        }
        int limit = std::max(1, opts.limit);

        std::vector<std::string> termsLower;
        for (const auto &t : opts.terms)
            if (!oneLine(t).empty())
                termsLower.push_back(lower(oneLine(t)));

        std::vector<Hit> hits;
        double total = 0;
        std::string error;
        bool ok = isQpmRegistry(registry) ? searchQpm(registry, termsLower, hits, error)
                                          : searchNpm(registry, termsLower, limit, hits, total, error);
        if (!ok)
        {
            std::cerr << "[qpm] search failed (" << error << ")\n"
                      << "[qpm] registry: " << registry << "\n";
            return 1;
        }
        if (isQpmRegistry(registry))
            total = static_cast<double>(hits.size());
        if (hits.size() > static_cast<size_t>(limit))
            hits.resize(static_cast<size_t>(limit));

        if (opts.json)
        {
            JsonValue list = JsonValue::makeArray();
            for (const auto &h : hits)
            {
                JsonValue o = JsonValue::makeObject();
                o.set("name", h.name);
                o.set("version", h.version);
                o.set("description", h.description);
                JsonValue keywords = JsonValue::makeArray();
                for (const auto &k : h.keywords)
                    keywords.push(k);
                o.set("keywords", keywords);
                o.set("author", h.author);
                o.set("date", h.date);
                if (h.downloads >= 0)
                    o.set("downloads", h.downloads);
                std::string url = packagePageUrl(registry, h.name);
                if (!url.empty())
                    o.set("url", url);
                list.push(o);
            }
            std::cout << list.stringify(2) << "\n";
            return 0;
        }

        if (opts.parseable)
        {
            for (const auto &h : hits)
                std::cout << h.name << '\t' << h.description << '\t' << (h.author.empty() ? "" : "=" + h.author)
                          << '\t' << h.date.substr(0, 10) << (h.date.size() >= 16 ? " " + h.date.substr(11, 5) : "")
                          << '\t' << h.version << '\t' << join(h.keywords, " ") << "\n";
            return 0;
        }

        if (hits.empty())
        {
            if (opts.terms.empty())
            {
                std::cout << "No packages have been published to " << registry << " yet\n";
                return 0;
            }
            std::cout << "No matches found for \"" << join(opts.terms, " ") << "\"\n";
            if (isQpmRegistry(registry) && !upstreamRegistryUrl().empty())
                std::cout << "[qpm] `qpm install` also fetches npm packages; look for one with `qpm search --npm "
                          << join(opts.terms, " ") << "`\n";
            return 0;
        }

        bool anyDescription = false, anyAuthor = false, anyKeywords = false, anyDownloads = false;
        for (const auto &h : hits)
        {
            anyDescription = anyDescription || !h.description.empty();
            anyAuthor = anyAuthor || !h.author.empty();
            anyKeywords = anyKeywords || !h.keywords.empty();
            anyDownloads = anyDownloads || h.downloads >= 0;
        }

        Column name{"NAME"}, description{"DESCRIPTION"}, author{"AUTHOR"}, date{"DATE"}, version{"VERSION"},
            downloads{isQpmRegistry(registry) ? "DOWNLOADS" : "WEEKLY"}, keywords{"KEYWORDS"};
        name.highlighted = description.highlighted = keywords.highlighted = true;
        description.flexible = keywords.flexible = true;
        description.floor = 30; // keywords go before the description gets this narrow
        downloads.rightAlign = true;
        for (const auto &h : hits)
        {
            name.cells.push_back(h.name);
            description.cells.push_back(h.description);
            author.cells.push_back(h.author.empty() ? "" : "=" + h.author);
            date.cells.push_back(h.date.substr(0, 10));
            version.cells.push_back(h.version);
            downloads.cells.push_back(h.downloads >= 0 ? formatCount(h.downloads) : "");
            keywords.cells.push_back(join(h.keywords, " "));
        }

        std::vector<Column> cols = {name};
        if (opts.description && anyDescription)
            cols.push_back(description);
        if (anyAuthor)
            cols.push_back(author);
        cols.push_back(date);
        cols.push_back(version);
        if (anyDownloads)
            cols.push_back(downloads);
        if (anyKeywords)
            cols.push_back(keywords);

        printTable(cols, opts.longFormat ? 0 : consoleWidth(), termsLower, opts.color && colorEnabled());
        if (total > static_cast<double>(hits.size()))
            std::cout << "[qpm] showing " << hits.size() << " of " << formatCount(total)
                      << " matches (--limit <n> shows more)\n";
        return 0;
    }

    int runView(const ViewOptions &opts)
    {
        // "name@range"; a scoped "@scope/name" keeps its leading '@'.
        std::string name = opts.spec, range;
        size_t at = name.find('@', name.empty() || name[0] != '@' ? 0 : 1);
        if (at != std::string::npos)
        {
            range = name.substr(at + 1);
            name.resize(at);
        }
        if (name.empty())
        {
            std::cerr << "[qpm] usage: qpm view <pkg>[@<version>] [<field>...]\n";
            return 1;
        }

        // Same lookup order as install: the QPM registry, then upstream npm.
        std::string registry = registryBaseUrl();
        HttpResponse resp = httpGet(registry + urlEncodeComponent(name), "application/json");
        std::string upstream = upstreamRegistryUrl();
        if (resp.error.empty() && resp.status == 404 && !upstream.empty() && upstream != registry)
        {
            registry = upstream;
            resp = httpGet(registry + urlEncodeComponent(name), "application/json");
        }
        if (resp.error.empty() && resp.status == 404)
        {
            std::cerr << "[qpm] package \"" << name << "\" not found"
                      << (upstream.empty() || upstream == registryBaseUrl() ? "" : " (on the QPM registry or npm)")
                      << " — `qpm search " << name << "` lists similar names\n";
            return 1;
        }
        if (!resp.ok())
        {
            std::cerr << "[qpm] failed to fetch " << name << " (" << describeFailure(resp) << ")\n";
            return 1;
        }
        JsonValue doc;
        std::string error;
        if (!parseBody(resp, doc, error))
        {
            std::cerr << "[qpm] " << error << "\n";
            return 1;
        }

        const JsonValue *versionsObj = doc.find("versions");
        std::vector<std::string> versionList;
        if (versionsObj && versionsObj->isObject())
            for (const auto &m : versionsObj->object())
                versionList.push_back(m.first);
        std::sort(versionList.begin(), versionList.end(), [](const std::string &a, const std::string &b)
                  {
                      SemVer va, vb;
                      bool pa = SemVer::tryParse(a, va), pb = SemVer::tryParse(b, vb);
                      if (pa && pb)
                          return compareSemVer(va, vb) < 0;
                      return pa != pb ? pb : a < b; });

        JsonValue tags = doc.get("dist-tags");
        std::string version;
        if (range.empty())
            range = "latest";
        if (tags.get(range).isString() && versionsObj && versionsObj->has(tags.get(range).asString()))
            version = tags.get(range).asString();
        else
            version = maxSatisfying(versionList, range);
        if (version.empty())
        {
            std::cerr << "[qpm] no version of " << name << " matches \"" << range << "\""
                      << (versionList.empty() ? "" : " (latest is " + tags.get("latest").asString(versionList.back()) + ")")
                      << "\n";
            return 1;
        }

        // npm view's data: the version's manifest, plus the package-wide fields
        // it doesn't repeat, with "versions" as a sorted list.
        JsonValue v = versionsObj->get(version);
        JsonValue data = JsonValue::makeObject();
        for (const auto &m : v.object())
            data.set(m.first, m.second);
        for (const char *key : {"description", "keywords", "license", "homepage", "repository", "author",
                                "maintainers", "owner", "downloads", "readme"})
        {
            const JsonValue *top = doc.find(key);
            const JsonValue *mine = data.find(key);
            bool missing = !mine || mine->isNull() || (mine->isString() && mine->asString().empty());
            if (top && missing)
                data.set(key, *top);
        }
        data.set("dist-tags", tags);
        JsonValue allVersions = JsonValue::makeArray();
        for (const auto &s : versionList)
            allVersions.push(s);
        data.set("versions", allVersions);
        if (doc.has("time"))
            data.set("time", doc.get("time"));

        if (!opts.fields.empty())
        {
            if (opts.json)
            {
                if (opts.fields.size() == 1)
                {
                    const JsonValue *f = lookupField(data, opts.fields[0]);
                    if (f)
                        std::cout << f->stringify(2) << "\n";
                    return 0;
                }
                JsonValue picked = JsonValue::makeObject();
                for (const auto &field : opts.fields)
                    if (const JsonValue *f = lookupField(data, field))
                        picked.set(field, *f);
                std::cout << picked.stringify(2) << "\n";
                return 0;
            }
            for (const auto &field : opts.fields)
            {
                const JsonValue *f = lookupField(data, field);
                if (!f)
                    continue; // like npm: an absent field prints nothing
                if (opts.fields.size() == 1)
                    std::cout << showValue(*f) << "\n";
                else
                    std::cout << field << " = " << showValue(*f) << "\n";
            }
            return 0;
        }
        if (opts.json)
        {
            std::cout << data.stringify(2) << "\n";
            return 0;
        }

        bool color = opts.color && colorEnabled();
        auto paint = [color](const std::string &text, const char *code)
        { return color ? std::string("\x1b[") + code + "m" + text + "\x1b[0m" : text; };

        JsonValue deps = data.get("dependencies");
        size_t depCount = deps.isObject() ? deps.object().size() : 0;
        std::string license = data.get("license").isString() ? data.get("license").asString()
                                                             : data.get("license").get("type").asString("Proprietary");
        std::cout << "\n"
                  << paint(name + "@" + version, "1;32") << " | " << (license.empty() ? "Proprietary" : license)
                  << " | deps: " << (depCount ? std::to_string(depCount) : "none")
                  << " | versions: " << versionList.size();
        if (data.get("downloads").isNumber())
            std::cout << " | downloads: " << formatCount(data.get("downloads").asNumber());
        std::cout << "\n";
        std::string description = oneLine(data.get("description").asString());
        if (!description.empty())
            std::cout << description << "\n";
        std::string page = packagePageUrl(registry, name);
        std::string homepage = data.get("homepage").asString();
        if (!page.empty())
            std::cout << paint(page, "36") << "\n";
        if (!homepage.empty() && homepage != page)
            std::cout << paint(homepage, "36") << "\n";

        std::vector<std::string> keywordList = stringList(data.get("keywords"));
        if (!keywordList.empty())
            std::cout << "\nkeywords: " << join(keywordList, ", ") << "\n";

        JsonValue dist = data.get("dist");
        if (dist.isObject())
        {
            std::cout << "\ndist\n";
            if (dist.get("tarball").isString())
                std::cout << ".tarball: " << paint(dist.get("tarball").asString(), "36") << "\n";
            if (dist.get("shasum").isString())
                std::cout << ".shasum: " << dist.get("shasum").asString() << "\n";
            if (dist.get("integrity").isString())
                std::cout << ".integrity: " << dist.get("integrity").asString() << "\n";
            if (dist.get("unpackedSize").asNumber() > 0)
                std::cout << ".unpackedSize: " << formatSize(dist.get("unpackedSize").asNumber()) << "\n";
        }

        if (depCount)
        {
            std::cout << "\ndependencies:\n";
            size_t shown = 0;
            for (const auto &m : deps.object())
            {
                if (++shown > 24)
                {
                    std::cout << "(..." << depCount - 24 << " more)\n";
                    break;
                }
                std::cout << m.first << ": " << m.second.asString() << "\n";
            }
        }

        std::vector<std::string> maintainers;
        JsonValue maintainerList = data.get("maintainers");
        std::string owner = data.get("owner").asString();
        if (maintainerList.isArray())
            for (const auto &m : maintainerList.array())
                maintainers.push_back(personName(m));
        else if (!owner.empty() && owner != "anonymous")
            maintainers.push_back(owner);
        if (!maintainers.empty())
        {
            std::cout << "\nmaintainers:\n";
            for (const auto &m : maintainers)
                std::cout << "- " << m << "\n";
        }

        if (tags.isObject() && !tags.object().empty())
        {
            std::cout << "\ndist-tags:\n";
            for (const auto &m : tags.object())
                std::cout << paint(m.first, "1") << ": " << m.second.asString() << "\n";
        }

        std::string published = timeAgo(doc.get("time").get(version).asString());
        if (!published.empty())
        {
            std::string by = data.get("_npmUser").isObject() ? data.get("_npmUser").get("name").asString()
                                                             : data.get("owner").asString();
            std::cout << "\npublished " << published;
            if (by == "anonymous")
                std::cout << " by a guest";
            else if (!by.empty())
                std::cout << " by " << paint(by, "1");
            std::cout << "\n";
        }
        std::cout << "\n";
        return 0;
    }

} // namespace qpm
