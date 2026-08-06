#include "QpmSemver.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace qpm
{
    namespace
    {
        std::vector<std::string> splitStr(const std::string &s, char sep)
        {
            std::vector<std::string> out;
            std::string cur;
            for (char c : s)
            {
                if (c == sep)
                {
                    out.push_back(cur);
                    cur.clear();
                }
                else
                {
                    cur += c;
                }
            }
            out.push_back(cur);
            return out;
        }

        std::string trim(const std::string &s)
        {
            size_t a = 0, b = s.size();
            while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
            while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
            return s.substr(a, b - a);
        }

        bool isDigits(const std::string &s)
        {
            if (s.empty()) return false;
            for (char c : s)
                if (!std::isdigit(static_cast<unsigned char>(c)))
                    return false;
            return true;
        }

        int comparePrereleaseIdent(const std::string &a, const std::string &b)
        {
            bool an = isDigits(a), bn = isDigits(b);
            if (an && bn)
            {
                long long ai = std::stoll(a), bi = std::stoll(b);
                return ai < bi ? -1 : (ai > bi ? 1 : 0);
            }
            if (an != bn)
                return an ? -1 : 1; // numeric identifiers have lower precedence
            return a < b ? -1 : (a > b ? 1 : 0);
        }

        // A partial version token like "1", "1.2", "1.2.3", "1.x", "*", "1.2.3-beta.1".
        struct Partial
        {
            bool any = false;
            int major = -1, minor = -1, patch = -1;
            std::vector<std::string> prerelease;
        };

        bool isWildcardToken(const std::string &s)
        {
            return s.empty() || s == "x" || s == "X" || s == "*";
        }

        Partial parsePartial(const std::string &tokenIn)
        {
            Partial p;
            std::string token = tokenIn;
            // Strip build metadata.
            auto plusPos = token.find('+');
            if (plusPos != std::string::npos)
                token = token.substr(0, plusPos);

            std::string main = token, pre;
            auto dashPos = token.find('-');
            if (dashPos != std::string::npos)
            {
                main = token.substr(0, dashPos);
                pre = token.substr(dashPos + 1);
            }

            if (isWildcardToken(main))
            {
                p.any = true;
                return p;
            }

            auto parts = splitStr(main, '.');
            auto readPart = [](const std::string &s) -> int
            {
                if (isWildcardToken(s)) return -1;
                if (!isDigits(s)) return -1;
                try { return std::stoi(s); } catch (...) { return -1; }
            };
            if (parts.size() > 0) p.major = readPart(parts[0]);
            if (parts.size() > 1) p.minor = readPart(parts[1]);
            if (parts.size() > 2) p.patch = readPart(parts[2]);
            if (p.major == -1) p.any = true;

            if (!pre.empty())
                p.prerelease = splitStr(pre, '.');
            return p;
        }

        SemVer fillLow(const Partial &p)
        {
            SemVer v;
            v.major = p.major < 0 ? 0 : p.major;
            v.minor = p.minor < 0 ? 0 : p.minor;
            v.patch = p.patch < 0 ? 0 : p.patch;
            v.prerelease = p.prerelease;
            return v;
        }

        using Cmp = std::pair<std::string, SemVer>; // op, version

        void pushUpperExclusive(std::vector<Cmp> &out, int major, int minor, int patch)
        {
            SemVer v;
            v.major = major;
            v.minor = minor;
            v.patch = patch;
            out.push_back({"<", v});
        }

        // Expand a single comparator token (with optional leading operator) into
        // one or more primitive >=/<=/>/</= comparators, AND-ed together.
        void expandToken(const std::string &rawTok, std::vector<Cmp> &out)
        {
            std::string tok = trim(rawTok);
            if (tok.empty())
                return;

            std::string op;
            size_t i = 0;
            if (tok.compare(0, 2, ">=") == 0) { op = ">="; i = 2; }
            else if (tok.compare(0, 2, "<=") == 0) { op = "<="; i = 2; }
            else if (tok[0] == '>') { op = ">"; i = 1; }
            else if (tok[0] == '<') { op = "<"; i = 1; }
            else if (tok[0] == '=') { op = "="; i = 1; }
            else if (tok[0] == '^') { op = "^"; i = 1; }
            else if (tok[0] == '~') { op = "~"; i = 1; }

            std::string rest = trim(tok.substr(i));
            Partial p = parsePartial(rest);

            if (op.empty())
            {
                // Bare version / X-range.
                if (p.any) { out.push_back({">=", SemVer{}}); return; }
                if (p.minor < 0)
                {
                    out.push_back({">=", fillLow(p)});
                    pushUpperExclusive(out, p.major + 1, 0, 0);
                }
                else if (p.patch < 0)
                {
                    out.push_back({">=", fillLow(p)});
                    pushUpperExclusive(out, p.major, p.minor + 1, 0);
                }
                else
                {
                    SemVer exact = fillLow(p);
                    out.push_back({"=", exact});
                }
                return;
            }

            if (op == "^")
            {
                if (p.any) { out.push_back({">=", SemVer{}}); return; }
                SemVer low = fillLow(p);
                out.push_back({">=", low});
                if (low.major > 0 || p.minor < 0)
                {
                    if (p.minor < 0) pushUpperExclusive(out, p.major + 1, 0, 0);
                    else pushUpperExclusive(out, low.major + 1, 0, 0);
                }
                else if (low.minor > 0 || p.patch < 0)
                {
                    if (p.patch < 0) pushUpperExclusive(out, 0, low.minor + 1, 0);
                    else pushUpperExclusive(out, 0, low.minor + 1, 0);
                }
                else
                {
                    pushUpperExclusive(out, 0, 0, low.patch + 1);
                }
                return;
            }

            if (op == "~")
            {
                if (p.any) { out.push_back({">=", SemVer{}}); return; }
                SemVer low = fillLow(p);
                out.push_back({">=", low});
                if (p.minor < 0) pushUpperExclusive(out, p.major + 1, 0, 0);
                else pushUpperExclusive(out, low.major, low.minor + 1, 0);
                return;
            }

            if (op == "=")
            {
                out.push_back({"=", fillLow(p)});
                return;
            }

            // >=, <=, >, < : partial components fill with 0.
            out.push_back({op, fillLow(p)});
        }

        // Splits a comparator-set string on whitespace, but keeps a hyphen range
        // ("1.2.3 - 2.3.4") intact as one logical unit.
        std::vector<std::string> splitComparatorSet(const std::string &setStr)
        {
            std::vector<std::string> raw;
            {
                std::istringstream iss(setStr);
                std::string w;
                while (iss >> w) raw.push_back(w);
            }
            std::vector<std::string> merged;
            for (size_t i = 0; i < raw.size(); ++i)
            {
                if (raw[i] == "-" && i > 0 && i + 1 < raw.size())
                {
                    merged.back() = merged.back() + " - " + raw[i + 1];
                    ++i;
                }
                else
                {
                    merged.push_back(raw[i]);
                }
            }
            return merged;
        }
    } // namespace

    bool SemVer::tryParse(const std::string &s, SemVer &out)
    {
        Partial p = parsePartial(trim(s));
        if (p.any || p.major < 0 || p.minor < 0 || p.patch < 0)
            return false;
        out.major = p.major;
        out.minor = p.minor;
        out.patch = p.patch;
        out.prerelease = p.prerelease;
        return true;
    }

    std::string SemVer::toString() const
    {
        std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
        if (!prerelease.empty())
        {
            s += "-";
            for (size_t i = 0; i < prerelease.size(); ++i)
            {
                if (i) s += ".";
                s += prerelease[i];
            }
        }
        return s;
    }

    int compareSemVer(const SemVer &a, const SemVer &b)
    {
        if (a.major != b.major) return a.major < b.major ? -1 : 1;
        if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
        if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
        if (a.prerelease.empty() && b.prerelease.empty()) return 0;
        if (a.prerelease.empty()) return 1; // release > prerelease
        if (b.prerelease.empty()) return -1;
        size_t n = std::min(a.prerelease.size(), b.prerelease.size());
        for (size_t i = 0; i < n; ++i)
        {
            int c = comparePrereleaseIdent(a.prerelease[i], b.prerelease[i]);
            if (c != 0) return c;
        }
        if (a.prerelease.size() != b.prerelease.size())
            return a.prerelease.size() < b.prerelease.size() ? -1 : 1;
        return 0;
    }

    SemVerRange::SemVerRange(const std::string &rangeStr)
    {
        std::string trimmed = trim(rangeStr);
        if (trimmed.empty() || trimmed == "*" || trimmed == "latest" || trimmed == "x")
        {
            isAny_ = true;
            return;
        }

        for (const std::string &orPart : splitStr(trimmed, '|'))
        {
            // splitStr on '|' turns "||" into an empty token between two pipes; skip those.
            std::string setStr = trim(orPart);
            if (setStr.empty())
                continue;

            std::vector<Cmp> group;
            auto tokens = splitComparatorSet(setStr);
            for (size_t i = 0; i < tokens.size(); ++i)
            {
                auto hy = tokens[i].find(" - ");
                if (hy != std::string::npos)
                {
                    std::string lo = trim(tokens[i].substr(0, hy));
                    std::string hi = trim(tokens[i].substr(hy + 3));
                    Partial pl = parsePartial(lo);
                    Partial ph = parsePartial(hi);
                    group.push_back({">=", fillLow(pl)});
                    if (ph.minor < 0) pushUpperExclusive(group, ph.major + 1, 0, 0);
                    else if (ph.patch < 0) pushUpperExclusive(group, ph.major, ph.minor + 1, 0);
                    else group.push_back({"<=", fillLow(ph)});
                }
                else
                {
                    expandToken(tokens[i], group);
                }
            }
            if (!group.empty())
                groups_.push_back(std::move(group));
        }
        if (groups_.empty())
            isAny_ = true;
    }

    bool SemVerRange::satisfies(const SemVer &v) const
    {
        if (isAny_)
            return true;
        for (const auto &group : groups_)
        {
            bool ok = true;
            for (const auto &c : group)
            {
                int cmp = compareSemVer(v, c.second);
                bool pass = false;
                if (c.first == ">=") pass = cmp >= 0;
                else if (c.first == "<=") pass = cmp <= 0;
                else if (c.first == ">") pass = cmp > 0;
                else if (c.first == "<") pass = cmp < 0;
                else if (c.first == "=") pass = cmp == 0;
                if (!pass) { ok = false; break; }
            }
            if (ok)
                return true;
        }
        return false;
    }

    std::string maxSatisfying(const std::vector<std::string> &versions, const std::string &range)
    {
        SemVerRange r(range);
        // Does the range itself pin an exact prerelease version? If so, allow
        // matching prereleases with that same major.minor.patch (npm semantics).
        SemVer pinnedPre;
        bool hasPinnedPre = false;
        {
            SemVer maybe;
            if (SemVer::tryParse(trim(range), maybe) && maybe.hasPrerelease())
            {
                pinnedPre = maybe;
                hasPinnedPre = true;
            }
        }

        const SemVer *best = nullptr;
        SemVer bestVal;
        std::string bestStr;
        for (const auto &vs : versions)
        {
            SemVer v;
            if (!SemVer::tryParse(vs, v))
                continue;
            if (v.hasPrerelease())
            {
                bool allowed = hasPinnedPre && v.major == pinnedPre.major &&
                                v.minor == pinnedPre.minor && v.patch == pinnedPre.patch;
                if (!allowed)
                    continue;
            }
            if (!r.satisfies(v))
                continue;
            if (!best || compareSemVer(v, bestVal) > 0)
            {
                bestVal = v;
                bestStr = vs;
                best = &bestVal;
            }
        }
        return bestStr;
    }

} // namespace qpm
