#include "Vm.h"
#include "Error.h"
#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

QuantumValue VM::callStringMethod(const std::string &str, const std::string &m,
                                  std::vector<QuantumValue> args)
{
    if (m == "length" || m == "size")
        return QuantumValue((double)str.size());
    // C++ compatibility: e.what() on caught exception strings; s.begin()/s.end()
    // for iterator-style calls like reverse(s.begin(), s.end()).
    if (m == "what" || m == "begin" || m == "end")
        return QuantumValue(str);
    // Python str.is*() classification methods
    if (m == "isalnum" || m == "isalpha" || m == "isdigit" || m == "isnumeric" ||
        m == "isspace" || m == "isupper" || m == "islower")
    {
        if (str.empty())
            return QuantumValue(false);
        for (unsigned char c : str)
        {
            bool ok = (m == "isalnum")   ? std::isalnum(c) != 0
                      : (m == "isalpha") ? std::isalpha(c) != 0
                      : (m == "isspace") ? std::isspace(c) != 0
                      : (m == "isupper") ? std::isupper(c) != 0
                      : (m == "islower") ? std::islower(c) != 0
                                         : std::isdigit(c) != 0; // isdigit/isnumeric
            if (!ok)
                return QuantumValue(false);
        }
        return QuantumValue(true);
    }
    if (m == "capitalize")
    {
        std::string r = str;
        std::transform(r.begin(), r.end(), r.begin(), ::tolower);
        if (!r.empty())
            r[0] = (char)::toupper((unsigned char)r[0]);
        return QuantumValue(r);
    }
    if (m == "title")
    {
        std::string r = str;
        bool newWord = true;
        for (auto &c : r)
        {
            c = newWord ? (char)::toupper((unsigned char)c) : (char)::tolower((unsigned char)c);
            newWord = std::isalpha((unsigned char)c) == 0;
        }
        return QuantumValue(r);
    }
    if (m == "toUpperCase" || m == "upper" || m == "upcase")
    {
        std::string r = str;
        std::transform(r.begin(), r.end(), r.begin(), ::toupper);
        return QuantumValue(r);
    }
    if (m == "toLowerCase" || m == "lower" || m == "downcase")
    {
        std::string r = str;
        std::transform(r.begin(), r.end(), r.begin(), ::tolower);
        return QuantumValue(r);
    }
    if (m == "trim" || m == "strip")
    {
        std::string r = str;
        while (!r.empty() && std::isspace((unsigned char)r.front()))
            r.erase(r.begin());
        while (!r.empty() && std::isspace((unsigned char)r.back()))
            r.pop_back();
        return QuantumValue(r);
    }
    // Ruby String#chomp — strips one trailing line ending only (not all whitespace).
    if (m == "chomp")
    {
        std::string r = str;
        if (!r.empty() && r.back() == '\n')
            r.pop_back();
        if (!r.empty() && r.back() == '\r')
            r.pop_back();
        return QuantumValue(r);
    }
    // Ruby String#reverse
    if (m == "reverse")
        return QuantumValue(std::string(str.rbegin(), str.rend()));
    // Ruby String#empty? / #chars / #each_char
    if (m == "empty")
        return QuantumValue(str.empty());
    if (m == "chars")
    {
        auto arr = std::make_shared<Array>();
        for (char c : str)
            arr->push_back(QuantumValue(std::string(1, c)));
        return QuantumValue(arr);
    }
    if (m == "each_char")
    {
        // The callback is invoked through the same dispatch the array
        // higher-order methods use, by delegating to a chars array.
        auto arr = std::make_shared<Array>();
        for (char c : str)
            arr->push_back(QuantumValue(std::string(1, c)));
        return callArrayMethod(arr, "each", args);
    }
    // Ruby String#to_i / #to_f / #to_s — parse a leading numeric prefix.
    if (m == "to_i")
    {
        try
        {
            return QuantumValue((double)std::stoll(str));
        }
        catch (...)
        {
            return QuantumValue(0.0);
        }
    }
    if (m == "to_f")
    {
        try
        {
            return QuantumValue(std::stod(str));
        }
        catch (...)
        {
            return QuantumValue(0.0);
        }
    }
    if (m == "to_s")
        return QuantumValue(str);
    // Ruby Exception#message — the raised value here is a plain
    // "TypeName: description" string; strip the leading type-name prefix
    // so e.message reads like Ruby's message text.
    if (m == "message")
    {
        std::smatch mm;
        static const std::regex prefixRe("^[A-Za-z_][A-Za-z0-9_]*: (.*)$");
        if (std::regex_match(str, mm, prefixRe))
            return QuantumValue(mm[1].str());
        return QuantumValue(str);
    }
    if (m == "startsWith" || m == "startswith")
    {
        if (args.empty())
            return QuantumValue(false);
        return QuantumValue(str.substr(0, std::min(str.size(), args[0].toString().size())) == args[0].toString());
    }
    if (m == "endsWith" || m == "endswith")
    {
        if (args.empty())
            return QuantumValue(false);
        std::string s = args[0].toString();
        return QuantumValue(str.size() >= s.size() && str.substr(str.size() - s.size()) == s);
    }
    if (m == "includes" || m == "contains" || m == "include")
    {
        if (args.empty())
            return QuantumValue(false);
        return QuantumValue(str.find(args[0].toString()) != std::string::npos);
    }
    if (m == "indexOf" || m == "index")
    {
        if (args.empty())
            return QuantumValue(-1.0);
        auto pos = str.find(args[0].toString());
        return QuantumValue(pos == std::string::npos ? -1.0 : (double)pos);
    }
    if (m == "split")
    {
        std::string sep = args.empty() ? "" : (args[0].isNil() ? "" : args[0].toString());
        auto arr = std::make_shared<Array>();
        if (sep.empty())
        {
            for (char c : str)
                arr->push_back(QuantumValue(std::string(1, c)));
        }
        else if (sep.size() >= 2 && sep.front() == '/' && sep.find_last_of('/') > 0)
        {
            size_t lastSlash = sep.find_last_of('/');
            std::string pattern = sep.substr(1, lastSlash - 1);
            std::regex::flag_type flags = std::regex::ECMAScript;
            if (sep.substr(lastSlash + 1).find('i') != std::string::npos)
                flags |= std::regex::icase;
            try
            {
                std::regex re(pattern, flags);
                std::sregex_token_iterator it(str.begin(), str.end(), re, -1), end;
                for (; it != end; ++it)
                    arr->push_back(QuantumValue(it->str()));
            }
            catch (const std::regex_error &)
            {
                arr->push_back(QuantumValue(str));
            }
        }
        else
        {
            size_t p = 0, f;
            while ((f = str.find(sep, p)) != std::string::npos)
            {
                arr->push_back(QuantumValue(str.substr(p, f - p)));
                p = f + sep.size();
            }
            arr->push_back(QuantumValue(str.substr(p)));
        }
        return QuantumValue(arr);
    }
    if (m == "join")
    {
        if (args.empty())
            return QuantumValue(str);
        if (args[0].isArray())
        {
            std::string out;
            auto arr = args[0].asArray();
            for (size_t i = 0; i < arr->size(); ++i)
            {
                if (i)
                    out += str;
                out += (*arr)[i].toString();
            }
            return QuantumValue(out);
        }
        return QuantumValue(args[0].toString());
    }
    if (m == "replace")
    {
        if (args.size() < 2)
            return QuantumValue(str);
        std::string s = str, from = args[0].toString(), to = args[1].toString();
        size_t p = s.find(from);
        if (p != std::string::npos)
            s = s.substr(0, p) + to + s.substr(p + from.size());
        return QuantumValue(s);
    }
    if (m == "replaceAll")
    {
        if (args.size() < 2)
            return QuantumValue(str);
        std::string s = str, from = args[0].toString(), to = args[1].toString();
        size_t p = 0;
        while ((p = s.find(from, p)) != std::string::npos)
        {
            s = s.substr(0, p) + to + s.substr(p + from.size());
            p += to.size();
        }
        return QuantumValue(s);
    }
    // Ruby String#gsub / #sub — replace all/first matches. The pattern arg
    // follows the same "/regex/flags" convention already used by split/test;
    // a plain string argument falls back to literal substring replacement.
    if (m == "gsub" || m == "sub")
    {
        if (args.size() < 2)
            return QuantumValue(str);
        std::string pat = args[0].toString(), to = args[1].toString();
        bool global = (m == "gsub");
        if (pat.size() >= 2 && pat.front() == '/' && pat.find_last_of('/') > 0)
        {
            size_t lastSlash = pat.find_last_of('/');
            std::string pattern = pat.substr(1, lastSlash - 1);
            std::string flags = pat.substr(lastSlash + 1);
            std::regex::flag_type regexFlags = std::regex::ECMAScript;
            if (flags.find('i') != std::string::npos)
                regexFlags |= std::regex::icase;
            try
            {
                std::regex re(pattern, regexFlags);
                return QuantumValue(std::regex_replace(str, re, to,
                    global ? std::regex_constants::format_default
                           : std::regex_constants::format_first_only));
            }
            catch (const std::regex_error &)
            {
                return QuantumValue(str);
            }
        }
        // Literal substring replacement
        std::string s = str;
        size_t p = 0;
        while ((p = s.find(pat, p)) != std::string::npos)
        {
            s = s.substr(0, p) + to + s.substr(p + pat.size());
            p += to.size();
            if (!global)
                break;
        }
        return QuantumValue(s);
    }
    if (m == "substring" || m == "substr")
    {
        int start = args.empty() ? 0 : (int)args[0].asNumber();
        int len2 = args.size() > 1 ? (int)args[1].asNumber() : (int)str.size() - start;
        if (start < 0)
            start = 0;
        return QuantumValue(str.substr(std::min((size_t)start, str.size()), std::max(0, len2)));
    }
    if (m == "slice")
    {
        int start = args.empty() ? 0 : (int)args[0].asNumber();
        int end = args.size() > 1 ? (int)args[1].asNumber() : (int)str.size();
        int n = static_cast<int>(str.size());
        if (start < 0)
            start += n;
        if (end < 0)
            end += n;
        start = std::max(0, std::min(start, n));
        end = std::max(0, std::min(end, n));
        if (end < start)
            end = start;
        return QuantumValue(str.substr(start, end - start));
    }
    if (m == "charAt")
    {
        if (args.empty())
            return QuantumValue(std::string(""));
        int i = (int)args[0].asNumber();
        if (i < 0 || i >= (int)str.size())
            return QuantumValue(std::string(""));
        return QuantumValue(std::string(1, str[i]));
    }
    if (m == "charCodeAt")
    {
        int i = args.empty() ? 0 : (int)args[0].asNumber();
        if (i < 0 || i >= (int)str.size())
            return QuantumValue(std::numeric_limits<double>::quiet_NaN());
        return QuantumValue((double)(unsigned char)str[i]);
    }
    if (m == "repeat")
    {
        int n = args.empty() ? 0 : (int)args[0].asNumber();
        std::string r;
        for (int i = 0; i < n; i++)
            r += str;
        return QuantumValue(r);
    }
    if (m == "padStart" || m == "rjust")
    {
        int n = args.empty() ? 0 : (int)args[0].asNumber();
        std::string p = args.size() > 1 ? args[1].toString() : " ";
        std::string r = str;
        while ((int)r.size() < n)
            r = p + r;
        return QuantumValue(r.substr(r.size() - std::max((size_t)n, str.size())));
    }
    if (m == "padEnd" || m == "ljust")
    {
        int n = args.empty() ? 0 : (int)args[0].asNumber();
        std::string p = args.size() > 1 ? args[1].toString() : " ";
        std::string r = str;
        while ((int)r.size() < n)
            r += p;
        return QuantumValue(r.substr(0, std::max((size_t)n, str.size())));
    }
    if (m == "isdigit")
    {
        for (char c : str)
            if (!std::isdigit((unsigned char)c))
                return QuantumValue(false);
        return QuantumValue(!str.empty());
    }
    if (m == "isalpha")
    {
        for (char c : str)
            if (!std::isalpha((unsigned char)c))
                return QuantumValue(false);
        return QuantumValue(!str.empty());
    }
    if (m == "isupper")
    {
        for (char c : str)
            if (std::isalpha((unsigned char)c) && !std::isupper((unsigned char)c))
                return QuantumValue(false);
        return QuantumValue(!str.empty());
    }
    if (m == "islower")
    {
        for (char c : str)
            if (std::isalpha((unsigned char)c) && !std::islower((unsigned char)c))
                return QuantumValue(false);
        return QuantumValue(!str.empty());
    }
    if (m == "format")
    {
        // Simple format: replace {} placeholders
        std::string result = str;
        size_t idx = 0;
        size_t p;
        while ((p = result.find("{}")) != std::string::npos && idx < args.size())
        {
            result = result.substr(0, p) + args[idx++].toString() + result.substr(p + 2);
        }
        return QuantumValue(result);
    }
    if (m == "translate")
    {
        if (args.empty() || !args[0].isDict())
            return QuantumValue(str);
        std::string out;
        auto table = args[0].asDict();
        for (char c : str)
        {
            auto it = table->find(std::to_string((int)(unsigned char)c));
            if (it != table->end())
                out += it->second.toString();
            else
                out += c;
        }
        return QuantumValue(out);
    }
    if (m == "test")
    {
        if (args.empty())
            return QuantumValue(false);
        if (str.size() >= 2 && str.front() == '/')
        {
            size_t lastSlash = str.find_last_of('/');
            if (lastSlash != 0 && lastSlash != std::string::npos)
            {
                std::string pattern = str.substr(1, lastSlash - 1);
                std::string flags = str.substr(lastSlash + 1);
                std::regex::flag_type regexFlags = std::regex::ECMAScript;
                if (flags.find('i') != std::string::npos)
                    regexFlags |= std::regex::icase;
                try
                {
                    return QuantumValue(std::regex_search(args[0].toString(), std::regex(pattern, regexFlags)));
                }
                catch (const std::regex_error &)
                {
                    return QuantumValue(args[0].toString().find(pattern) != std::string::npos);
                }
            }
        }
        return QuantumValue(args[0].toString().find(str) != std::string::npos);
    }
    if (m == "count")
    {
        if (args.empty())
            return QuantumValue((double)str.size());
        std::string sub = args[0].toString();
        if (sub.empty())
            return QuantumValue((double)str.size());
        int cnt = 0;
        size_t p = 0;
        while ((p = str.find(sub, p)) != std::string::npos)
        {
            cnt++;
            p += sub.size();
        }
        return QuantumValue((double)cnt);
    }
    throw TypeError("String has no method '" + m + "'");
}

// ─── Dict methods ─────────────────────────────────────────────────────────────

